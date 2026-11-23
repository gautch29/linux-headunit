// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <atomic>
#include <cstdint>

namespace headunit {
inline constexpr const char* touchscreen_hid_uuid = "7e73a100";
// One HID touch report. The first contact is always present; the second is
// used only when the receiver declares a two-finger digitizer.
struct TouchInput {
  bool pressed = false;
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  bool second_pressed = false;
  std::uint16_t second_x = 0;
  std::uint16_t second_y = 0;
};

// Receiver-wide choice, made once at startup before any /info or touch owner
// exists: a two-finger digitizer (pinch/rotate) instead of single touch.
inline std::atomic<bool>& multitouch_setting() {
  static std::atomic<bool> enabled{false};
  return enabled;
}
inline void set_multitouch(bool enabled) {
  multitouch_setting().store(enabled);
}
inline bool multitouch_enabled() {
  return multitouch_setting().load();
}
}  // namespace headunit
