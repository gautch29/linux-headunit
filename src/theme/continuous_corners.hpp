// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/theme/scene.hpp>
#include <algorithm>
#include <array>

namespace headunit::theme::graphics {
// Original bounded approximation, shared by Cairo and Skia. Two cubics per
// corner meet with equal tangent and curvature; curvature reaches zero at each
// straight edge. This is not a pixel-identical Core Animation corner model.
inline Path continuous_rectangle(Rect rect, double radius) {
  const auto r = std::clamp(radius, 0., std::min(rect.size.width, rect.size.height) / 2);
  const auto left = rect.origin.x, top = rect.origin.y;
  const auto right = left + rect.size.width, bottom = top + rect.size.height;
  Path result;
  result.append({SegmentType::move, {left + r, top}});
  const std::array<Transform, 4> corners{{
      {r, 0, 0, r, right - r, top},
      {0, r, -r, 0, right, bottom - r},
      {-r, 0, 0, -r, left + r, bottom},
      {0, -r, r, 0, left, top + r},
  }};
  for (const auto& corner : corners) {
    result.append({SegmentType::line, corner.apply({0, 0})});
    if (r > 0) {
      result.append({SegmentType::cubic, corner.apply({.8, .2}), corner.apply({.32, 0}),
                     corner.apply({.6, 0})});
      result.append({SegmentType::cubic, corner.apply({1, 1}), corner.apply({1, .4}),
                     corner.apply({1, .68})});
    }
  }
  result.append({SegmentType::close});
  return result;
}
}  // namespace headunit::theme::graphics
