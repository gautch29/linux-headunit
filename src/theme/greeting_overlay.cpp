// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/greeting_overlay.hpp>
#include <headunit/theme/brand_artwork.hpp>
#include <cmath>
namespace headunit::theme::graphics {
std::shared_ptr<const Layer> greeting_overlay(const GreetingFrame& frame, Size size) {
  if (frame.phase == GreetingPhase::inactive || frame.variant == GreetingVariant::off ||
      !std::isfinite(frame.opacity) || !std::isfinite(frame.reveal) || frame.opacity <= 0 ||
      frame.opacity > 1 || frame.reveal < 0 || frame.reveal > 1 || !std::isfinite(size.width) ||
      !std::isfinite(size.height) || size.width <= 0 || size.height <= 0)
    return {};
  auto root = std::make_shared<Layer>();
  root->anchor = {0, 0};
  root->bounds.size = size;
  root->opacity = frame.opacity;
  // Fit the same authored canvas as instruments, clear of the top arrows and
  // bottom warning row. No opaque whole-screen cover or entrypoint delay.
  const auto scale = std::min(size.width / 1920, size.height / 720);
  root->transform = {
      scale, 0, 0, scale, (size.width - 1920 * scale) / 2, (size.height - 720 * scale) / 2};
  auto logo = std::make_shared<Layer>();
  logo->anchor = {0, 0};
  constexpr double art_scale = .4;
  logo->transform = {art_scale, 0, 0, art_scale, 960 - 215 * art_scale, 170};
  if (frame.variant == GreetingVariant::brand) {
    for (const auto& points : brand_polygons) {
      auto shape = std::make_shared<Layer>(Kind::shape);
      shape->anchor = {0, 0};
      shape->path = std::make_shared<Path>();
      shape->path->append({SegmentType::move, points.front()});
      for (std::size_t i = 1; i < points.size(); ++i)
        shape->path->append({SegmentType::line, points[i]});
      shape->path->append({SegmentType::close});
      shape->fill = {.8, .9, 1, frame.reveal};
      shape->stroke = {.4, .82, 1, 1 - frame.reveal};
      shape->line_width = 2;
      logo->insert(shape, logo->children.size());
    }
  } else {
    auto line = std::make_shared<Layer>(Kind::shape);
    line->anchor = {0, 0};
    line->path = std::make_shared<Path>();
    line->path->append({SegmentType::move, {215 - 200 * frame.reveal, 152}});
    line->path->append({SegmentType::line, {215 + 200 * frame.reveal, 152}});
    line->stroke = {.4, .82, 1, 1};
    line->line_width = 3;
    logo->insert(line, 0);
  }
  root->insert(logo, 0);
  return root;
}
}  // namespace headunit::theme::graphics
