// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/touch_input.hpp>
#include <headunit/theme/scene.hpp>

namespace headunit::camera {
// Canvas coordinates, already fitted by PreviewTouch. Capture rotation and
// mirroring affect camera pixels, never this locally rendered control.
theme::graphics::Rect manual_close_frame(unsigned canvas_width, unsigned canvas_height);
class ManualCloseGesture {
 public:
  ManualCloseGesture(unsigned canvas_width, unsigned canvas_height);
  // Activate only on a new press inside the button. Motion from outside and
  // release/cancel reports cannot become a request. The caller owns routing.
  bool consume(const TouchInput&, bool enabled) noexcept;

 private:
  theme::graphics::Rect frame_;
  bool pressed_ = false;
};
}  // namespace headunit::camera
