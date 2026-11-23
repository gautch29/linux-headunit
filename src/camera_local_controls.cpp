// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_local_controls.hpp>
#include <algorithm>
#include <stdexcept>

namespace headunit::camera {
theme::graphics::Rect manual_close_frame(unsigned width, unsigned height) {
  if (width < 320 || width > 4096 || height < 240 || height > 2160)
    throw std::invalid_argument("invalid native camera control canvas");
  const double button_width = std::min(210., double(width) - 48.);
  return {{double(width) - button_width - 24, 24}, {button_width, 64}};
}
ManualCloseGesture::ManualCloseGesture(unsigned width, unsigned height)
    : frame_(manual_close_frame(width, height)) {}
bool ManualCloseGesture::consume(const TouchInput& input, bool enabled) noexcept {
  const bool initial = input.pressed && !pressed_;
  pressed_ = input.pressed;
  return enabled && initial && input.x >= frame_.origin.x && input.y >= frame_.origin.y &&
         input.x < frame_.origin.x + frame_.size.width &&
         input.y < frame_.origin.y + frame_.size.height;
}
}  // namespace headunit::camera
