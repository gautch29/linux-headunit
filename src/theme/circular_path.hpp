// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/scene.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace headunit::theme::graphics {
struct CircularPath {
  Point center;
  double radius, start, sweep;
  bool closed = false;
};
// Only a single contour with no connecting lines qualifies. A partial closed
// arc includes a chord, and a general ellipse has nonuniform arc length.
inline std::optional<CircularPath> circular_path(const Path& path) {
  if (path.segments.size() != 1) return {};
  const auto& s = path.segments.front();
  constexpr auto turn = 2 * std::numbers::pi;
  if (s.type == SegmentType::ellipse && s.c1.x > 0 && s.c1.x == s.c1.y)
    return CircularPath{{s.p.x + s.c1.x / 2, s.p.y + s.c1.y / 2}, s.c1.x / 2, 0, turn, true};
  if (s.type != SegmentType::arc || s.radius <= 0) return {};
  auto sweep = s.end - s.start;
  if (s.clockwise && sweep < 0) sweep += std::ceil(-sweep / turn) * turn;
  if (!s.clockwise && sweep > 0) sweep -= std::ceil(sweep / turn) * turn;
  if (std::abs(sweep) > turn) return {};
  return CircularPath{s.p, s.radius, s.start, sweep};
}
// Smooth circular contours have no joins. The general miter-limit margin can
// be ten times too large for a gauge, wasting raster work and texture space.
inline double stroke_outset(const Layer& layer) {
  if (layer.path && circular_path(*layer.path))
    return layer.line_width * .5 * (layer.cap == Cap::square ? std::sqrt(2.) : 1.);
  return layer.line_width * .5 * std::max(1., layer.miter_limit);
}
}  // namespace headunit::theme::graphics
