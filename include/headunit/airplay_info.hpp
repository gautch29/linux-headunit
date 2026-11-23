// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/airplay_identity.hpp"
#include "headunit/presentation_status.hpp"

#include <cstdint>
#include <span>
#include <vector>
#include <string>
#include <optional>

namespace headunit {

struct AirplayDisplaySize {
  std::uint32_t width = 1280;
  std::uint32_t height = 720;
  std::uint32_t max_fps = 60;
  // Zero means unmeasured: omit physical-size claims from the advertisement.
  std::uint32_t width_mm = 0, height_mm = 0;
};

// Media-only AAC audio is optional. Its existing compatibility declaration also
// lists basic audio categories needed by the observed phone negotiation; this
// does not implement them. SETUP accepts only the supported AAC media downlink,
// and still rejects guidance/call/microphone streams.
std::vector<std::uint8_t> make_airplay_info(const AirplayIdentity& identity,
                                            AirplayDisplaySize main_display,
                                            AirplayDisplaySize cluster_display,
                                            bool native_ultra = false,
                                            const std::string& main_panel = "Center_Display",
                                            const std::string& cluster_panel = "Cluster_Display",
                                            bool media_audio = false, bool oem_icon = false);

// Before the phone decides, retain the complete capability offer. Once its
// initial SETUP selects Standard, describe that session's geometry and HID.
// Selection is per connection; it must not change the next phone's offer.
// Copy of a binary /info plist with root `nightMode` set. Its presence tells
// the phone that day/night appearance is supported; setNightMode updates it.
std::vector<std::uint8_t> with_night_mode(std::span<const std::uint8_t> info, bool night);
// Copy with root `limitedUI` and the restricted `limitedUIElements` (soft
// keyboard, phone keypad and long lists) declaring driving-restriction support.
std::vector<std::uint8_t> with_limited_ui(std::span<const std::uint8_t> info, bool limited);

const std::vector<std::uint8_t>& select_airplay_info(const std::vector<std::uint8_t>& capabilities,
                                                     const std::vector<std::uint8_t>& standard,
                                                     std::optional<CarplayMode> mode) noexcept;

}  // namespace headunit
