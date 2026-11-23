// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace headunit::theme::video {
struct View;
}

namespace headunit::theme::graphics {
struct Point {
  double x = 0, y = 0;
};
struct Size {
  double width = 0, height = 0;
};
struct Rect {
  Point origin;
  Size size;
};
struct Color {
  double r = 0, g = 0, b = 0, a = 0;
};
struct Transform {
  double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
  Point apply(Point) const;
  // Column-vector matrix product: result applies rhs, then this transform.
  Transform multiply(const Transform& rhs) const;
  std::optional<Transform> inverse() const;
};
enum class SegmentType { move, line, quad, cubic, arc, ellipse, close };
struct Segment {
  SegmentType type;
  Point p{}, c1{}, c2{};
  double radius = 0, start = 0, end = 0;
  bool clockwise = true;
};
struct Path {
  std::vector<Segment> segments;
  std::size_t point_count = 0;
  void append(Segment);
};
enum class Kind { layer, shape, gradient, image, video, backdrop };
enum class Blend {
  source_over,
  copy,
  clear,
  source_in,
  source_out,
  source_atop,
  dest_over,
  dest_in,
  dest_out,
  dest_atop,
  add,
  xor_,
  screen,
  exclusion,
  maximum,
  minimum,
  subtract_source,
  subtract_dest,
  component_mask
};
enum class FillRule { nonzero, evenodd };
enum class Cap { butt, round, square };
enum class Join { miter, round, bevel };
enum class GradientKind { linear, radial, conical };
struct Stop {
  Color color;
  double location = 0;
};
struct Image {
  std::uint32_t width = 0, height = 0;
  // Cairo ARGB32: native-endian uint32_t 0xAARRGGBB, premultiplied components.
  std::vector<std::uint32_t> pixels;
};
// Immutable, cropped alpha coverage in output-pixel coordinates. Replacing a
// mask creates a new object, so a GPU cache cannot silently reuse stale pixels.
class AlphaMask {
 public:
  explicit AlphaMask(const Image&, std::int32_t x = 0, std::int32_t y = 0);
  AlphaMask(const AlphaMask&) = delete;
  AlphaMask& operator=(const AlphaMask&) = delete;
  std::int32_t x() const { return x_; }
  std::int32_t y() const { return y_; }
  std::uint32_t width() const { return width_; }
  std::uint32_t height() const { return height_; }
  std::span<const std::uint8_t> pixels() const { return pixels_; }
  bool empty() const { return pixels_.empty(); }

 private:
  std::int32_t x_ = 0, y_ = 0;
  std::uint32_t width_ = 0, height_ = 0;
  std::vector<std::uint8_t> pixels_;
};
struct Layer;
struct BlurFilter : std::enable_shared_from_this<BlurFilter> {
  double radius = 0;
  bool enabled = true, hard_edges = false, normalized_edges = false;
  std::weak_ptr<Layer> parent;
  void detach();
};
struct Layer : std::enable_shared_from_this<Layer> {
  explicit Layer(Kind value = Kind::layer) : kind(value) {}
  Kind kind;
  std::string name;
  Point position{}, anchor{.5, .5};
  Rect bounds{};
  Transform transform{};
  Color background{}, border{}, fill{}, stroke{}, tint{1, 1, 1, 1};
  double opacity = 1, border_width = 0, corner_radius = 0, line_width = 1, miter_limit = 10;
  double stroke_start = 0, stroke_end = 1;
  bool hidden = false, clip = false, group_blending = true, antialiased = true, rasterize = false;
  bool continuous_corners = false;
  Blend blend = Blend::source_over;
  FillRule fill_rule = FillRule::nonzero;
  Cap cap = Cap::butt;
  Join join = Join::miter;
  GradientKind gradient_kind = GradientKind::linear;
  Point gradient_start{.5, 0}, gradient_end{.5, 1};
  std::vector<Stop> stops;
  std::shared_ptr<Path> path;
  std::shared_ptr<Image> image;
  std::shared_ptr<video::View> video;
  std::weak_ptr<Layer> parent;
  std::shared_ptr<Layer> mask;
  std::vector<std::shared_ptr<Layer>> children;
  std::vector<std::shared_ptr<BlurFilter>> filters;
  Rect frame() const;
  void set_frame(Rect);
  void detach();
  void insert(std::shared_ptr<Layer>, std::size_t index);
  void remove(std::shared_ptr<Layer>);
  void set_mask(std::shared_ptr<Layer>);
  Transform local_transform() const;
  std::optional<Transform> transform_to(const std::shared_ptr<Layer>&) const;
  void clear();
  void add_filter(std::shared_ptr<BlurFilter>);
  void remove_filter(std::shared_ptr<BlurFilter>);
  // A destination-erasing child must be evaluated against its immediate
  // parent's image, even when that parent is opaque and source-over. Otherwise
  // an optimized flattened traversal cuts holes in unrelated ancestor content.
  bool needs_group(double effective_alpha) const;
};

// Deterministic CPU reference renderer for pixel correctness. GPU composition
// and video import can consume the same scene model; this is not a performance
// claim for a complete Ultra package at display refresh rate.
// Optional per-frame costs, containing counts and durations only. Profiling
// never changes pixels, skips work, or advances a presentation timestamp.
struct RenderCosts {
  std::uint64_t visited_layers = 0, conical_pixels = 0, blur_taps = 0;
  std::uint64_t content_ns = 0, backdrop_ns = 0;
};
Image render(const std::shared_ptr<Layer>&, std::uint32_t width, std::uint32_t height,
             RenderCosts* costs = nullptr);
std::shared_ptr<Image> decode_png(std::span<const std::uint8_t>);
void write_png(const Image&, const std::string& path);
}  // namespace headunit::theme::graphics
