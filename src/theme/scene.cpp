// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/scene.hpp>
#include <headunit/theme/video_stream.hpp>
#include "circular_path.hpp"
#include "continuous_corners.hpp"
#include <cairo.h>
#include <png.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace headunit::theme::graphics {
namespace {
constexpr double pi = std::numbers::pi;
void check(bool ok, const char* reason) {
  if (!ok) throw std::invalid_argument(reason);
}
void scalar(double v) {
  check(std::isfinite(v) && std::abs(v) <= 1e7, "invalid drawing coordinate");
}
void point(Point p) {
  scalar(p.x);
  scalar(p.y);
}
void rectangle(Rect r) {
  point(r.origin);
  scalar(r.size.width);
  scalar(r.size.height);
  check(r.size.width >= 0 && r.size.height >= 0, "negative drawing size");
}
void color(cairo_t* cr, Color c) {
  for (const auto v : {c.r, c.g, c.b, c.a})
    check(std::isfinite(v) && v >= 0 && v <= 1, "invalid drawing color");
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}
struct SurfaceDelete {
  void operator()(cairo_surface_t* p) const { cairo_surface_destroy(p); }
};
struct ContextDelete {
  void operator()(cairo_t* p) const { cairo_destroy(p); }
};
using Surface = std::unique_ptr<cairo_surface_t, SurfaceDelete>;
using Context = std::unique_ptr<cairo_t, ContextDelete>;
Surface surface(Image& image) {
  check(image.width && image.height && image.width <= 8192 && image.height <= 8192 &&
            std::uint64_t(image.width) * image.height <= 32 * 1024 * 1024 &&
            image.pixels.size() == std::size_t(image.width) * image.height,
        "invalid image dimensions");
  Surface s(cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(image.pixels.data()), CAIRO_FORMAT_ARGB32, image.width,
      image.height, image.width * 4));
  check(cairo_surface_status(s.get()) == CAIRO_STATUS_SUCCESS, "cannot create drawing surface");
  return s;
}
void append_path(cairo_t*, const Path&);
void rounded(cairo_t* cr, Rect r, double radius, bool continuous) {
  scalar(radius);
  check(radius >= 0, "negative corner radius");
  if (continuous) {
    // Bound flattening error to one hundredth of a pixel at the corner joins.
    cairo_set_tolerance(cr, std::min(cairo_get_tolerance(cr), .01));
    append_path(cr, continuous_rectangle(r, radius));
    return;
  }
  const auto x = r.origin.x, y = r.origin.y, w = r.size.width, h = r.size.height;
  const double c = std::clamp(radius, 0.0, std::min(w, h) / 2);
  cairo_new_sub_path(cr);
  if (c == 0) {
    cairo_rectangle(cr, x, y, w, h);
    return;
  }
  cairo_arc(cr, x + w - c, y + c, c, -pi / 2, 0);
  cairo_arc(cr, x + w - c, y + h - c, c, 0, pi / 2);
  cairo_arc(cr, x + c, y + h - c, c, pi / 2, pi);
  cairo_arc(cr, x + c, y + c, c, pi, 3 * pi / 2);
  cairo_close_path(cr);
}
void append_path(cairo_t* cr, const Path& path) {
  for (const auto& s : path.segments) {
    switch (s.type) {
      case SegmentType::move:
        cairo_move_to(cr, s.p.x, s.p.y);
        break;
      case SegmentType::line:
        cairo_line_to(cr, s.p.x, s.p.y);
        break;
      case SegmentType::quad: {
        double x = 0, y = 0;
        cairo_get_current_point(cr, &x, &y);
        cairo_curve_to(cr, x + 2 * (s.c1.x - x) / 3, y + 2 * (s.c1.y - y) / 3,
                       s.p.x + 2 * (s.c1.x - s.p.x) / 3, s.p.y + 2 * (s.c1.y - s.p.y) / 3, s.p.x,
                       s.p.y);
        break;
      }
      case SegmentType::cubic:
        cairo_curve_to(cr, s.c1.x, s.c1.y, s.c2.x, s.c2.y, s.p.x, s.p.y);
        break;
      case SegmentType::arc:
        if (s.clockwise)
          cairo_arc(cr, s.p.x, s.p.y, s.radius, s.start, s.end);
        else
          cairo_arc_negative(cr, s.p.x, s.p.y, s.radius, s.start, s.end);
        break;
      case SegmentType::ellipse:
        if (s.c1.x > 0 && s.c1.y > 0) {
          cairo_save(cr);
          cairo_translate(cr, s.p.x + s.c1.x / 2, s.p.y + s.c1.y / 2);
          cairo_scale(cr, s.c1.x / 2, s.c1.y / 2);
          cairo_new_sub_path(cr);
          cairo_arc(cr, 0, 0, 1, 0, 2 * pi);
          cairo_close_path(cr);
          cairo_restore(cr);
        }
        break;
      case SegmentType::close:
        cairo_close_path(cr);
        break;
    }
  }
}
void trim_path(cairo_t* cr, const Path& path, double begin, double end) {
  scalar(begin);
  scalar(end);
  // The reference asset deliberately subtracts epsilon at a zero-speed stroke
  // boundary. Intersect finite requested intervals with the available path.
  begin = std::clamp(begin, 0.0, 1.0);
  end = std::clamp(end, 0.0, 1.0);
  if (begin == 0 && end == 1) return;
  if (const auto circle = circular_path(path)) {
    // Flattening a wide stroke before trimming leaves short straight sections
    // along its inner/outer edges and rotates its cap to the final chord.
    // Circular distance is exactly radius * angle: retain a real curved path.
    cairo_new_path(cr);
    if (end <= begin || circle->sweep == 0) return;
    const auto start = circle->start + circle->sweep * begin;
    const auto finish = circle->start + circle->sweep * end;
    if (circle->sweep > 0)
      cairo_arc(cr, circle->center.x, circle->center.y, circle->radius, start, finish);
    else
      cairo_arc_negative(cr, circle->center.x, circle->center.y, circle->radius, start, finish);
    return;
  }
  std::unique_ptr<cairo_path_t, decltype(&cairo_path_destroy)> flat(cairo_copy_path_flat(cr),
                                                                    cairo_path_destroy);
  check(flat->status == CAIRO_STATUS_SUCCESS, "cannot flatten stroke");
  struct Line {
    Point a, b;
    bool restart;
  };
  std::vector<Line> lines;
  Point current{}, start{};
  bool restart = true;
  double length = 0;
  for (int i = 0; i < flat->num_data;) {
    const auto* data = &flat->data[i];
    if (data->header.type == CAIRO_PATH_MOVE_TO) {
      current = start = {data[1].point.x, data[1].point.y};
      restart = true;
    } else if (data->header.type == CAIRO_PATH_LINE_TO ||
               data->header.type == CAIRO_PATH_CLOSE_PATH) {
      const Point to = data->header.type == CAIRO_PATH_CLOSE_PATH
                           ? start
                           : Point{data[1].point.x, data[1].point.y};
      lines.push_back({current, to, restart});
      length += std::hypot(to.x - current.x, to.y - current.y);
      current = to;
      restart = false;
    }
    i += data->header.length;
    check(lines.size() <= 262144, "flattened stroke too large");
  }
  cairo_new_path(cr);
  if (end <= begin || length == 0) return;
  double distance = 0;
  bool drawing = false;
  for (const auto& line : lines) {
    const auto n = std::hypot(line.b.x - line.a.x, line.b.y - line.a.y);
    const auto lo = std::max(begin * length - distance, 0.0),
               hi = std::min(end * length - distance, n);
    if (n > 0 && lo < hi) {
      const Point a{line.a.x + (line.b.x - line.a.x) * lo / n,
                    line.a.y + (line.b.y - line.a.y) * lo / n};
      const Point b{line.a.x + (line.b.x - line.a.x) * hi / n,
                    line.a.y + (line.b.y - line.a.y) * hi / n};
      if (!drawing || line.restart || lo > 0) cairo_move_to(cr, a.x, a.y);
      cairo_line_to(cr, b.x, b.y);
      drawing = true;
    } else
      drawing = false;
    distance += n;
  }
}
bool circular_stroke(cairo_t* cr, const Layer& layer) {
  const auto circle = circular_path(*layer.path);
  const auto half = layer.line_width / 2;
  if (!circle || half >= circle->radius) return false;
  scalar(layer.stroke_start);
  scalar(layer.stroke_end);
  const auto begin = std::clamp(layer.stroke_start, 0., 1.),
             end = std::clamp(layer.stroke_end, 0., 1.);
  cairo_new_path(cr);
  if (end <= begin || circle->sweep == 0) return true;
  const auto a = circle->start + circle->sweep * begin, b = circle->start + circle->sweep * end;
  const auto lo = std::min(a, b), hi = std::max(a, b);
  const auto x = circle->center.x, y = circle->center.y, r = circle->radius;
  // Construct both radial edges independently. Stroking an approximated
  // centreline magnifies its curvature error on wide ribbons near their ends.
  // Cap extensions remain separate and preserve the native Cap semantics.
  cairo_save(cr);
  cairo_set_tolerance(cr, .01);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
  cairo_arc(cr, x, y, r + half, lo, hi);
  cairo_arc_negative(cr, x, y, r - half, hi, lo);
  cairo_close_path(cr);
  if (!(circle->closed && begin == 0 && end == 1))
    for (const auto angle : {lo, hi}) {
      const auto px = x + r * std::cos(angle), py = y + r * std::sin(angle);
      if (layer.cap == Cap::round) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, px, py, half, 0, 2 * pi);
        cairo_close_path(cr);
      } else if (layer.cap == Cap::square) {
        cairo_save(cr);
        cairo_translate(cr, px, py);
        cairo_rotate(cr, angle);
        cairo_rectangle(cr, -half, angle == lo ? -half : 0, 2 * half, half);
        cairo_restore(cr);
      }
    }
  cairo_fill(cr);
  cairo_restore(cr);
  return true;
}
cairo_operator_t blend(Blend value) {
  switch (value) {
    case Blend::source_over:
      return CAIRO_OPERATOR_OVER;
    case Blend::copy:
      return CAIRO_OPERATOR_SOURCE;
    case Blend::clear:
      return CAIRO_OPERATOR_CLEAR;
    case Blend::source_in:
      return CAIRO_OPERATOR_IN;
    case Blend::source_out:
      return CAIRO_OPERATOR_OUT;
    case Blend::source_atop:
      return CAIRO_OPERATOR_ATOP;
    case Blend::dest_over:
      return CAIRO_OPERATOR_DEST_OVER;
    case Blend::dest_in:
      return CAIRO_OPERATOR_DEST_IN;
    case Blend::dest_out:
      return CAIRO_OPERATOR_DEST_OUT;
    case Blend::dest_atop:
      return CAIRO_OPERATOR_DEST_ATOP;
    case Blend::add:
      return CAIRO_OPERATOR_ADD;
    case Blend::xor_:
      return CAIRO_OPERATOR_XOR;
    case Blend::screen:
      return CAIRO_OPERATOR_SCREEN;
    case Blend::exclusion:
      return CAIRO_OPERATOR_EXCLUSION;
    case Blend::maximum:
      return CAIRO_OPERATOR_LIGHTEN;
    case Blend::minimum:
      return CAIRO_OPERATOR_DARKEN;
    default:
      throw std::invalid_argument("blend operation not implemented");
  }
}
void conical_gradient(cairo_t* cr, const Layer& layer, double alpha, std::size_t bytes,
                      std::size_t& gradient_pixels) {
  const auto r = layer.bounds;
  if (!r.size.width || !r.size.height) return;
  point(layer.gradient_start);
  point(layer.gradient_end);
  const Point center{r.origin.x + layer.gradient_start.x * r.size.width,
                     r.origin.y + layer.gradient_start.y * r.size.height};
  const Point end{r.origin.x + layer.gradient_end.x * r.size.width,
                  r.origin.y + layer.gradient_end.y * r.size.height};
  check(std::hypot(end.x - center.x, end.y - center.y) > 1e-12,
        "degenerate conical gradient direction");
  double previous = -1;
  for (const auto& stop : layer.stops) {
    check(std::isfinite(stop.location) && stop.location >= 0 && stop.location <= 1 &&
              stop.location >= previous,
          "invalid conical stops");
    for (const auto v : {stop.color.r, stop.color.g, stop.color.b, stop.color.a})
      check(std::isfinite(v) && v >= 0 && v <= 1, "invalid conical color");
    previous = stop.location;
  }
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);
  auto inverse = matrix;
  check(cairo_matrix_invert(&inverse) == CAIRO_STATUS_SUCCESS, "invalid conical transform");
  std::array<Point, 4> corners{{r.origin,
                                {r.origin.x + r.size.width, r.origin.y},
                                {r.origin.x, r.origin.y + r.size.height},
                                {r.origin.x + r.size.width, r.origin.y + r.size.height}}};
  for (auto& p : corners)
    cairo_matrix_transform_point(&matrix, &p.x, &p.y);
  auto lo = corners[0], hi = lo;
  for (const auto p : corners) {
    lo.x = std::min(lo.x, p.x);
    lo.y = std::min(lo.y, p.y);
    hi.x = std::max(hi.x, p.x);
    hi.y = std::max(hi.y, p.y);
  }
  const auto target = cairo_get_target(cr);
  const auto width = cairo_image_surface_get_width(target),
             height = cairo_image_surface_get_height(target);
  const auto x0 = int(std::clamp(std::floor(lo.x), 0.0, double(width))),
             y0 = int(std::clamp(std::floor(lo.y), 0.0, double(height)));
  const auto x1 = int(std::clamp(std::ceil(hi.x), 0.0, double(width))),
             y1 = int(std::clamp(std::ceil(hi.y), 0.0, double(height)));
  if (x1 <= x0 || y1 <= y0) return;
  const auto pixels = std::size_t(x1 - x0) * (y1 - y0);
  check(pixels * 4 <= 128 * 1024 * 1024 - bytes, "conical gradient surface budget exceeded");
  check(pixels <= 32 * 1024 * 1024 - gradient_pixels, "conical gradient work budget exceeded");
  gradient_pixels += pixels;
  Image image{std::uint32_t(x1 - x0), std::uint32_t(y1 - y0), std::vector<std::uint32_t>(pixels)};
  const auto zero = std::atan2(end.y - center.y, end.x - center.x);
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      double lx = x + .5, ly = y + .5;
      cairo_matrix_transform_point(&inverse, &lx, &ly);
      // Asset GradientLayoutUtils maps clockwise gauge progress to decreasing
      // stops; the native sweep therefore increases counterclockwise in y-down.
      double position = (zero - std::atan2(ly - center.y, lx - center.x)) / (2 * pi);
      position -= std::floor(position);
      const auto next =
          std::upper_bound(layer.stops.begin(), layer.stops.end(), position,
                           [](double v, const Stop& stop) { return v < stop.location; });
      const auto& a = next == layer.stops.begin() ? layer.stops.front() : *(next - 1);
      const auto& b = next == layer.stops.end() ? layer.stops.back() : *next;
      const auto t =
          a.location == b.location
              ? 0.0
              : std::clamp((position - a.location) / (b.location - a.location), 0.0, 1.0);
      auto mix = [&](double v, double w) {
        return std::uint32_t(std::lround(255 * (v + (w - v) * t)));
      };
      const auto ac = a.color, bc = b.color;
      image.pixels[std::size_t(y - y0) * image.width + x - x0] =
          (mix(ac.a, bc.a) << 24) | (mix(ac.r * ac.a, bc.r * bc.a) << 16) |
          (mix(ac.g * ac.a, bc.g * bc.a) << 8) | mix(ac.b * ac.a, bc.b * bc.a);
    }
  auto source = surface(image);
  cairo_save(cr);
  rounded(cr, r, layer.corner_radius, layer.continuous_corners);
  cairo_clip(cr);
  cairo_identity_matrix(cr);
  cairo_set_source_surface(cr, source.get(), x0, y0);
  cairo_paint_with_alpha(cr, alpha);
  cairo_restore(cr);
}
void own_content(cairo_t* cr, const Layer& layer, double alpha, std::size_t bytes,
                 std::size_t& gradient_pixels) {
  if (layer.background.a > 0) {
    rounded(cr, layer.bounds, layer.corner_radius, layer.continuous_corners);
    auto c = layer.background;
    c.a *= alpha;
    color(cr, c);
    cairo_fill(cr);
  }
  if (layer.kind == Kind::shape && layer.path) {
    cairo_new_path(cr);
    append_path(cr, *layer.path);
    cairo_set_antialias(cr, layer.antialiased ? CAIRO_ANTIALIAS_BEST : CAIRO_ANTIALIAS_NONE);
    cairo_set_fill_rule(cr, layer.fill_rule == FillRule::evenodd ? CAIRO_FILL_RULE_EVEN_ODD
                                                                 : CAIRO_FILL_RULE_WINDING);
    auto fill = layer.fill;
    fill.a *= alpha;
    color(cr, fill);
    cairo_fill_preserve(cr);
    if (layer.line_width > 0 && layer.stroke.a > 0) {
      auto stroke = layer.stroke;
      stroke.a *= alpha;
      color(cr, stroke);
      if (!circular_stroke(cr, layer)) {
        trim_path(cr, *layer.path, layer.stroke_start, layer.stroke_end);
        cairo_set_line_width(cr, layer.line_width);
        cairo_set_miter_limit(cr, layer.miter_limit);
        cairo_set_line_cap(cr, static_cast<cairo_line_cap_t>(layer.cap));
        cairo_set_line_join(cr, static_cast<cairo_line_join_t>(layer.join));
        cairo_stroke(cr);
      }
    } else
      cairo_new_path(cr);
  } else if (layer.kind == Kind::gradient && layer.gradient_kind == GradientKind::conical &&
             !layer.stops.empty()) {
    conical_gradient(cr, layer, alpha, bytes, gradient_pixels);
  } else if (layer.kind == Kind::gradient && !layer.stops.empty()) {
    const auto r = layer.bounds;
    const Point a{r.origin.x + layer.gradient_start.x * r.size.width,
                  r.origin.y + layer.gradient_start.y * r.size.height};
    const Point b{r.origin.x + layer.gradient_end.x * r.size.width,
                  r.origin.y + layer.gradient_end.y * r.size.height};
    std::unique_ptr<cairo_pattern_t, decltype(&cairo_pattern_destroy)> pattern(
        layer.gradient_kind == GradientKind::linear
            ? cairo_pattern_create_linear(a.x, a.y, b.x, b.y)
            : cairo_pattern_create_radial(a.x, a.y, 0, a.x, a.y, std::hypot(b.x - a.x, b.y - a.y)),
        cairo_pattern_destroy);
    for (const auto& stop : layer.stops) {
      const auto c = stop.color;
      cairo_pattern_add_color_stop_rgba(pattern.get(), stop.location, c.r, c.g, c.b, c.a * alpha);
    }
    check(cairo_pattern_status(pattern.get()) == CAIRO_STATUS_SUCCESS, "invalid gradient");
    cairo_pattern_set_extend(pattern.get(), CAIRO_EXTEND_PAD);
    cairo_set_source(cr, pattern.get());
    rounded(cr, r, layer.corner_radius, layer.continuous_corners);
    cairo_fill(cr);
  } else if ((layer.kind == Kind::image || layer.kind == Kind::video) &&
             layer.bounds.size.width > 0 && layer.bounds.size.height > 0) {
    const std::shared_ptr<const Image> content =
        layer.kind == Kind::video ? (layer.video ? layer.video->image() : nullptr) : layer.image;
    if (content) {
      const auto& image = *content;
      std::optional<Image> tinted;
      const auto tint = layer.tint;
      if (tint.r != 1 || tint.g != 1 || tint.b != 1 || tint.a != 1) {
        tinted = image;
        for (auto& px : tinted->pixels) {
          const auto a = std::uint32_t(std::lround((px >> 24) * tint.a));
          const auto r = std::uint32_t(std::lround(((px >> 16) & 255) * tint.r * tint.a));
          const auto g = std::uint32_t(std::lround(((px >> 8) & 255) * tint.g * tint.a));
          const auto b = std::uint32_t(std::lround((px & 255) * tint.b * tint.a));
          px = (a << 24) | (r << 16) | (g << 8) | b;
        }
      }
      auto source = surface(tinted ? *tinted : const_cast<Image&>(*content));
      cairo_save(cr);
      cairo_translate(cr, layer.bounds.origin.x, layer.bounds.origin.y);
      cairo_scale(cr, layer.bounds.size.width / image.width,
                  layer.bounds.size.height / image.height);
      cairo_rectangle(cr, 0, 0, image.width, image.height);
      cairo_clip(cr);
      cairo_set_source_surface(cr, source.get(), 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
      cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
      cairo_paint_with_alpha(cr, alpha);
      cairo_restore(cr);
    }
  }
  if (layer.border_width > 0 && layer.border.a > 0) {
    const auto inset = std::min(layer.border_width / 2,
                                std::min(layer.bounds.size.width, layer.bounds.size.height) / 2);
    Rect r = layer.bounds;
    r.origin.x += inset;
    r.origin.y += inset;
    r.size.width -= 2 * inset;
    r.size.height -= 2 * inset;
    rounded(cr, r, std::max(0.0, layer.corner_radius - inset), layer.continuous_corners);
    cairo_set_line_width(cr, layer.border_width);
    auto c = layer.border;
    c.a *= alpha;
    color(cr, c);
    cairo_stroke(cr);
  }
}
struct RenderContext {
  std::size_t count = 0, bytes = 0, frame_bytes = 0, gradient_pixels = 0;
  std::uint64_t blur_work = 0;
  std::vector<cairo_surface_t*> ancestors;
  bool drawing_mask = false;
  RenderCosts* costs = nullptr;
};
using CostClock = std::chrono::steady_clock;
std::uint64_t elapsed_ns(CostClock::time_point start) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(CostClock::now() - start).count();
}
struct Backdrop {
  Image image;
  int x = 0, y = 0;
};
std::vector<double> gaussian(double sigma) {
  check(std::isfinite(sigma) && sigma >= 0 && sigma <= 128,
        "blur radius exceeds reference renderer limit");
  if (sigma < 1e-6) return {1};
  const auto extent = int(std::ceil(3 * sigma));
  std::vector<double> weights(2 * extent + 1);
  double total = 0;
  for (int k = -extent; k <= extent; ++k) {
    const double x = k / sigma;
    total += (weights[k + extent] = std::exp(-.5 * x * x));
  }
  for (auto& weight : weights)
    weight /= total;
  return weights;
}
void blur_axis(const Image& input, Image& output, const std::vector<double>& weights,
               bool horizontal, const BlurFilter& filter) {
  const auto extent = int(weights.size() / 2), width = int(input.width), height = int(input.height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      std::array<double, 4> channels{};
      double normalization = 0;
      for (int k = -extent; k <= extent; ++k) {
        auto sx = horizontal ? x + k : x, sy = horizontal ? y : y + k;
        if (filter.hard_edges) {
          sx = std::clamp(sx, 0, width - 1);
          sy = std::clamp(sy, 0, height - 1);
        }
        if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
        const auto weight = weights[k + extent];
        normalization += weight;
        const auto px = input.pixels[std::size_t(sy) * width + sx];
        for (unsigned channel = 0; channel < 4; ++channel)
          channels[channel] += ((px >> (channel * 8)) & 255) * weight;
      }
      const auto divisor = filter.normalized_edges ? normalization : 1.0;
      std::uint32_t pixel = 0;
      for (unsigned channel = 0; channel < 4; ++channel)
        pixel |= std::uint32_t(std::clamp(std::lround(channels[channel] / divisor), 0L, 255L))
                 << (channel * 8);
      output.pixels[std::size_t(y) * width + x] = pixel;
    }
}
std::optional<Backdrop> capture_backdrop(cairo_t* cr, const Layer& layer, RenderContext& context) {
  check(!context.drawing_mask, "backdrop sampling inside mask layers is not supported");
  std::vector<std::pair<std::vector<double>, std::vector<double>>> kernels;
  std::vector<const BlurFilter*> filters;
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);
  // A separable screen-space convolution is exact for transforms whose rows
  // are orthogonal. Rotated anisotropic scale/shear requires a different kernel.
  const auto sx = std::hypot(matrix.xx, matrix.xy), sy = std::hypot(matrix.yx, matrix.yy);
  check(std::abs(matrix.xx * matrix.yx + matrix.xy * matrix.yy) <= 1e-8 * std::max(1.0, sx * sy),
        "nonseparable backdrop transform is not implemented");
  int padx = 0, pady = 0;
  for (const auto& filter : layer.filters) {
    check(bool(filter), "null blur filter");
    check(std::isfinite(filter->radius) && filter->radius >= 0 && filter->radius <= 128,
          "invalid blur radius");
    if (!filter->enabled || filter->radius == 0) continue;
    auto x = gaussian(filter->radius * sx), y = gaussian(filter->radius * sy);
    padx += int(x.size() / 2);
    pady += int(y.size() / 2);
    kernels.emplace_back(std::move(x), std::move(y));
    filters.push_back(filter.get());
  }
  if (filters.empty() || !layer.bounds.size.width || !layer.bounds.size.height) return {};
  const auto r = layer.bounds;
  std::array<Point, 4> corners{{r.origin,
                                {r.origin.x + r.size.width, r.origin.y},
                                {r.origin.x, r.origin.y + r.size.height},
                                {r.origin.x + r.size.width, r.origin.y + r.size.height}}};
  for (auto& p : corners)
    cairo_matrix_transform_point(&matrix, &p.x, &p.y);
  auto lo = corners[0], hi = lo;
  for (const auto p : corners) {
    lo.x = std::min(lo.x, p.x);
    lo.y = std::min(lo.y, p.y);
    hi.x = std::max(hi.x, p.x);
    hi.y = std::max(hi.y, p.y);
  }
  const auto target = cairo_get_target(cr);
  check(cairo_surface_get_type(target) == CAIRO_SURFACE_TYPE_IMAGE,
        "backdrop requires an image target");
  const auto width = cairo_image_surface_get_width(target),
             height = cairo_image_surface_get_height(target);
  const auto x0 = int(std::clamp(std::floor(lo.x) - padx, 0.0, double(width)));
  const auto y0 = int(std::clamp(std::floor(lo.y) - pady, 0.0, double(height)));
  const auto x1 = int(std::clamp(std::ceil(hi.x) + padx, 0.0, double(width)));
  const auto y1 = int(std::clamp(std::ceil(hi.y) + pady, 0.0, double(height)));
  if (x1 <= x0 || y1 <= y0) return {};
  const auto pixels = std::size_t(x1 - x0) * (y1 - y0), bytes = pixels * 4;
  check(bytes * 2 <= 128 * 1024 * 1024 - context.bytes, "backdrop surface budget exceeded");
  constexpr std::uint64_t max_work = 1000000000;
  for (const auto& [x, y] : kernels) {
    const auto work = std::uint64_t(pixels) * (x.size() + y.size());
    check(work <= max_work - context.blur_work, "backdrop convolution work budget exceeded");
    context.blur_work += work;
  }
  Backdrop result{
      {std::uint32_t(x1 - x0), std::uint32_t(y1 - y0), std::vector<std::uint32_t>(pixels)}, x0, y0};
  {
    auto canvas = surface(result.image);
    Context capture(cairo_create(canvas.get()));
    cairo_translate(capture.get(), -x0, -y0);
    auto append = [&](cairo_surface_t* source) {
      cairo_surface_flush(source);
      cairo_set_source_surface(capture.get(), source, 0, 0);
      cairo_paint(capture.get());
    };
    for (auto* ancestor : context.ancestors)
      append(ancestor);
    append(cairo_get_group_target(cr));
    cairo_surface_flush(canvas.get());
    check(cairo_status(capture.get()) == CAIRO_STATUS_SUCCESS, "backdrop capture failed");
  }
  Image temporary{result.image.width, result.image.height, std::vector<std::uint32_t>(pixels)};
  for (std::size_t i = 0; i < filters.size(); ++i) {
    blur_axis(result.image, temporary, kernels[i].first, true, *filters[i]);
    blur_axis(temporary, result.image, kernels[i].second, false, *filters[i]);
  }
  context.bytes += bytes;
  return result;
}
void paint_backdrop(cairo_t* cr, const Layer& layer, Backdrop& backdrop, double alpha, bool group) {
  // An opaque scene composes correctly through existing isolated groups. A
  // translucent backdrop needs a replacement/coverage buffer through each
  // ancestor group, which this reference path does not yet provide.
  if (group)
    check(std::all_of(backdrop.image.pixels.begin(), backdrop.image.pixels.end(),
                      [](auto p) { return (p >> 24) == 255; }),
          "translucent backdrop inside compositing groups is not implemented");
  auto source = surface(backdrop.image);
  cairo_save(cr);
  rounded(cr, layer.bounds, layer.corner_radius, layer.continuous_corners);
  cairo_clip(cr);
  cairo_identity_matrix(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, source.get(), backdrop.x, backdrop.y);
  cairo_paint_with_alpha(cr, alpha);
  cairo_restore(cr);
}
void draw(cairo_t* cr, const Layer& layer, unsigned depth, RenderContext& context,
          double inherited = 1) {
  auto& count = context.count;
  auto& group_bytes = context.bytes;
  const auto frame_bytes = context.frame_bytes;
  check(depth <= 64 && ++count <= 4096, "scene exceeds rendering limits");
  check(std::isfinite(layer.opacity) && layer.opacity >= 0 && layer.opacity <= 1,
        "invalid layer opacity");
  if (layer.hidden || layer.opacity == 0) return;
  rectangle(layer.bounds);
  point(layer.position);
  point(layer.anchor);
  const auto& t = layer.transform;
  for (const auto v : {t.a, t.b, t.c, t.d, t.tx, t.ty})
    scalar(v);
  if (std::abs(t.a * t.d - t.b * t.c) < 1e-12) return;
  cairo_save(cr);
  cairo_translate(cr, layer.position.x, layer.position.y);
  cairo_matrix_t matrix{t.a, t.b, t.c, t.d, t.tx, t.ty};
  cairo_transform(cr, &matrix);
  cairo_translate(cr, -layer.bounds.origin.x - layer.anchor.x * layer.bounds.size.width,
                  -layer.bounds.origin.y - layer.anchor.y * layer.bounds.size.height);
  if (layer.clip) {
    rounded(cr, layer.bounds, layer.corner_radius, layer.continuous_corners);
    cairo_clip(cr);
  }
  const auto alpha = inherited * layer.opacity;
  const bool group = layer.needs_group(alpha);
  check(layer.filters.size() <= 8 && (layer.filters.empty() || layer.kind == Kind::backdrop),
        "unsupported filter attachment");
  const auto backdrop_start = context.costs ? CostClock::now() : CostClock::time_point{};
  auto backdrop =
      layer.kind == Kind::backdrop ? capture_backdrop(cr, layer, context) : std::nullopt;
  if (context.costs && layer.kind == Kind::backdrop)
    context.costs->backdrop_ns += elapsed_ns(backdrop_start);
  if (group) {
    check(frame_bytes <= 128 * 1024 * 1024 - group_bytes, "compositing surface budget exceeded");
    group_bytes += frame_bytes;
    context.ancestors.push_back(cairo_get_group_target(cr));
    cairo_push_group(cr);
  }
  cairo_set_operator(cr, group ? CAIRO_OPERATOR_OVER : blend(layer.blend));
  const auto local_alpha = group && layer.group_blending ? 1 : alpha;
  if (backdrop) {
    check(layer.blend == Blend::source_over, "nonstandard backdrop blending is not implemented");
    paint_backdrop(cr, layer, *backdrop, local_alpha, group || !context.ancestors.empty());
    group_bytes -= backdrop->image.pixels.size() * 4;
    backdrop.reset();
  }
  const auto content_start = context.costs ? CostClock::now() : CostClock::time_point{};
  own_content(cr, layer, local_alpha, context.bytes, context.gradient_pixels);
  if (context.costs) context.costs->content_ns += elapsed_ns(content_start);
  for (const auto& child : layer.children)
    draw(cr, *child, depth + 1, context, local_alpha);
  if (layer.mask) {
    check(2 * frame_bytes <= 128 * 1024 * 1024 - group_bytes, "mask surface budget exceeded");
    group_bytes += 2 * frame_bytes;
    std::unique_ptr<cairo_pattern_t, decltype(&cairo_pattern_destroy)> content(
        cairo_pop_group(cr), cairo_pattern_destroy);
    const auto was_mask = context.drawing_mask;
    context.drawing_mask = true;
    cairo_push_group(cr);
    draw(cr, *layer.mask, depth + 1, context);
    context.drawing_mask = was_mask;
    std::unique_ptr<cairo_pattern_t, decltype(&cairo_pattern_destroy)> mask(cairo_pop_group(cr),
                                                                            cairo_pattern_destroy);
    cairo_push_group(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source(cr, content.get());
    cairo_mask(cr, mask.get());
    group_bytes -= 2 * frame_bytes;
  }
  if (group) {
    group_bytes -= frame_bytes;
    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, blend(layer.blend));
    cairo_paint_with_alpha(cr, layer.group_blending ? alpha : 1);
    context.ancestors.pop_back();
  }
  cairo_restore(cr);
  check(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "scene rendering failed");
}
}  // namespace
Point Transform::apply(Point p) const {
  return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty};
}
Transform Transform::multiply(const Transform& r) const {
  return {a * r.a + c * r.b, b * r.a + d * r.b,        a * r.c + c * r.d,
          b * r.c + d * r.d, a * r.tx + c * r.ty + tx, b * r.tx + d * r.ty + ty};
}
std::optional<Transform> Transform::inverse() const {
  const auto det = a * d - b * c;
  if (!std::isfinite(det) || std::abs(det) < 1e-12) return {};
  return Transform{
      d / det, -b / det, -c / det, a / det, (c * ty - d * tx) / det, (b * tx - a * ty) / det};
}
void Path::append(Segment s) {
  check(segments.size() < 65536, "path segment limit exceeded");
  point(s.p);
  point(s.c1);
  point(s.c2);
  scalar(s.radius);
  scalar(s.start);
  scalar(s.end);
  check(s.radius >= 0, "negative arc radius");
  check(s.type != SegmentType::ellipse || (s.c1.x >= 0 && s.c1.y >= 0), "negative ellipse size");
  segments.push_back(s);
  point_count += s.type == SegmentType::cubic   ? 3
                 : s.type == SegmentType::quad  ? 2
                 : s.type == SegmentType::close ? 0
                                                : 1;
}
bool Layer::needs_group(double alpha) const {
  if (mask) return true;
  if (!group_blending) return false;
  if (alpha < 1 || blend != Blend::source_over) return true;
  return std::any_of(children.begin(), children.end(), [](const auto& child) {
    if (!child || child->hidden || child->opacity == 0) return false;
    return child->blend == Blend::dest_out || child->blend == Blend::dest_in ||
           child->blend == Blend::clear || child->blend == Blend::copy;
  });
}
Rect Layer::frame() const {
  const auto w = bounds.size.width, h = bounds.size.height;
  const auto x = -anchor.x * w, y = -anchor.y * h;
  std::array<Point, 4> points{transform.apply({x, y}), transform.apply({x + w, y}),
                              transform.apply({x, y + h}), transform.apply({x + w, y + h})};
  auto min = points[0], max = min;
  for (const auto p : points) {
    min.x = std::min(min.x, p.x);
    min.y = std::min(min.y, p.y);
    max.x = std::max(max.x, p.x);
    max.y = std::max(max.y, p.y);
  }
  return {{position.x + min.x, position.y + min.y}, {max.x - min.x, max.y - min.y}};
}
void Layer::set_frame(Rect r) {
  rectangle(r);
  const auto& t = transform;
  for (const auto v : {t.a, t.b, t.c, t.d, t.tx, t.ty})
    scalar(v);
  // A path-only shape uses a zero-size frame to position its rotated origin.
  // This is unambiguous even though resizing a rotated bounding box may not be.
  if ((r.size.width == 0 && r.size.height == 0) || (t.a == 1 && t.b == 0 && t.c == 0 && t.d == 1)) {
    bounds.size = r.size;
    position = {r.origin.x + anchor.x * r.size.width - t.tx,
                r.origin.y + anchor.y * r.size.height - t.ty};
    return;
  }
  const auto current = frame();
  check(std::abs(current.size.width - r.size.width) < 1e-9 &&
            std::abs(current.size.height - r.size.height) < 1e-9,
        "resizing a transformed layer frame is ambiguous");
  position.x += r.origin.x - current.origin.x;
  position.y += r.origin.y - current.origin.y;
}
void Layer::detach() {
  if (auto p = parent.lock()) p->remove(shared_from_this());
}
void Layer::insert(std::shared_ptr<Layer> child, std::size_t index) {
  check(child && child.get() != this && index <= children.size(), "invalid layer insertion");
  unsigned depth = 0;
  for (auto p = shared_from_this(); p; p = p->parent.lock()) {
    check(p != child && ++depth <= 64, "cyclic or excessive layer hierarchy");
  }
  auto same = std::find(children.begin(), children.end(), child);
  if (same != children.end() && std::size_t(same - children.begin()) < index) --index;
  child->detach();
  children.insert(children.begin() + static_cast<std::ptrdiff_t>(index), child);
  child->parent = shared_from_this();
}
void Layer::remove(std::shared_ptr<Layer> child) {
  if (!child) return;
  if (mask == child) {
    mask->parent.reset();
    mask.reset();
    return;
  }
  const auto i = std::find(children.begin(), children.end(), child);
  if (i != children.end()) {
    (*i)->parent.reset();
    children.erase(i);
  }
}
void Layer::set_mask(std::shared_ptr<Layer> value) {
  if (value == mask) return;
  if (value) {
    unsigned depth = 0;
    for (auto p = shared_from_this(); p; p = p->parent.lock())
      check(p != value && ++depth <= 64, "cyclic mask hierarchy");
  }
  if (mask) mask->parent.reset();
  if (value) value->detach();
  mask = value;
  if (mask) mask->parent = shared_from_this();
}
Transform Layer::local_transform() const {
  return Transform{1, 0, 0, 1, position.x, position.y}.multiply(transform).multiply(
      {1, 0, 0, 1, -bounds.origin.x - anchor.x * bounds.size.width,
       -bounds.origin.y - anchor.y * bounds.size.height});
}
std::optional<Transform> Layer::transform_to(const std::shared_ptr<Layer>& target) const {
  if (!target) return {};
  auto world = [](const Layer* layer) {
    Transform matrix{};
    const Layer* root = layer;
    unsigned depth = 0;
    while (layer) {
      check(++depth <= 64, "excessive transform hierarchy");
      matrix = layer->local_transform().multiply(matrix);
      root = layer;
      auto p = layer->parent.lock();
      layer = p.get();
    }
    return std::pair{root, matrix};
  };
  const auto [a, ma] = world(this);
  const auto [b, mb] = world(target.get());
  if (a != b) return {};
  const auto inverse = mb.inverse();
  return inverse ? std::optional(inverse->multiply(ma)) : std::nullopt;
}
void Layer::clear() {
  for (auto& child : children)
    child->parent.reset();
  children.clear();
}
void BlurFilter::detach() {
  if (auto p = parent.lock()) p->remove_filter(shared_from_this());
}
void Layer::add_filter(std::shared_ptr<BlurFilter> filter) {
  check(kind == Kind::backdrop && bool(filter), "blur filters currently require a backdrop layer");
  if (filter->parent.lock().get() == this) return;
  check(filters.size() < 8, "too many backdrop filters");
  filters.reserve(filters.size() + 1);
  filter->detach();
  filters.push_back(filter);
  filter->parent = shared_from_this();
}
void Layer::remove_filter(std::shared_ptr<BlurFilter> filter) {
  const auto found = std::find(filters.begin(), filters.end(), filter);
  if (found != filters.end()) {
    (*found)->parent.reset();
    filters.erase(found);
  }
}
Image render(const std::shared_ptr<Layer>& root, std::uint32_t width, std::uint32_t height,
             RenderCosts* costs) {
  if (costs) *costs = {};
  check(width && height && width <= 8192 && height <= 8192 &&
            std::uint64_t(width) * height <= 32 * 1024 * 1024,
        "invalid render size");
  Image image{width, height, std::vector<std::uint32_t>(std::size_t(width) * height)};
  auto s = surface(image);
  Context cr(cairo_create(s.get()));
  RenderContext context;
  context.bytes = context.frame_bytes = image.pixels.size() * 4;
  context.costs = costs;
  if (root) draw(cr.get(), *root, 0, context);
  cairo_surface_flush(s.get());
  if (costs) {
    costs->visited_layers = context.count;
    costs->conical_pixels = context.gradient_pixels;
    costs->blur_taps = context.blur_work;
  }
  return image;
}
namespace {
std::uint32_t png_number(std::span<const std::uint8_t> bytes, std::size_t pos) {
  return (std::uint32_t(bytes[pos]) << 24) | (std::uint32_t(bytes[pos + 1]) << 16) |
         (std::uint32_t(bytes[pos + 2]) << 8) | bytes[pos + 3];
}

// Theme packs store colour already multiplied by alpha and say so with a
// tEXt chunk "hasPremultipliedAlpha" = "yes" ahead of the image data.
// Multiplying such pixels again darkens every antialiased edge and glow.
bool declares_premultiplied_alpha(std::span<const std::uint8_t> bytes) {
  constexpr std::string_view keyword{"hasPremultipliedAlpha"};
  std::size_t offset = 8;
  while (offset + 12 <= bytes.size()) {
    const std::size_t length = png_number(bytes, offset);
    check(length <= bytes.size() - offset - 12, "PNG chunk exceeds input");
    const auto* type = bytes.data() + offset + 4;
    if (std::memcmp(type, "IDAT", 4) == 0 || std::memcmp(type, "IEND", 4) == 0) {
      return false;
    }
    if (std::memcmp(type, "tEXt", 4) == 0) {
      const std::string_view text(reinterpret_cast<const char*>(bytes.data() + offset + 8), length);
      const auto separator = text.find('\0');
      if (separator != std::string_view::npos && text.substr(0, separator) == keyword) {
        return text.substr(separator + 1) == "yes";
      }
    }
    offset += 12 + length;
  }
  return false;
}

// Same rounding as Cairo's PNG loader, so straight-alpha images keep their
// previous premultiplied values exactly.
std::uint32_t multiply_alpha(std::uint32_t alpha, std::uint32_t colour) {
  const std::uint32_t product = alpha * colour + 0x80;
  return ((product >> 8) + product) >> 8;
}
}  // namespace

std::shared_ptr<Image> decode_png(std::span<const std::uint8_t> bytes) {
  constexpr std::array<std::uint8_t, 8> magic{137, 80, 78, 71, 13, 10, 26, 10};
  check(bytes.size() >= 33 && bytes.size() <= 16 * 1024 * 1024 &&
            std::equal(magic.begin(), magic.end(), bytes.begin()) &&
            std::memcmp(bytes.data() + 12, "IHDR", 4) == 0,
        "invalid PNG input");
  const auto w = png_number(bytes, 16);
  const auto h = png_number(bytes, 20);
  check(png_number(bytes, 8) == 13 && w && h && w <= 8192 && h <= 8192 &&
            std::uint64_t(w) * h <= 16 * 1024 * 1024,
        "PNG dimensions exceed limits");
  const bool premultiplied = declares_premultiplied_alpha(bytes);

  png_image png{};
  png.version = PNG_IMAGE_VERSION;
  struct Release {
    png_image* image;
    ~Release() { png_image_free(image); }
  } release{&png};
  check(png_image_begin_read_from_memory(&png, bytes.data(), bytes.size()) != 0,
        "PNG decoding failed");
  check(png.width == w && png.height == h, "PNG dimensions changed");
  png.format = PNG_FORMAT_RGBA;
  std::vector<std::uint8_t> rgba(PNG_IMAGE_SIZE(png));
  check(png_image_finish_read(&png, nullptr, rgba.data(), 0, nullptr) != 0, "PNG decoding failed");

  auto image = std::make_shared<Image>(Image{w, h, std::vector<std::uint32_t>(std::size_t(w) * h)});
  for (std::size_t i = 0; i < image->pixels.size(); ++i) {
    const auto* source = rgba.data() + i * 4;
    const std::uint32_t alpha = source[3];
    std::uint32_t red = source[0];
    std::uint32_t green = source[1];
    std::uint32_t blue = source[2];
    if (premultiplied) {
      // A declared premultiplied pixel can never exceed its own coverage.
      red = std::min(red, alpha);
      green = std::min(green, alpha);
      blue = std::min(blue, alpha);
    } else {
      red = multiply_alpha(alpha, red);
      green = multiply_alpha(alpha, green);
      blue = multiply_alpha(alpha, blue);
    }
    image->pixels[i] = (alpha << 24) | (red << 16) | (green << 8) | blue;
  }
  return image;
}
void write_png(const Image& image, const std::string& path) {
  auto& mutable_image = const_cast<Image&>(image);
  auto s = surface(mutable_image);
  check(cairo_surface_write_to_png(s.get(), path.c_str()) == CAIRO_STATUS_SUCCESS,
        "cannot write PNG");
}
}  // namespace headunit::theme::graphics
