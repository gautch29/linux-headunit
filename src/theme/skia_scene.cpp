// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "skia_scene.hpp"
#include "circular_path.hpp"
#include "continuous_corners.hpp"
#include <headunit/theme/video_stream.hpp>
#include <include/core/SkCanvas.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageFilter.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSurface.h>
#include <include/core/SkShader.h>
#include <include/effects/SkColorMatrix.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkImageFilters.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/GrContextOptions.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/SkImageGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#include <include/gpu/ganesh/gl/egl/GrGLMakeEGLInterface.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <bit>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

namespace headunit::theme::graphics {
namespace {
static_assert(std::endian::native == std::endian::little,
              "Skia image adapter requires little-endian ARGB words");
constexpr double pi = std::numbers::pi;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
void scalar(double v) {
  check(std::isfinite(v) && std::abs(v) <= 1e7, "invalid Skia coordinate");
}
void unit(double v) {
  check(std::isfinite(v) && v >= 0 && v <= 1, "invalid Skia unit value");
}
void color(Color c) {
  for (const auto n : {c.r, c.g, c.b, c.a})
    unit(n);
}
SkColor4f skcolor(Color c, double alpha = 1) {
  return {float(c.r), float(c.g), float(c.b), float(c.a * alpha)};
}
SkPoint skpoint(Point p) {
  return {float(p.x), float(p.y)};
}
std::uint64_t clock_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
SkRect skrect(Rect r) {
  return SkRect::MakeXYWH(r.origin.x, r.origin.y, r.size.width, r.size.height);
}
SkMatrix skmatrix(Transform t) {
  return SkMatrix::MakeAll(t.a, t.c, t.tx, t.b, t.d, t.ty, 0, 0, 1);
}
SkRRect rounded(Rect r, double radius) {
  SkRRect result;
  result.setRectXY(skrect(r), std::clamp(radius, 0., std::min(r.size.width, r.size.height) / 2),
                   std::clamp(radius, 0., std::min(r.size.width, r.size.height) / 2));
  return result;
}
SkBlendMode blend(Blend b) {
  switch (b) {
    case Blend::source_over:
      return SkBlendMode::kSrcOver;
    case Blend::copy:
      return SkBlendMode::kSrc;
    case Blend::clear:
      return SkBlendMode::kClear;
    case Blend::source_in:
      return SkBlendMode::kSrcIn;
    case Blend::source_out:
      return SkBlendMode::kSrcOut;
    case Blend::source_atop:
      return SkBlendMode::kSrcATop;
    case Blend::dest_over:
      return SkBlendMode::kDstOver;
    case Blend::dest_in:
      return SkBlendMode::kDstIn;
    case Blend::dest_out:
      return SkBlendMode::kDstOut;
    case Blend::dest_atop:
      return SkBlendMode::kDstATop;
    case Blend::add:
      return SkBlendMode::kPlus;
    case Blend::xor_:
      return SkBlendMode::kXor;
    case Blend::screen:
      return SkBlendMode::kScreen;
    case Blend::exclusion:
      return SkBlendMode::kExclusion;
    case Blend::maximum:
      return SkBlendMode::kLighten;
    case Blend::minimum:
      return SkBlendMode::kDarken;
    default:
      throw std::runtime_error("Skia blend operation not implemented");
  }
}
bool unbounded(SkBlendMode mode) {
  return mode == SkBlendMode::kSrcIn || mode == SkBlendMode::kSrcOut ||
         mode == SkBlendMode::kDstIn || mode == SkBlendMode::kDstATop;
}
struct PaintOperation {
  SkAutoCanvasRestore restore;
  SkBlendMode mode;
  PaintOperation(SkCanvas& canvas, SkBlendMode requested)
      : restore(&canvas, false), mode(requested) {
    // Cairo's IN/OUT/DST_IN/DST_ATOP operators also affect transparent pixels
    // outside the painted geometry. Preserve that full-clip behavior per paint.
    if (unbounded(mode)) {
      SkPaint paint;
      paint.setBlendMode(mode);
      canvas.saveLayer(nullptr, &paint);
      mode = SkBlendMode::kSrcOver;
    }
  }
};
SkPath path(const Path& p, FillRule rule) {
  SkPathBuilder out;
  out.setFillType(rule == FillRule::evenodd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
  for (const auto& s : p.segments)
    switch (s.type) {
      case SegmentType::move:
        out.moveTo(skpoint(s.p));
        break;
      case SegmentType::line:
        out.lineTo(skpoint(s.p));
        break;
      case SegmentType::quad:
        out.quadTo(skpoint(s.c1), skpoint(s.p));
        break;
      case SegmentType::cubic:
        out.cubicTo(skpoint(s.c1), skpoint(s.c2), skpoint(s.p));
        break;
      case SegmentType::close:
        out.close();
        break;
      case SegmentType::ellipse:
        if (s.c1.x > 0 && s.c1.y > 0) out.addOval(SkRect::MakeXYWH(s.p.x, s.p.y, s.c1.x, s.c1.y));
        break;
      case SegmentType::arc: {
        double sweep = s.end - s.start;
        if (s.clockwise && sweep < 0) sweep += std::ceil(-sweep / (2 * pi)) * 2 * pi;
        if (!s.clockwise && sweep > 0) sweep -= std::ceil(sweep / (2 * pi)) * 2 * pi;
        const auto start = std::remainder(s.start, 2 * pi) * 180 / pi;
        const auto oval =
            SkRect::MakeXYWH(s.p.x - s.radius, s.p.y - s.radius, 2 * s.radius, 2 * s.radius);
        // Keep repeated turns: they affect trimmed length and even-odd fill.
        // Half-circle pieces avoid arcTo's full-circle special case.
        const auto pieces = std::max(1u, unsigned(std::ceil(std::abs(sweep) / pi)));
        for (unsigned i = 0; i < pieces; ++i)
          out.arcTo(oval, start + std::remainder(sweep * i / pieces, 2 * pi) * 180 / pi,
                    sweep / pieces * 180 / pi, false);
        break;
      }
    }
  return out.detach();
}
sk_sp<SkShader> conical_center(sk_sp<SkShader> gradient, Point center, double angle,
                               std::span<const Stop> stops) {
  // atan2(0,0) is undefined in GPU sweep implementations. The reference fixes
  // it to zero; preserve that convention at the singular centre sample.
  static const auto effect = SkRuntimeEffect::MakeForShader(
      SkString("uniform shader gradient; uniform float2 center; uniform half4 atCenter;"
               "half4 main(float2 p){return "
               "all(lessThanEqual(abs(p-center),float2(0.000001)))?atCenter:gradient.eval(p);}"));
  check(bool(effect.effect), "Skia conical centre effect unavailable");
  double position = angle / (2 * pi);
  position -= std::floor(position);
  const auto next = std::upper_bound(stops.begin(), stops.end(), position,
                                     [](double v, const Stop& stop) { return v < stop.location; });
  const auto& a = (next == stops.begin() ? stops.front() : *(next - 1));
  const auto& b = (next == stops.end() ? stops.back() : *next);
  const auto t = a.location == b.location
                     ? 0.
                     : std::clamp((position - a.location) / (b.location - a.location), 0., 1.);
  const auto mix = [&](double v, double w) {
    return float(v + (w - v) * t);
  };
  SkRuntimeShaderBuilder builder(effect.effect);
  builder.child("gradient") = std::move(gradient);
  builder.uniform("center") = std::array<float, 2>{float(center.x), float(center.y)};
  builder.uniform("atCenter") = std::array<float, 4>{
      mix(a.color.r * a.color.a, b.color.r * b.color.a),
      mix(a.color.g * a.color.a, b.color.g * b.color.a),
      mix(a.color.b * a.color.a, b.color.b * b.color.a), mix(a.color.a, b.color.a)};
  return builder.makeShader();
}
using BackdropPaths = std::unordered_map<const Layer*, bool>;
BackdropPaths backdrop_paths(const std::shared_ptr<const Layer>& root) {
  BackdropPaths result;
  std::size_t count = 0;
  const auto visit = [&](auto&& self, const std::shared_ptr<const Layer>& node,
                         unsigned depth) -> bool {
    check(bool(node) && depth <= 64 && ++count <= 4096, "Skia backdrop traversal exceeds limits");
    bool found = false;
    if (!node->hidden && node->opacity != 0) {
      if (node->kind == Kind::backdrop)
        for (const auto& filter : node->filters)
          if (filter && filter->enabled && filter->radius > 0) found = true;
      for (const auto& child : node->children)
        found = self(self, child, depth + 1) || found;
      if (node->mask)
        check(!self(self, node->mask, depth + 1), "Skia backdrop is not a coverage mask");
    }
    result[node.get()] = found;
    return found;
  };
  if (root) visit(visit, root, 0);
  return result;
}
void validate_image(const Image& image) {
  check(image.width && image.height && image.width <= 8192 && image.height <= 8192 &&
            std::uint64_t(image.width) * image.height <= 16 * 1024 * 1024 &&
            image.pixels.size() == std::size_t(image.width) * image.height,
        "invalid Skia image");
}
}  // namespace
struct SkiaSceneConfiguration {
  const bool probe_disable_simple_masks =
      std::getenv("HEADUNIT_THEME_PROBE_GPU_ONLY") &&
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_SIMPLE_MASK_GROUPS");
  const bool probe_disable_additive_mask_fusion =
      std::getenv("HEADUNIT_THEME_PROBE_GPU_ONLY") &&
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_ADDITIVE_MASK_FUSION");
  const bool probe_disable_fill_mask_clip =
      std::getenv("HEADUNIT_THEME_PROBE_GPU_ONLY") &&
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_FILL_MASK_CLIP");
  const bool probe_disable_backdrop_output_clip =
      std::getenv("HEADUNIT_THEME_PROBE_GPU_ONLY") &&
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_OUTPUT_CLIP");
  const bool probe_disable_backdrop_mask_clip =
      std::getenv("HEADUNIT_THEME_PROBE_GPU_ONLY") &&
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_MASK_CLIP");
  const bool probe_legacy_backdrop_filter =
      std::getenv("HEADUNIT_THEME_PROBE_LEGACY_BACKDROP_FILTER");
  const bool probe_full_backdrop_surface =
      std::getenv("HEADUNIT_THEME_PROBE_FULL_BACKDROP_SURFACE");
  const bool probe_disable_tight_path_bounds =
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_TIGHT_PATH_BOUNDS");
  const bool probe_disable_isolated_bounds =
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_ISOLATED_BOUNDS");
  const bool probe_disable_backdrop_mask_shader =
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_MASK_SHADER");
  const bool probe_disable_backdrop_group_fusion =
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_GROUP_FUSION");
  const bool probe_full_backdrop_group = std::getenv("HEADUNIT_THEME_PROBE_FULL_BACKDROP_GROUP");
  const bool probe_disable_coverage_crop =
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_COVERAGE_CROP");
  const bool probe_disable_backdrop_snapshot_region =
      std::getenv("HEADUNIT_THEME_PROBE_DISABLE_BACKDROP_SNAPSHOT_REGION");
};
struct SkiaScene::Impl : SkiaSceneConfiguration {
  sk_sp<GrDirectContext> context;
  sk_sp<SkSurface> surface;
  struct CachedImage {
    std::weak_ptr<const Image> source;
    sk_sp<SkImage> image;
    std::size_t bytes;
    std::uint64_t used, checked_frame;
  };
  std::unordered_map<const Image*, CachedImage> images;
  std::unordered_map<const video::NativeFrame*, sk_sp<SkImage>> native_images;
  std::size_t image_bytes = 0;
  std::uint64_t serial = 0, frame_serial = 0;
  std::unordered_map<const Layer*, std::optional<SkRect>> content_bounds;
  BackdropPaths backdrops;
  struct StrokeCache {
    std::vector<Segment> segments;
    std::array<double, 11> settings;
    std::shared_ptr<Image> pixels;
    SkRect destination;
    std::uint64_t used;
  };
  std::vector<StrokeCache> strokes;
  std::size_t stroke_bytes = 0;
  unsigned width = 0, height = 0;
  struct SceneKey {
    std::vector<double> numbers;
    // Retain exact image snapshots, so identity comparison cannot alias an
    // evicted/reallocated image. image() compares mutable pixels before reuse.
    std::vector<sk_sp<SkImage>> images;
    std::size_t image_bytes = 0;
    bool operator==(const SceneKey& other) const {
      return numbers == other.numbers && images == other.images;
    }
    std::size_t bytes() const {
      return numbers.size() * sizeof(double) + images.size() * sizeof(sk_sp<SkImage>) + image_bytes;
    }
  };
  struct RetainedGradient {
    std::weak_ptr<const Layer> source;
    SceneKey key;
    sk_sp<SkImage> image;
    SkRect destination;
    std::size_t bytes;
    std::uint64_t used;
  };
  std::vector<RetainedGradient> retained;
  GpuSceneCacheStats retained_stats;
  Impl() = default;  //Explicit offline raster comparison only.
  Impl(unsigned width, unsigned height, unsigned framebuffer,
       const SkiaSceneConfiguration& configuration)
      : SkiaSceneConfiguration(configuration), width(width), height(height) {
    GrContextOptions options;
    // Animated small gauge paths need accurate coverage, not a low-resolution
    // distance-field texture reused across many scales.
    options.fDisableDistanceFieldPaths = true;
    // Prefer analytic/coverage AA over four-sample intermediate surfaces.
    // MSAA has coarse coverage at thin gauge edges on the Pi render target.
    options.fInternalMultisampleCount = 1;
    context = GrDirectContexts::MakeGL(GrGLInterfaces::MakeEGL(), options);
    check(bool(context), "Skia EGL context unavailable");
    context->setResourceCacheLimit(128 * 1024 * 1024);
    const GrGLFramebufferInfo info{framebuffer,
                                   0x8058};  //GL_RGBA8, matching the owner's RGBA target.
    const auto target = GrBackendRenderTargets::MakeGL(width, height, 0, 0, info);
    surface =
        SkSurfaces::WrapBackendRenderTarget(context.get(), target, kBottomLeft_GrSurfaceOrigin,
                                            kRGBA_8888_SkColorType, nullptr, nullptr);
    check(bool(surface), "Skia GPU surface unavailable");
  }
  void prune() {
    for (auto i = retained.begin(); i != retained.end();)
      if (i->source.expired()) {
        retained_stats.bytes -= i->bytes;
        i = retained.erase(i);
      } else
        ++i;
    // Stroke coverage is keyed by exact geometry and transform below. Keep
    // expired source paths until the bounded LRU needs the space: scanning and
    // erasing those entries on every frame costs a measurable millisecond on
    // the CM5's signed cluster layout.
    for (auto i = images.begin(); i != images.end();)
      if (i->second.source.expired()) {
        image_bytes -= i->second.bytes;
        i = images.erase(i);
      } else
        ++i;
  }
  sk_sp<SkImage> image(const std::shared_ptr<const Image>& source) {
    auto i = images.find(source.get());
    if (i != images.end()) {
      // Traversal runs synchronously on the scene's owner thread and invokes
      // no client callbacks. Shared sprite sheets cannot change between their
      // uses in this draw. Compare their pixels once per frame, not per glyph.
      if (i->second.checked_frame == frame_serial && !i->second.source.expired()) {
        i->second.used = ++serial;
        ++retained_stats.image_hits;
        return i->second.image;
      }
      // Image is a public mutable type. Exact comparison detects mutation;
      // pointer identity or a collision-prone hash alone cannot safely cache it.
      SkPixmap cached;
      if (!i->second.source.expired() && i->second.image->width() == int(source->width) &&
          i->second.image->height() == int(source->height) &&
          i->second.image->peekPixels(&cached) && cached.rowBytes() == source->width * 4 &&
          !std::memcmp(cached.addr(), source->pixels.data(), source->pixels.size() * 4)) {
        i->second.used = ++serial;
        i->second.checked_frame = frame_serial;
        ++retained_stats.image_hits;
        return i->second.image;
      }
      image_bytes -= i->second.bytes;
      images.erase(i);
    }
    const auto bytes = source->pixels.size() * 4;
    while (!images.empty() && (images.size() >= 128 || image_bytes + bytes > 64 * 1024 * 1024)) {
      const auto oldest = std::min_element(
          images.begin(), images.end(),
          [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
      image_bytes -= oldest->second.bytes;
      images.erase(oldest);
    }
    // CPU reference uses native-endian 0xAARRGGBB words. This target is ARM64 LE.
    const SkPixmap pixmap(SkImageInfo::Make(source->width, source->height, kBGRA_8888_SkColorType,
                                            kPremul_SkAlphaType),
                          source->pixels.data(), source->width * 4);
    auto result = SkImages::RasterFromPixmapCopy(pixmap);
    check(bool(result), "Skia image snapshot failed");
    images.emplace(source.get(), CachedImage{source, result, bytes, ++serial, frame_serial});
    image_bytes += bytes;
    ++retained_stats.image_builds;
    return result;
  }
  bool scene_key(const Layer& value, SceneKey& key, bool normalized_opacity = false) {
    // Cache only source-over geometry. Video/backdrop depends on external
    // frames or destination pixels, and must not be captured as static content.
    if (value.blend != Blend::source_over || value.kind == Kind::video ||
        value.kind == Kind::backdrop || !value.filters.empty())
      return false;
    const auto segments = value.path ? value.path->segments.size() : 0;
    if (key.numbers.size() + 80 + segments * 13 + value.stops.size() * 5 > 131072) return false;
    auto& n = key.numbers;
    const auto opacity = normalized_opacity ? 1 : value.opacity;
    n.insert(n.end(), {double(unsigned(value.kind)), double(value.children.size()),
                       double(bool(value.mask)), double(value.hidden), opacity});
    if (value.hidden || value.opacity == 0) return true;
    const auto t = value.local_transform();
    n.insert(n.end(), {t.a,
                       t.b,
                       t.c,
                       t.d,
                       t.tx,
                       t.ty,
                       value.bounds.origin.x,
                       value.bounds.origin.y,
                       value.bounds.size.width,
                       value.bounds.size.height,
                       opacity,
                       value.border_width,
                       value.corner_radius,
                       value.line_width,
                       value.miter_limit,
                       value.stroke_start,
                       value.stroke_end,
                       double(value.clip),
                       double(value.group_blending),
                       double(value.antialiased),
                       double(value.continuous_corners),
                       double(unsigned(value.fill_rule)),
                       double(unsigned(value.cap)),
                       double(unsigned(value.join)),
                       double(unsigned(value.gradient_kind)),
                       value.gradient_start.x,
                       value.gradient_start.y,
                       value.gradient_end.x,
                       value.gradient_end.y});
    if (!value.transform.inverse()) return true;
    for (const auto c : {value.background, value.border, value.fill, value.stroke, value.tint})
      n.insert(n.end(), {c.r, c.g, c.b, c.a});
    n.push_back(value.stops.size());
    for (const auto& s : value.stops)
      n.insert(n.end(), {s.location, s.color.r, s.color.g, s.color.b, s.color.a});
    n.push_back(bool(value.path));
    n.push_back(segments);
    if (value.path)
      for (const auto& s : value.path->segments)
        n.insert(n.end(), {double(unsigned(s.type)), s.p.x, s.p.y, s.c1.x, s.c1.y, s.c2.x, s.c2.y,
                           s.radius, s.start, s.end, double(s.clockwise)});
    n.push_back(bool(value.image));
    if (value.image) {
      const auto bytes = value.image->pixels.size() * 4;
      if (key.image_bytes + bytes > 64 * 1024 * 1024) return false;
      key.images.push_back(image(value.image));
      key.image_bytes += bytes;
    }
    for (const auto& child : value.children)
      if (!scene_key(*child, key)) return false;
    return !value.mask || scene_key(*value.mask, key);
  }
  SkRect shape_bounds(const Layer& layer) const {
    const auto shape = path(*layer.path, layer.fill_rule);
    // getBounds includes off-curve Bezier controls. For a short circular arc
    // those can extend almost a hundred pixels beyond its visible contour.
    // Tight extrema keep coverage/filter regions and copies on the real path.
    return probe_disable_tight_path_bounds ? shape.getBounds() : shape.computeTightBounds();
  }
  struct StrokeCoverage {
    std::shared_ptr<Image> pixels;
    SkRect destination;
  };
  std::optional<StrokeCoverage> stroke_coverage(const SkMatrix& matrix, const Layer& layer,
                                                bool filled = false) {
    const Transform transform{matrix.getScaleX(), matrix.getSkewY(),      matrix.getSkewX(),
                              matrix.getScaleY(), matrix.getTranslateX(), matrix.getTranslateY()};
    const std::array<double, 11> settings{
        transform.a,
        transform.b,
        transform.c,
        transform.d,
        transform.tx,
        transform.ty,
        filled ? -1 : layer.line_width,
        filled ? 0 : layer.miter_limit,
        filled ? 0 : layer.stroke_start,
        filled ? 0 : layer.stroke_end,
        filled ? double(unsigned(layer.fill_rule) * 2 + layer.antialiased)
               : double(unsigned(layer.cap) * 16 + unsigned(layer.join) * 2 + layer.antialiased)};
    const auto same = [](const Segment& a, const Segment& b) {
      return a.type == b.type && a.p.x == b.p.x && a.p.y == b.p.y && a.c1.x == b.c1.x &&
             a.c1.y == b.c1.y && a.c2.x == b.c2.x && a.c2.y == b.c2.y && a.radius == b.radius &&
             a.start == b.start && a.end == b.end && a.clockwise == b.clockwise;
    };
    auto found = std::find_if(strokes.begin(), strokes.end(), [&](const auto& entry) {
      return entry.settings == settings && entry.segments.size() == layer.path->segments.size() &&
             std::equal(entry.segments.begin(), entry.segments.end(), layer.path->segments.begin(),
                        same);
    });
    if (found == strokes.end()) {
      auto bounds = shape_bounds(layer);
      if (!filled) {
        const auto pad = stroke_outset(layer);
        bounds.outset(pad, pad);
      }
      bounds = matrix.mapRect(bounds);
      check(bounds.isFinite(), "invalid stroke coverage bounds");
      bounds = SkRect::MakeLTRB(std::floor(bounds.left()) - 2, std::floor(bounds.top()) - 2,
                                std::ceil(bounds.right()) + 2, std::ceil(bounds.bottom()) + 2);
      if (!bounds.intersect(SkRect::MakeWH(width, height))) return {};
      const unsigned w = unsigned(bounds.width()), h = unsigned(bounds.height());
      if (!w || !h) return {};
      auto shape = std::make_shared<Layer>(Kind::shape);
      shape->anchor = {0, 0};
      shape->transform = transform;
      shape->transform.tx -= bounds.left();
      shape->transform.ty -= bounds.top();
      shape->path = layer.path;
      shape->fill_rule = layer.fill_rule;
      if (filled)
        shape->fill = {1, 1, 1, 1};
      else {
        shape->stroke = {1, 1, 1, 1};
        shape->line_width = layer.line_width;
        shape->miter_limit = layer.miter_limit;
        shape->cap = layer.cap;
        shape->join = layer.join;
      }
      shape->stroke_start = layer.stroke_start;
      shape->stroke_end = layer.stroke_end;
      shape->antialiased = layer.antialiased;
      const auto raster_began = clock_ns();
      auto pixels = std::make_shared<Image>(graphics::render(shape, w, h));
      if (!probe_disable_coverage_crop) {
        // The exact CPU coverage is already available. Discard its transparent
        // padding before uploading/sampling it; a short trimmed arc otherwise
        // retains the full untrimmed circle's rectangle. Preserve every nonzero
        // AA pixel, without estimating the path or changing its endpoint.
        unsigned left = w, top = h, right = 0, bottom = 0;
        for (unsigned y = 0; y < h; ++y)
          for (unsigned x = 0; x < w; ++x)
            if (pixels->pixels[std::size_t(y) * w + x] >> 24) {
              left = std::min(left, x);
              top = std::min(top, y);
              right = std::max(right, x + 1);
              bottom = std::max(bottom, y + 1);
            }
        if (left >= right) {
          *pixels = {};
          bounds = SkRect::MakeEmpty();
        } else if (left || top || right != w || bottom != h) {
          Image cropped{right - left, bottom - top,
                        std::vector<std::uint32_t>(std::size_t(right - left) * (bottom - top))};
          for (unsigned y = top; y < bottom; ++y)
            std::copy_n(pixels->pixels.begin() + std::size_t(y) * w + left, cropped.width,
                        cropped.pixels.begin() + std::size_t(y - top) * cropped.width);
          *pixels = std::move(cropped);
          bounds = SkRect::MakeXYWH(bounds.left() + left, bounds.top() + top, right - left,
                                    bottom - top);
        }
      }
      retained_stats.stroke_raster_ns += clock_ns() - raster_began;
      ++retained_stats.stroke_builds;
      const auto bytes = pixels->pixels.size() * 4 + layer.path->segments.size() * sizeof(Segment);
      while (!strokes.empty() &&
             (strokes.size() >= 64 || stroke_bytes + bytes > 64 * 1024 * 1024)) {
        const auto oldest =
            std::min_element(strokes.begin(), strokes.end(),
                             [](const auto& a, const auto& b) { return a.used < b.used; });
        stroke_bytes -=
            oldest->pixels->pixels.size() * 4 + oldest->segments.size() * sizeof(Segment);
        strokes.erase(oldest);
      }
      strokes.push_back({layer.path->segments, settings, std::move(pixels), bounds, ++serial});
      stroke_bytes += bytes;
      found = strokes.end() - 1;
    } else
      ++retained_stats.stroke_hits;
    found->used = ++serial;
    if (found->destination.isEmpty()) return {};
    return StrokeCoverage{found->pixels, found->destination};
  }
  void stroke(SkCanvas& canvas, const Layer& layer, double alpha, SkBlendMode mode) {
    const auto found = stroke_coverage(canvas.getLocalToDeviceAs3x3(), layer);
    if (!found) return;
    SkAutoCanvasRestore restore(&canvas, true);
    canvas.resetMatrix();
    if (mode == SkBlendMode::kSrcOver) {
      // Source-over is linear in source alpha, so cached coverage can directly
      // modulate it. Avoid a separate shader-clip surface for ordinary gauges.
      SkPaint paint;
      paint.setAlphaf(alpha);
      paint.setColorFilter(
          SkColorFilters::Blend(skcolor(layer.stroke), nullptr, SkBlendMode::kSrcIn));
      canvas.drawImageRect(image(found->pixels), found->destination,
                           SkSamplingOptions(SkFilterMode::kNearest), &paint);
      return;
    }
    const auto offset = SkMatrix::Translate(found->destination.left(), found->destination.top());
    auto coverage = image(found->pixels)
                        ->makeShader(SkTileMode::kDecal, SkTileMode::kDecal,
                                     SkSamplingOptions(SkFilterMode::kNearest), &offset);
    check(bool(coverage), "Skia stroke coverage shader unavailable");
    // Alpha is geometric coverage. Treating it as ordinary source-image alpha
    // would incorrectly clear the bounding rectangle with Src/Clear blends.
    canvas.clipRect(found->destination, false);
    canvas.clipShader(std::move(coverage));
    SkPaint paint;
    paint.setBlendMode(mode);
    paint.setColor(skcolor(layer.stroke, alpha));
    canvas.drawPaint(paint);
  }
  sk_sp<SkShader> gradient_shader(const Layer& layer) {
    const auto& r = layer.bounds;
    const Point a{r.origin.x + layer.gradient_start.x * r.size.width,
                  r.origin.y + layer.gradient_start.y * r.size.height};
    const Point b{r.origin.x + layer.gradient_end.x * r.size.width,
                  r.origin.y + layer.gradient_end.y * r.size.height};
    std::vector<SkColor4f> colors;
    std::vector<float> positions;
    for (const auto& s : layer.stops) {
      colors.push_back(skcolor(s.color));
      positions.push_back(s.location);
    }
    const SkGradient gradient(
        {colors, positions, SkTileMode::kClamp},
        {layer.gradient_kind == GradientKind::conical ? SkGradient::Interpolation::InPremul::kYes
                                                      : SkGradient::Interpolation::InPremul::kNo});
    sk_sp<SkShader> shader;
    if (layer.gradient_kind == GradientKind::linear) {
      const SkPoint points[]{skpoint(a), skpoint(b)};
      shader = SkShaders::LinearGradient(points, gradient);
    } else if (layer.gradient_kind == GradientKind::radial)
      shader = SkShaders::RadialGradient(skpoint(a), std::hypot(b.x - a.x, b.y - a.y), gradient);
    else {
      // CP sweep increases counterclockwise in y-down. Reflect canonical
      // Skia sweep coordinates, then rotate its zero ray to gradient_end.
      const auto angle = std::atan2(b.y - a.y, b.x - a.x), cs = std::cos(angle),
                 sn = std::sin(angle);
      const auto matrix = skmatrix({cs, sn, sn, -cs, a.x, a.y});
      shader = conical_center(SkShaders::SweepGradient({0, 0}, gradient, &matrix), a, angle,
                              layer.stops);
    }
    check(bool(shader), "Skia gradient creation failed");
    return shader;
  }
  void draw_rounded(SkCanvas& canvas, Rect rect, double radius, bool continuous,
                    const SkPaint& paint) {
    if (continuous && radius > 0)
      canvas.drawPath(path(continuous_rectangle(rect, radius), FillRule::nonzero), paint);
    else
      canvas.drawRRect(rounded(rect, radius), paint);
  }
  void content(SkCanvas& canvas, const Layer& layer, double alpha, SkBlendMode mode) {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setBlendMode(mode);
    if (layer.background.a > 0) {
      PaintOperation operation(canvas, mode);
      paint.setBlendMode(operation.mode);
      paint.setColor(skcolor(layer.background, alpha));
      draw_rounded(canvas, layer.bounds, layer.corner_radius, layer.continuous_corners, paint);
    }
    if (layer.kind == Kind::shape && layer.path) {
      const auto shape = path(*layer.path, layer.fill_rule);
      paint.setAntiAlias(layer.antialiased);
      {
        PaintOperation operation(canvas, mode);
        paint.setBlendMode(operation.mode);
        paint.setColor(skcolor(layer.fill, alpha));
        canvas.drawPath(shape, paint);
      }
      const auto lo = std::clamp(layer.stroke_start, 0., 1.),
                 hi = std::clamp(layer.stroke_end, 0., 1.);
      if (layer.line_width > 0 && layer.stroke.a > 0 && hi > lo) {
        PaintOperation operation(canvas, mode);
        stroke(canvas, layer, alpha, operation.mode);
      }
    } else if (layer.kind == Kind::gradient && !layer.stops.empty() &&
               layer.bounds.size.width > 0 && layer.bounds.size.height > 0) {
      const auto& r = layer.bounds;
      auto shader = gradient_shader(layer);
      PaintOperation operation(canvas, mode);
      paint.setBlendMode(operation.mode);
      check(bool(shader), "Skia gradient creation failed");
      paint.setColor(SkColors::kWhite);
      paint.setAlphaf(alpha);
      paint.setShader(std::move(shader));
      draw_rounded(canvas, r, layer.corner_radius, layer.continuous_corners, paint);
    } else if ((layer.kind == Kind::image || layer.kind == Kind::video) &&
               layer.bounds.size.width > 0 && layer.bounds.size.height > 0) {
      const auto native =
          layer.kind == Kind::video && layer.video ? layer.video->native() : nullptr;
      const std::shared_ptr<const Image> source =
          layer.kind == Kind::video ? (layer.video && !native ? layer.video->image() : nullptr)
                                    : layer.image;
      if (source || native) {
        PaintOperation operation(canvas, mode);
        paint.setBlendMode(operation.mode);
        paint.setColor(SkColors::kWhite);
        paint.setAlphaf(alpha);
        SkColorMatrix tint;
        tint.setScale(layer.tint.r, layer.tint.g, layer.tint.b, layer.tint.a);
        paint.setColorFilter(SkColorFilters::Matrix(tint));
        if (native) {
          const auto found = native_images.find(native.get());
          check(found != native_images.end(), "native video texture not supplied to Skia");
          const auto crop = SkRect::MakeXYWH(native->crop_x, native->crop_y, native->metadata.width,
                                             native->metadata.height);
          canvas.drawImageRect(found->second, crop, skrect(layer.bounds),
                               SkSamplingOptions(SkFilterMode::kLinear), &paint,
                               SkCanvas::kStrict_SrcRectConstraint);
        } else
          canvas.drawImageRect(image(source), skrect(layer.bounds),
                               SkSamplingOptions(SkFilterMode::kLinear), &paint);
      }
    }
    if (layer.border_width > 0 && layer.border.a > 0) {
      const auto inset = std::min(layer.border_width / 2,
                                  std::min(layer.bounds.size.width, layer.bounds.size.height) / 2);
      auto r = layer.bounds;
      r.origin.x += inset;
      r.origin.y += inset;
      r.size.width -= 2 * inset;
      r.size.height -= 2 * inset;
      PaintOperation operation(canvas, mode);
      paint = SkPaint();
      paint.setAntiAlias(true);
      paint.setBlendMode(operation.mode);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(layer.border_width);
      paint.setColor(skcolor(layer.border, alpha));
      draw_rounded(canvas, r, std::max(0., layer.corner_radius - inset), layer.continuous_corners,
                   paint);
    }
  }
  bool masked_gradient(SkCanvas& canvas, const Layer& value, double alpha) {
    if (value.kind != Kind::gradient || !value.mask || !value.children.empty() ||
        value.background.a != 0 || (value.border_width > 0 && value.border.a > 0) ||
        (value.blend != Blend::source_over && value.blend != Blend::add) ||
        (value.blend == Blend::add && probe_disable_additive_mask_fusion) || value.stops.empty() ||
        value.bounds.size.width == 0 || value.bounds.size.height == 0 || value.corner_radius != 0 ||
        value.clip)
      return false;
    auto matrix = canvas.getLocalToDeviceAs3x3();
    auto node = value.mask;
    double mask_alpha = 1;
    for (unsigned depth = 0; depth <= 64; ++depth) {
      if (node->blend != Blend::source_over || node->mask || node->clip ||
          node->background.a != 0 || (node->border_width > 0 && node->border.a > 0) ||
          !node->filters.empty())
        return false;
      if (node->hidden || node->opacity == 0) return true;
      mask_alpha *= node->opacity;
      matrix.preConcat(skmatrix(node->local_transform()));
      if (node->kind == Kind::layer && node->children.size() == 1) {
        node = node->children[0];
        continue;
      }
      if (node->kind != Kind::shape || !node->path || !node->children.empty() || node->fill.a != 0)
        return false;
      if (node->stroke.a == 0 || node->line_width == 0) return true;
      mask_alpha *= node->stroke.a;
      const auto mask = stroke_coverage(matrix, *node);
      if (!mask) return true;
      SkMatrix inverse;
      check(canvas.getLocalToDeviceAs3x3().invert(&inverse), "singular masked-gradient transform");
      const auto local = SkMatrix::Concat(
          inverse, SkMatrix::Translate(mask->destination.left(), mask->destination.top()));
      auto coverage = image(mask->pixels)
                          ->makeShader(SkTileMode::kDecal, SkTileMode::kDecal,
                                       SkSamplingOptions(SkFilterMode::kNearest), &local);
      static const auto effect = SkRuntimeEffect::MakeForShader(
          SkString("uniform shader gradient; uniform shader coverage; uniform half maskAlpha;"
                   "half4 main(float2 p){half a=coverage.eval(p).a*maskAlpha;if(a==0)return "
                   "half4(0);return gradient.eval(p)*a;}"));
      check(bool(effect.effect) && bool(coverage), "masked-gradient shader unavailable");
      SkRuntimeShaderBuilder builder(effect.effect);
      builder.child("gradient") = gradient_shader(value);
      builder.child("coverage") = std::move(coverage);
      builder.uniform("maskAlpha") = float(mask_alpha);
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setAlphaf(alpha);
      paint.setBlendMode(blend(value.blend));
      paint.setShader(builder.makeShader());
      check(bool(paint.getShader()), "masked-gradient shader creation failed");
      if (value.blend == Blend::add) ++retained_stats.additive_mask_fusions;
      if (!probe_disable_coverage_crop) {
        const auto transform = canvas.getLocalToDeviceAs3x3();
        canvas.resetMatrix();
        canvas.clipRect(mask->destination, false);
        canvas.setMatrix(transform);
      }
      canvas.drawRect(skrect(value.bounds), paint);
      return true;
    }
    return false;
  }
  bool simple_shape_mask_group(SkCanvas& canvas, const Layer& value, double alpha) {
    // A single solid rounded rectangle under one shape mask has only one
    // source-over paint. Applying the mask as a coverage clip is equivalent to
    // masking an isolated group, but avoids two intermediate GPU layers.
    if (value.kind != Kind::layer || value.blend != Blend::source_over || value.background.a != 0 ||
        (value.border_width > 0 && value.border.a > 0) || value.children.size() != 1 ||
        !value.mask || !value.filters.empty())
      return false;
    const auto& child = *value.children.front();
    const auto& mask = *value.mask;
    if (child.kind != Kind::layer || child.blend != Blend::source_over || child.background.a == 0 ||
        (child.border_width > 0 && child.border.a > 0) || child.mask || !child.children.empty() ||
        !child.filters.empty() || mask.kind != Kind::shape || mask.blend != Blend::source_over ||
        mask.mask || mask.clip || mask.background.a != 0 ||
        (mask.border_width > 0 && mask.border.a > 0) || !mask.path || !mask.children.empty() ||
        !mask.filters.empty() || !mask.transform.inverse())
      return false;
    if (mask.hidden || mask.opacity == 0) return true;
    if (!probe_disable_fill_mask_clip && mask.fill.a == 1 && mask.opacity == 1 &&
        (mask.line_width <= 0 || mask.stroke.a == 0)) {
      const auto matrix =
          SkMatrix::Concat(canvas.getLocalToDeviceAs3x3(), skmatrix(mask.local_transform()));
      const auto coverage = stroke_coverage(matrix, mask, true);
      if (!coverage) return true;
      SkMatrix inverse;
      check(canvas.getLocalToDeviceAs3x3().invert(&inverse), "singular fill-mask group transform");
      const auto local = SkMatrix::Concat(
          inverse, SkMatrix::Translate(coverage->destination.left(), coverage->destination.top()));
      auto shader = image(coverage->pixels)
                        ->makeShader(SkTileMode::kDecal, SkTileMode::kDecal,
                                     SkSamplingOptions(SkFilterMode::kNearest), &local);
      check(bool(shader), "fill-mask group coverage unavailable");
      canvas.clipShader(std::move(shader));
      ++retained_stats.simple_fill_mask_groups;
      layer(canvas, child, alpha, false);
      return true;
    }
    if (mask.fill.a != 0 || mask.line_width <= 0 || mask.stroke.a == 0) return false;
    const auto matrix =
        SkMatrix::Concat(canvas.getLocalToDeviceAs3x3(), skmatrix(mask.local_transform()));
    const auto coverage = stroke_coverage(matrix, mask);
    if (!coverage) return true;
    SkMatrix inverse;
    check(canvas.getLocalToDeviceAs3x3().invert(&inverse), "singular stroke-mask group transform");
    const auto local = SkMatrix::Concat(
        inverse, SkMatrix::Translate(coverage->destination.left(), coverage->destination.top()));
    auto shader = image(coverage->pixels)
                      ->makeShader(SkTileMode::kDecal, SkTileMode::kDecal,
                                   SkSamplingOptions(SkFilterMode::kNearest), &local);
    static const auto effect = SkRuntimeEffect::MakeForShader(
        SkString("uniform shader coverage; uniform half alpha;"
                 "half4 main(float2 p){return coverage.eval(p)*alpha;}"));
    check(bool(effect.effect) && bool(shader), "stroke-mask group shader unavailable");
    SkRuntimeShaderBuilder builder(effect.effect);
    builder.child("coverage") = std::move(shader);
    builder.uniform("alpha") = float(mask.opacity * mask.stroke.a);
    shader = builder.makeShader();
    check(bool(shader), "stroke-mask group coverage unavailable");
    canvas.clipShader(std::move(shader));
    ++retained_stats.simple_mask_groups;
    layer(canvas, child, alpha, false);
    return true;
  }
  std::optional<SkRect> bounds(const Layer& value, bool isolated = false) {
    // A destructive layer blended into an external destination must retain
    // its full clip. Inside a transparent isolation, however, it cannot create
    // coverage outside the union of source paints. Bound that isolated output
    // even when a descendant cuts holes in it.
    if (value.blend != Blend::source_over && !isolated) return std::nullopt;
    if (const auto found = content_bounds.find(&value); found != content_bounds.end())
      return found->second;
    SkRect box = SkRect::MakeEmpty();
    if (value.hidden || value.opacity == 0 || !value.transform.inverse())
      return content_bounds[&value] = box;
    if (backdrops[&value]) return content_bounds[&value] = std::nullopt;
    if (value.background.a > 0 || (value.border_width > 0 && value.border.a > 0) ||
        (value.kind == Kind::gradient && !value.stops.empty()) ||
        (value.kind == Kind::image && value.image) ||
        (value.kind == Kind::video && value.video &&
         (value.video->native() || value.video->image())))
      box = skrect(value.bounds);
    if (value.border_width > 0 && value.border.a > 0) {
      auto edge = skrect(value.bounds);
      const auto excess = std::max(
          0.,
          (value.border_width - std::min(value.bounds.size.width, value.bounds.size.height)) / 2);
      edge.outset(excess, excess);
      box.join(edge);
    }
    if (value.kind == Kind::shape && value.path) {
      auto shape = shape_bounds(value);
      if (value.stroke.a > 0 && value.line_width > 0) {
        const auto outset = stroke_outset(value);
        shape.outset(outset, outset);
      }
      if (value.fill.a > 0 || (value.stroke.a > 0 && value.line_width > 0)) box.join(shape);
    }
    for (const auto& child : value.children) {
      const auto child_box = bounds(*child, !probe_disable_isolated_bounds);
      if (!child_box) return content_bounds[&value] = std::nullopt;
      if (!child_box->isEmpty()) box.join(skmatrix(child->local_transform()).mapRect(*child_box));
    }
    if (value.mask) {
      const auto mask_box = bounds(*value.mask);
      if (!mask_box) return content_bounds[&value] = std::nullopt;
      if (!box.intersect(skmatrix(value.mask->local_transform()).mapRect(*mask_box)))
        box.setEmpty();
    }
    if (value.clip && !box.intersect(skrect(value.bounds))) box.setEmpty();
    if (!box.isFinite()) return content_bounds[&value] = std::nullopt;
    return content_bounds[&value] = box;
  }
  bool retained_gradient(SkCanvas& canvas, const Layer& value, double alpha) {
    if (value.kind != Kind::gradient || !value.mask || !value.children.empty() ||
        value.background.a != 0 || (value.border_width > 0 && value.border.a > 0) ||
        value.blend != Blend::source_over || value.stops.empty() || value.corner_radius != 0 ||
        value.clip || !canvas.isClipRect() || value.weak_from_this().expired())
      return false;
    const auto matrix = canvas.getLocalToDeviceAs3x3();
    SceneKey key;
    key.numbers.reserve(1024);
    for (unsigned i = 0; i < 9; ++i)
      key.numbers.push_back(matrix[i]);
    if (!scene_key(value, key, true)) return false;
    auto found = std::find_if(retained.begin(), retained.end(),
                              [&](const auto& e) { return e.source.lock().get() == &value; });
    if (found != retained.end() && found->key == key) {
      ++retained_stats.hits;
      found->used = ++serial;
    } else {
      if (found != retained.end()) {
        retained_stats.bytes -= found->bytes;
        retained.erase(found);
        ++retained_stats.evictions;
      }
      const auto box = bounds(value);
      if (!box) return false;
      if (box->isEmpty()) return true;
      auto destination = matrix.mapRect(*box);
      if (!destination.isFinite()) return false;
      destination =
          SkRect::MakeLTRB(std::floor(destination.left()) - 2, std::floor(destination.top()) - 2,
                           std::ceil(destination.right()) + 2, std::ceil(destination.bottom()) + 2);
      if (!destination.intersect(SkRect::MakeWH(width, height))) return true;
      const unsigned w = unsigned(destination.width()), h = unsigned(destination.height());
      const auto bytes = std::size_t(w) * h * 4 + key.bytes();
      if (bytes > 64 * 1024 * 1024) return false;
      while (!retained.empty() &&
             (retained.size() >= 32 || retained_stats.bytes + bytes > 64 * 1024 * 1024)) {
        const auto oldest =
            std::min_element(retained.begin(), retained.end(),
                             [](const auto& a, const auto& b) { return a.used < b.used; });
        retained_stats.bytes -= oldest->bytes;
        retained.erase(oldest);
        ++retained_stats.evictions;
      }
      // Retain a bounded GPU-rendered group, not a CPU scene bitmap. The mask,
      // paths and images are traversed normally only when their exact key changes.
      auto target = surface->makeSurface(
          SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
      check(bool(target), "Skia retained-gradient surface unavailable");
      auto& output = *target->getCanvas();
      output.clear(SkColors::kTransparent);
      output.setMatrix(
          SkMatrix::Concat(SkMatrix::Translate(-destination.left(), -destination.top()), matrix));
      Layer local = value;
      local.anchor = {0, 0};
      local.position = local.bounds.origin;
      local.transform = {};
      local.opacity = 1;
      content_bounds.erase(&local);
      layer(output, local, 1, false);
      content_bounds.erase(&local);
      auto snapshot = target->makeImageSnapshot();
      check(bool(snapshot), "Skia retained-gradient snapshot unavailable");
      retained.push_back({value.weak_from_this(), std::move(key), std::move(snapshot), destination,
                          bytes, ++serial});
      retained_stats.bytes += bytes;
      ++retained_stats.builds;
      found = retained.end() - 1;
    }
    SkAutoCanvasRestore restore(&canvas, true);
    canvas.resetMatrix();
    SkPaint paint;
    paint.setAlphaf(alpha);
    canvas.drawImageRect(found->image, found->destination,
                         SkSamplingOptions(SkFilterMode::kNearest), &paint);
    return true;
  }
  sk_sp<SkSurface> working_surface(const sk_sp<SkImage>& background = {}, SkIRect pixels = {}) {
    if (pixels.isEmpty())
      pixels = SkIRect::MakeWH(background ? background->width() : width,
                               background ? background->height() : height);
    auto target = surface->makeSurface(SkImageInfo::Make(
        pixels.width(), pixels.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType));
    check(bool(target), "Skia backdrop surface unavailable");
    auto& canvas = *target->getCanvas();
    canvas.clear(SkColors::kTransparent);
    if (background) {
      SkPaint copy;
      copy.setBlendMode(SkBlendMode::kSrc);
      canvas.drawImage(background, -pixels.left(), -pixels.top(), SkSamplingOptions(), &copy);
    }
    return target;
  }
  sk_sp<SkImage> snapshot(SkCanvas& canvas) {
    auto* target = canvas.getSurface();
    check(target, "Skia backdrop needs an owned working surface");
    auto result = target->makeImageSnapshot();
    check(bool(result), "Skia backdrop snapshot unavailable");
    return result;
  }
  void replace(SkCanvas& canvas, const sk_sp<SkImage>& before, const sk_sp<SkImage>& after,
               const sk_sp<SkImage>& mask, double alpha, SkPoint after_origin = {0, 0},
               SkPoint mask_origin = {0, 0}, sk_sp<SkShader> mask_shader = {},
               SkPoint before_origin = {0, 0}) {
    static const auto effect = SkRuntimeEffect::MakeForShader(SkString(
        "uniform shader before; uniform shader after; uniform shader mask; uniform float alpha;"
        "half4 main(float2 p) { return "
        "mix(before.eval(p),after.eval(p),half(clamp(alpha*mask.eval(p).a,0,1))); }"));
    check(bool(effect.effect), "Skia backdrop replacement shader failed");
    SkRuntimeShaderBuilder builder(effect.effect);
    const auto before_offset = SkMatrix::Translate(before_origin.x(), before_origin.y());
    builder.child("before") =
        before->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                           SkSamplingOptions(SkFilterMode::kNearest), &before_offset);
    const auto offset = SkMatrix::Translate(after_origin.x(), after_origin.y());
    builder.child("after") = after->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                               SkSamplingOptions(SkFilterMode::kNearest), &offset);
    const auto mask_offset = SkMatrix::Translate(mask_origin.x(), mask_origin.y());
    builder.child("mask") =
        mask_shader ? std::move(mask_shader)
        : mask      ? mask->makeShader(SkTileMode::kDecal, SkTileMode::kDecal,
                                       SkSamplingOptions(SkFilterMode::kNearest), &mask_offset)
                    : SkShaders::Color(SK_ColorWHITE);
    builder.uniform("alpha") = float(alpha);
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setShader(builder.makeShader());
    check(bool(paint.getShader()), "Skia backdrop replacement unavailable");
    SkAutoCanvasRestore restore(&canvas, true);
    canvas.resetMatrix();
    canvas.drawPaint(paint);
  }
  sk_sp<SkImageFilter> blur_filter(const Layer& value) {
    sk_sp<SkImageFilter> filter;
    for (const auto& blur : value.filters)
      if (blur->enabled && blur->radius > 0)
        filter = SkImageFilters::Blur(
            blur->radius, blur->radius,
            blur->hard_edges || blur->normalized_edges ? SkTileMode::kClamp : SkTileMode::kDecal,
            std::move(filter));
    return filter;
  }
  std::optional<SkIRect> device_support(const Layer& value, const SkMatrix& matrix) {
    if (context && !probe_disable_coverage_crop && value.kind == Kind::shape && value.path &&
        !value.mask && value.children.empty() && value.fill.a == 0 && value.background.a == 0 &&
        !(value.border_width > 0 && value.border.a > 0) && value.stroke.a > 0 &&
        value.line_width > 0) {
      const auto coverage = stroke_coverage(matrix, value);
      if (!coverage) return SkIRect::MakeEmpty();
      auto region = coverage->destination;
      region.outset(2, 2);
      SkIRect result;
      region.roundOut(&result);
      return result;
    }
    const auto support = bounds(value);
    if (!support) return {};
    auto region = matrix.mapRect(*support);
    if (!region.isFinite()) return {};
    if (region.isEmpty()) return SkIRect::MakeEmpty();
    region.outset(2, 2);
    SkIRect result;
    region.roundOut(&result);
    return result;
  }
  void backdrop(SkCanvas& canvas, const Layer& value, double alpha) {
    auto filter = blur_filter(value);
    if (!filter || value.bounds.size.width <= 0 || value.bounds.size.height <= 0) return;
    // Keep the complete source, including samples outside the panel. Bound
    // only filter output work/storage: saveLayer expands input for the filter
    // halo itself. Final replacement applies panel/ancestor AA exactly once.
    const auto before = snapshot(canvas);
    const auto matrix = canvas.getLocalToDeviceAs3x3();
    const auto full = SkIRect::MakeWH(before->width(), before->height());
    auto pixels = full;
    if (context && !probe_disable_backdrop_output_clip) {
      auto region = matrix.mapRect(skrect(value.bounds));
      if (region.isFinite()) {
        region.outset(2, 2);
        region.roundOut(&pixels);
        if (!pixels.intersect(full)) return;
        ++retained_stats.backdrop_output_clips;
      }
    }
    const bool cropped = context && !probe_legacy_backdrop_filter && !probe_full_backdrop_surface;
    const SkPoint origin =
        cropped ? SkPoint::Make(pixels.left(), pixels.top()) : SkPoint::Make(0, 0);
    auto filtered =
        cropped ? surface->makeSurface(SkImageInfo::Make(
                      pixels.width(), pixels.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType))
                : working_surface(probe_legacy_backdrop_filter ? before : sk_sp<SkImage>{}, full);
    check(bool(filtered), "Skia filtered surface unavailable");
    auto& target = *filtered->getCanvas();
    if (cropped)
      target.clear(SkColors::kTransparent);
    else
      target.clipIRect(pixels);
    target.setMatrix(SkMatrix::Concat(SkMatrix::Translate(-origin.x(), -origin.y()), matrix));
    SkPaint source;
    source.setBlendMode(SkBlendMode::kSrc);
    if (probe_legacy_backdrop_filter) {
      target.saveLayer(SkCanvas::SaveLayerRec(nullptr, &source, filter.get(), 0));
      target.restore();
    } else {
      // Filter the immutable input directly. Seeding another full-size surface
      // before asking saveLayer to copy its backdrop performs the same copy
      // twice. Keep the local matrix on the filter, but draw input in pixels.
      source.setImageFilter(filter);
      target.saveLayer(nullptr, &source);
      target.resetMatrix();
      target.drawImage(before, -origin.x(), -origin.y());
      target.restore();
    }
    const auto after = filtered->makeImageSnapshot();
    check(bool(after), "Skia filtered snapshot unavailable");
    SkAutoCanvasRestore restore(&canvas, true);
    canvas.clipRect(skrect(value.bounds), true);
    replace(canvas, before, after, {}, alpha, origin);
  }
  // A group that begins with a fully covering opaque backdrop can start
  // directly from that filtered immutable input. No seeded copy, intermediate
  // replacement or input-halo-sized working surface is needed. Later paints
  // retain their order and the final mask/opacity is applied exactly once.
  struct BackdropSeed {
    sk_sp<SkImageFilter> filter;
    SkMatrix transform;
    std::size_t first;
  };
  std::optional<BackdropSeed> backdrop_seed(const Layer& value, const SkMatrix& matrix,
                                            SkIRect output, double local) {
    if (!context || probe_disable_backdrop_group_fusion || probe_full_backdrop_group ||
        local != 1 || value.kind != Kind::layer || value.background.a > 0 ||
        (value.border_width > 0 && value.border.a > 0))
      return {};
    std::size_t first = 0;
    while (first < value.children.size() &&
           (value.children[first]->hidden || value.children[first]->opacity == 0))
      ++first;
    if (first == value.children.size()) return {};
    const auto& child = *value.children[first];
    if (child.kind != Kind::backdrop || child.opacity != 1 || child.mask ||
        !child.children.empty() || child.corner_radius != 0 || child.blend != Blend::source_over)
      return {};
    for (std::size_t i = first + 1; i < value.children.size(); ++i)
      if (backdrops[value.children[i].get()]) return {};
    const auto filter = blur_filter(child);
    if (!filter) return {};
    const auto transform = SkMatrix::Concat(matrix, skmatrix(child.local_transform()));
    if (!transform.rectStaysRect() ||
        !transform.mapRect(skrect(child.bounds)).contains(SkRect::Make(output)))
      return {};
    return BackdropSeed{filter, transform, first};
  }
  sk_sp<SkShader> backdrop_mask_shader(const Layer& mask, const SkMatrix& matrix) {
    if (!context || probe_disable_backdrop_mask_shader || probe_full_backdrop_group ||
        mask.kind != Kind::shape || !mask.path || mask.mask || !mask.children.empty() ||
        mask.clip || mask.background.a > 0 || (mask.border_width > 0 && mask.border.a > 0) ||
        mask.blend != Blend::source_over || mask.fill.a > 0 || mask.stroke.a <= 0 ||
        mask.line_width <= 0)
      return {};
    const auto coverage = stroke_coverage(matrix, mask);
    if (!coverage) return {};
    const auto offset =
        SkMatrix::Translate(coverage->destination.left(), coverage->destination.top());
    auto shader = image(coverage->pixels)
                      ->makeShader(SkTileMode::kDecal, SkTileMode::kDecal,
                                   SkSamplingOptions(SkFilterMode::kNearest), &offset);
    static const auto effect = SkRuntimeEffect::MakeForShader(
        SkString("uniform shader coverage; uniform float alpha; half4 main(float2 p){return "
                 "half4(coverage.eval(p).a*alpha);}"));
    check(bool(effect.effect), "backdrop coverage shader unavailable");
    SkRuntimeShaderBuilder builder(effect.effect);
    builder.child("coverage") = std::move(shader);
    builder.uniform("alpha") = float(mask.opacity * mask.stroke.a);
    ++retained_stats.backdrop_mask_shaders;
    return builder.makeShader();
  }
  // A copied backdrop is a replacement buffer, not an isolated transparent
  // group. Keep its immutable parent separately: a destination-out cut reveals
  // that parent instead of punching a transparent hole through the whole scene.
  // Origins are in the current working canvas's device pixels.
  struct BackdropParent {
    sk_sp<SkImage> image;
    SkPoint origin;
  };
  void backdrop_cut(SkCanvas& canvas, const Layer& value, double alpha, bool group,
                    bool allow_retained, const BackdropParent& parent) {
    auto* target = canvas.getSurface();
    check(target, "Skia backdrop cut needs an owned surface");
    const auto matrix = canvas.getLocalToDeviceAs3x3();
    auto pixels = SkIRect::MakeWH(target->width(), target->height());
    if (!pixels.intersect(canvas.getDeviceClipBounds())) return;
    if (const auto box = bounds(value, true)) {
      auto region = matrix.mapRect(*box);
      check(region.isFinite(), "invalid backdrop cut bounds");
      if (region.isEmpty()) return;
      region.outset(2, 2);
      SkIRect support;
      region.roundOut(&support);
      if (!pixels.intersect(support)) return;
    }
    const auto origin = SkPoint::Make(pixels.left(), pixels.top());
    auto coverage = working_surface({}, pixels);
    auto& source = *coverage->getCanvas();
    source.setMatrix(SkMatrix::Concat(SkMatrix::Translate(-origin.x(), -origin.y()), matrix));
    const auto local = group && value.group_blending ? 1 : alpha;
    content(source, value, local, SkBlendMode::kSrcOver);
    if (group) {
      for (const auto& child : value.children)
        layer(source, *child, local, allow_retained);
      if (value.mask) {
        SkPaint mask;
        mask.setBlendMode(SkBlendMode::kDstIn);
        source.saveLayer(nullptr, &mask);
        layer(source, *value.mask, 1, allow_retained);
        source.restore();
      }
    }
    const auto mask = coverage->makeImageSnapshot();
    check(bool(mask), "Skia backdrop cut coverage unavailable");
    const auto before = target->makeImageSnapshot(pixels);
    check(bool(before), "Skia backdrop cut snapshot unavailable");
    {
      // P' = (1-A)*P + A*D in premultiplied channels, where D is the
      // immutable parent. Final group opacity/masking still applies once.
      // Do not copy the current AA clip into coverage and apply it twice.
      SkAutoCanvasRestore restore(&canvas, true);
      canvas.resetMatrix();
      canvas.clipIRect(pixels);
      replace(canvas, before, parent.image, mask, group && value.group_blending ? alpha : 1,
              parent.origin, origin, {}, origin);
    }
    // Explicitly flattened layers apply their blend to their own paints;
    // descendants retain their own blend modes against the same destination.
    if (!group)
      for (const auto& child : value.children)
        layer(canvas, *child, local, allow_retained, &parent);
  }
  void backdrop_group(SkCanvas& canvas, const Layer& value, double alpha, bool allow_retained) {
    const auto matrix = canvas.getLocalToDeviceAs3x3();
    auto* parent = canvas.getSurface();
    check(parent, "Skia backdrop group needs an owned surface");
    const auto full = SkIRect::MakeWH(parent->width(), parent->height());
    auto output = full;
    if (context && !probe_full_backdrop_group) {
      if (!output.intersect(canvas.getDeviceClipBounds())) return;
      if (value.mask)
        if (const auto support = device_support(
                *value.mask, SkMatrix::Concat(matrix, skmatrix(value.mask->local_transform()))))
          if (!output.intersect(*support)) return;
    }
    const auto local = value.group_blending ? 1 : alpha;
    const auto seed = backdrop_seed(value, matrix, output, local);
    auto before_region = full;
    if (seed && !probe_disable_backdrop_snapshot_region) {
      before_region = seed->filter->filterBounds(output, seed->transform,
                                                 SkImageFilter::kReverse_MapDirection, &full);
      before_region.join(output);
      check(before_region.intersect(full), "Skia backdrop snapshot has no source region");
    }
    const auto before = parent->makeImageSnapshot(before_region);
    check(bool(before), "Skia backdrop group snapshot unavailable");
    const auto before_origin = SkPoint::Make(before_region.left(), before_region.top());
    if (before_region != full) {
      ++retained_stats.backdrop_snapshot_regions;
      retained_stats.backdrop_snapshot_saved_pixels +=
          std::uint64_t(full.width()) * full.height() -
          std::uint64_t(before_region.width()) * before_region.height();
    }
    sk_sp<SkSurface> working;
    std::size_t first_child = 0;
    const bool fused = bool(seed);
    if (seed) {
      working = working_surface({}, output);
      auto& target = *working->getCanvas();
      target.setMatrix(
          SkMatrix::Concat(SkMatrix::Translate(-output.left(), -output.top()), seed->transform));
      SkPaint paint;
      paint.setBlendMode(SkBlendMode::kSrc);
      paint.setImageFilter(seed->filter);
      target.saveLayer(nullptr, &paint);
      target.resetMatrix();
      target.drawImage(before, before_origin.x() - output.left(), before_origin.y() - output.top());
      target.restore();
      ++retained_stats.backdrop_group_fusions;
      first_child = seed->first + 1;
    }
    // Nested or partially opaque backdrops retain the full working surface.
    // Cropping their seeded intermediate changes Skia's blur sampling phase,
    // even when filterBounds includes the complete mathematical halo. Only
    // the qualified single, fully covering seed filters the immutable parent
    // directly into bounded output without changing intermediate coordinates.
    auto input = fused ? output : full;
    if (!input.intersect(full)) return;
    if (input != full) ++retained_stats.backdrop_group_regions;
    const auto origin = SkPoint::Make(input.left(), input.top());
    if (!fused) {
      working = working_surface(before, input);
      first_child = 0;
    }
    auto& target = *working->getCanvas();
    target.setMatrix(SkMatrix::Concat(SkMatrix::Translate(-origin.x(), -origin.y()), matrix));
    // Ancestor clips apply once when this completed group replaces its parent.
    // Regional input preserves the complete backwards-computed blur halo.
    content(target, value, local, SkBlendMode::kSrcOver);
    const BackdropParent backdrop_parent{
        before, SkPoint::Make(before_origin.x() - origin.x(), before_origin.y() - origin.y())};
    for (std::size_t i = first_child; i < value.children.size(); ++i)
      layer(target, *value.children[i], local, allow_retained, &backdrop_parent);
    sk_sp<SkImage> mask;
    sk_sp<SkShader> mask_shader;
    if (value.mask)
      mask_shader = backdrop_mask_shader(
          *value.mask, SkMatrix::Concat(matrix, skmatrix(value.mask->local_transform())));
    const auto mask_origin = SkPoint::Make(output.left(), output.top());
    if (value.mask && !mask_shader) {
      auto coverage = working_surface({}, output);
      auto& coverage_canvas = *coverage->getCanvas();
      coverage_canvas.setMatrix(
          SkMatrix::Concat(SkMatrix::Translate(-mask_origin.x(), -mask_origin.y()), matrix));
      layer(coverage_canvas, *value.mask, 1, allow_retained);
      mask = coverage->makeImageSnapshot();
      check(bool(mask), "Skia backdrop mask snapshot unavailable");
    }
    const auto after = working->makeImageSnapshot();
    check(bool(after), "Skia completed backdrop group unavailable");
    SkAutoCanvasRestore restore(&canvas, true);
    if (context && (!probe_disable_backdrop_mask_clip || output != full)) {
      canvas.resetMatrix();
      canvas.clipIRect(output);
      canvas.setMatrix(matrix);
      if (value.mask) ++retained_stats.backdrop_mask_replacement_clips;
    }
    replace(canvas, before, after, mask, value.group_blending ? alpha : 1, origin, mask_origin,
            std::move(mask_shader), before_origin);
  }
  void layer(SkCanvas& canvas, const Layer& value, double inherited = 1, bool allow_retained = true,
             const BackdropParent* parent = nullptr) {
    if (value.hidden || value.opacity == 0 || !value.transform.inverse()) return;
    SkAutoCanvasRestore restore(&canvas, true);
    canvas.concat(skmatrix(value.local_transform()));
    if (value.clip) {
      if (value.continuous_corners && value.corner_radius > 0)
        canvas.clipPath(
            path(continuous_rectangle(value.bounds, value.corner_radius), FillRule::nonzero), true);
      else
        canvas.clipRRect(rounded(value.bounds, value.corner_radius), true);
    }
    const auto alpha = inherited * value.opacity;
    if (value.kind == Kind::backdrop && backdrops[&value]) {
      backdrop(canvas, value, alpha);
      return;
    }
    const bool group = value.needs_group(alpha);
    if (group && backdrops[&value]) {
      backdrop_group(canvas, value, alpha, allow_retained);
      return;
    }
    if (parent && value.blend == Blend::dest_out) {
      backdrop_cut(canvas, value, alpha, group, allow_retained, *parent);
      return;
    }
    if (allow_retained && retained_gradient(canvas, value, alpha)) return;
    if (masked_gradient(canvas, value, alpha)) return;
    if (group && !probe_disable_simple_masks && simple_shape_mask_group(canvas, value, alpha))
      return;
    if (group) {
      if (const auto box = bounds(value)) {
        if (box->isEmpty()) return;
        const auto matrix = canvas.getLocalToDeviceAs3x3();
        auto device_box = matrix.mapRect(*box);
        // Round outward in device pixels, leaving two pixels for antialiasing.
        // The group itself must be source-over. Destructive descendants act
        // on its transparent isolation, whose output stays inside this union.
        if (device_box.isFinite()) {
          device_box = SkRect::MakeLTRB(
              std::floor(device_box.left()) - 2, std::floor(device_box.top()) - 2,
              std::ceil(device_box.right()) + 2, std::ceil(device_box.bottom()) + 2);
          canvas.resetMatrix();
          canvas.clipRect(device_box, false);
          canvas.setMatrix(matrix);
        }
      }
      SkPaint paint;
      paint.setAlphaf(value.group_blending ? alpha : 1);
      paint.setBlendMode(blend(value.blend));
      canvas.saveLayer(nullptr, &paint);
      ++retained_stats.group_saves;
    }
    const auto local = group && value.group_blending ? 1 : alpha;
    content(canvas, value, local, group ? SkBlendMode::kSrcOver : blend(value.blend));
    for (const auto& child : value.children)
      layer(canvas, *child, local, allow_retained, group ? nullptr : parent);
    if (value.mask) {
      // Isolate the whole mask; DstIn applies its transparent pixels too, so
      // content outside a smaller/empty mask is cleared as in the reference.
      SkPaint mask;
      mask.setBlendMode(SkBlendMode::kDstIn);
      canvas.saveLayer(nullptr, &mask);
      layer(canvas, *value.mask, 1, allow_retained);
      canvas.restore();
    }
    if (group) canvas.restore();
  }
};
void SkiaScene::validate(const std::shared_ptr<const Layer>& root, unsigned width,
                         unsigned height) {
  check(width && height && width <= 4096 && height <= 2160, "invalid Skia target");
  const auto backdrops = backdrop_paths(root);
  std::size_t count = 0, effect_bytes = 0;
  const std::size_t frame_bytes = std::size_t(width) * height * 4;
  const auto visit = [&](auto&& self, const std::shared_ptr<const Layer>& l, unsigned depth,
                         double inherited, std::size_t bytes, bool seeded) -> void {
    check(bool(l) && depth <= 64 && ++count <= 4096, "Skia scene exceeds traversal limits");
    check(unsigned(l->kind) <= unsigned(Kind::backdrop) &&
              unsigned(l->gradient_kind) <= unsigned(GradientKind::conical) &&
              unsigned(l->fill_rule) <= unsigned(FillRule::evenodd) &&
              unsigned(l->cap) <= unsigned(Cap::square) &&
              unsigned(l->join) <= unsigned(Join::bevel),
          "invalid Skia layer enum");
    unit(l->opacity);
    if (l->hidden || l->opacity == 0) return;
    for (const auto v :
         {l->position.x,       l->position.y,       l->anchor.x,          l->anchor.y,
          l->bounds.origin.x,  l->bounds.origin.y,  l->bounds.size.width, l->bounds.size.height,
          l->transform.a,      l->transform.b,      l->transform.c,       l->transform.d,
          l->transform.tx,     l->transform.ty,     l->border_width,      l->corner_radius,
          l->line_width,       l->miter_limit,      l->stroke_start,      l->stroke_end,
          l->gradient_start.x, l->gradient_start.y, l->gradient_end.x,    l->gradient_end.y})
      scalar(v);
    check(l->bounds.size.width >= 0 && l->bounds.size.height >= 0 && l->corner_radius >= 0 &&
              l->border_width >= 0 && l->line_width >= 0 && l->miter_limit >= 0,
          "invalid Skia geometry");
    if (!l->transform.inverse()) return;
    for (const auto c : {l->background, l->border, l->fill, l->stroke, l->tint})
      color(c);
    (void)blend(l->blend);
    check(l->filters.size() <= 8 && (l->filters.empty() || l->kind == Kind::backdrop),
          "invalid Skia filter attachment");
    for (const auto& f : l->filters) {
      check(bool(f), "null Skia filter");
      scalar(f->radius);
      check(f->radius >= 0 && f->radius <= 128, "invalid Skia blur");
    }
    if (l->kind == Kind::backdrop && !l->filters.empty())
      check(l->blend == Blend::source_over && !l->mask && !l->clip && l->corner_radius == 0 &&
                l->background.a == 0 && l->border_width == 0 && l->children.empty(),
            "unsupported Skia backdrop composition");
    check(l->stops.size() <= 256, "Skia gradient stop limit exceeded");
    double previous = -1;
    for (const auto& s : l->stops) {
      color(s.color);
      unit(s.location);
      check(s.location >= previous, "unordered Skia gradient stops");
      previous = s.location;
    }
    if (l->kind == Kind::gradient && !l->stops.empty() && l->bounds.size.width &&
        l->bounds.size.height)
      check(std::hypot((l->gradient_end.x - l->gradient_start.x) * l->bounds.size.width,
                       (l->gradient_end.y - l->gradient_start.y) * l->bounds.size.height) > 1e-12,
            "degenerate Skia gradient");
    if (l->path) {
      check(l->path->segments.size() <= 65536, "Skia path limit exceeded");
      double pieces = 0;
      for (const auto& s : l->path->segments) {
        check(unsigned(s.type) <= unsigned(SegmentType::close), "invalid Skia path enum");
        for (const auto n :
             {s.p.x, s.p.y, s.c1.x, s.c1.y, s.c2.x, s.c2.y, s.radius, s.start, s.end})
          scalar(n);
        check(s.radius >= 0 && (s.type != SegmentType::ellipse || (s.c1.x >= 0 && s.c1.y >= 0)),
              "invalid Skia path geometry");
        pieces += s.type == SegmentType::arc
                      ? std::max(1., std::ceil(std::abs(s.end - s.start) / pi))
                      : 1;
        check(pieces <= 65536, "Skia expanded path limit exceeded");
      }
    }
    if (l->image) validate_image(*l->image);
    if (l->video && !l->video->native()) {
      if (auto img = l->video->image()) validate_image(*img);
    }
    const auto alpha = inherited * l->opacity;
    const bool group = l->needs_group(alpha);
    const auto found = backdrops.find(l.get());
    const bool has_backdrop = found != backdrops.end() && found->second;
    if (has_backdrop || seeded)
      check(l->blend == Blend::source_over || (!has_backdrop && l->blend == Blend::dest_out),
            ("unsupported Skia backdrop replacement blend: layer=" + l->name +
             ";blend=" + std::to_string(unsigned(l->blend)))
                .c_str());
    // Snapshot + working/result + optional mask; nested effects must fit too.
    if (group) bytes += frame_bytes * (has_backdrop ? 4 : 1);
    if (group && has_backdrop) effect_bytes += frame_bytes * 4;
    if (l->kind == Kind::backdrop && has_backdrop) {
      bytes += frame_bytes * 3;
      effect_bytes += frame_bytes * 3;
    }
    // Destination-out needs source coverage and a current-destination snapshot,
    // plus copy-on-write storage when replacing that destination. Estimate full
    // frames before drawing; actual cuts use their bounded source regions.
    if (seeded && l->blend == Blend::dest_out) {
      bytes += frame_bytes * 3;
      effect_bytes += frame_bytes * 3;
    }
    // Also bound sibling effect work retained in deferred GPU command buffers.
    // This is an allocation/work estimate, not a driver-enforced memory ceiling.
    check(effect_bytes <= 512 * 1024 * 1024, "Skia backdrop frame work budget exceeded");
    check(bytes <= 128 * 1024 * 1024, "Skia group budget exceeded");
    if (!group && unbounded(blend(l->blend)))
      check(bytes + frame_bytes <= 128 * 1024 * 1024, "Skia paint group budget exceeded");
    // A regular isolated group starts transparent, even inside a backdrop's
    // copied destination. Its destructive children affect only that group.
    // Direct destination-out restores the immutable parent of a copied
    // backdrop. Other destructive modes remain unsupported in that buffer.
    const bool children_seeded = group ? has_backdrop : seeded;
    for (const auto& child : l->children)
      self(self, child, depth + 1, group && l->group_blending ? 1 : alpha, bytes, children_seeded);
    if (l->mask) self(self, l->mask, depth + 1, 1, bytes + frame_bytes, false);
  };
  if (root) visit(visit, root, 0, 1, frame_bytes, false);
}
SkiaScene::Configuration SkiaScene::capture_configuration() {
  return std::make_shared<const SkiaSceneConfiguration>();
}
SkiaScene::SkiaScene(unsigned w, unsigned h, unsigned framebuffer, Configuration configuration)
    : impl_(std::make_unique<Impl>(w, h, framebuffer,
                                   *(configuration ? configuration : capture_configuration()))) {}
SkiaScene::~SkiaScene() = default;
void SkiaScene::abandon() {
  impl_->context->abandonContext();
}
GpuSceneCacheStats SkiaScene::cache_stats() const {
  auto stats = impl_->retained_stats;
  stats.entries = impl_->retained.size();
  stats.image_bytes = impl_->image_bytes;
  if (impl_->context) {
    std::size_t bytes = 0;
    impl_->context->getResourceCacheUsage(nullptr, &bytes);
    stats.gpu_cache_bytes = bytes;
  }
  return stats;
}
Image SkiaScene::raster_probe(const std::shared_ptr<const Layer>& root, unsigned width,
                              unsigned height) {
  validate(root, width, height);
  Impl drawing;
  drawing.width = width;
  drawing.height = height;
  auto surface = SkSurfaces::Raster(
      SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType));
  check(bool(surface), "Skia raster diagnostic allocation failed");
  drawing.surface = surface;
  drawing.backdrops = backdrop_paths(root);
  surface->getCanvas()->clear(SkColors::kTransparent);
  if (root) drawing.layer(*surface->getCanvas(), *root);
  Image output{width, height, std::vector<std::uint32_t>(std::size_t(width) * height)};
  check(surface->readPixels(
            SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
            output.pixels.data(), width * 4, 0, 0),
        "Skia raster diagnostic read failed");
  return output;
}
void SkiaScene::draw(const std::shared_ptr<const Layer>& root,
                     std::span<SkiaNativeTexture*> native) {
  auto& h = *impl_;
  const auto preparing = clock_ns();
  h.prune();
  h.content_bounds.clear();
  h.backdrops = backdrop_paths(root);
  check(h.native_images.empty() && native.size() <= 8, "native Skia image lease bound exceeded");
  for (auto* input : native) {
    check(input && input->frame && input->texture && !input->released,
          "invalid native Skia texture");
    const auto& f = *input->frame;
    const GrGLTextureInfo info{0x8d65, input->texture,
                               0x8058};  //EXTERNAL_OES, RGBA8 after EGL YUV conversion.
    const auto texture =
        GrBackendTextures::MakeGL(f.storage_width, f.storage_height, skgpu::Mipmapped::kNo, info);
    auto image = SkImages::BorrowTextureFrom(
        h.context.get(), texture, kTopLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType,
        kOpaque_SkAlphaType, nullptr, [](void* flag) { *static_cast<bool*>(flag) = true; },
        &input->released);
    check(bool(image), "Skia native external texture is unsupported");
    check(h.native_images.emplace(input->frame.get(), std::move(image)).second,
          "duplicate native Skia texture");
  }
  if (++h.frame_serial == 0) {
    h.images.clear();
    h.image_bytes = 0;
  }
  h.context->resetContext();
  auto& canvas = *h.surface->getCanvas();
  const auto began = clock_ns();
  h.retained_stats.preparation_ns = began - preparing;
  canvas.clear(SkColors::kTransparent);
  if (root) h.layer(canvas, *root);
  const auto recorded = clock_ns();
  h.retained_stats.traversal_ns = recorded - began;
  check(h.context->flushAndSubmit(h.surface.get()).fSuccess, "Skia GPU submission failed");
  h.retained_stats.submit_ns = clock_ns() - recorded;
}
struct SkiaFrameImages {
  std::unordered_map<const video::NativeFrame*, sk_sp<SkImage>> images;
};
std::shared_ptr<SkiaFrameImages> SkiaScene::retain_native() {
  auto retained = std::make_shared<SkiaFrameImages>();
  retained->images = std::move(impl_->native_images);
  impl_->native_images.clear();
  return retained;
}
void SkiaScene::release_native(std::shared_ptr<SkiaFrameImages>& retained) {
  retained.reset();
  impl_->context->checkAsyncWorkCompletion();
}
void SkiaScene::release_native() {
  impl_->native_images.clear();
  impl_->context->checkAsyncWorkCompletion();
}
}  // namespace headunit::theme::graphics
