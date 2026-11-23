// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/airplay_info.hpp>
#include <array>
#include <string>
#include <string_view>

namespace headunit {
struct DisplayConfiguration {
  AirplayDisplaySize size;
  std::string panel_name;
  std::string wayland_display;
  std::string output_name;                          // wl_output name, such as HDMI-A-1.
  std::uint32_t asset_width = 0, asset_height = 0;  // Zero: same as output pixels.
};
using ReceiverDisplays = std::array<DisplayConfiguration, 2>;
ReceiverDisplays default_receiver_displays();
ReceiverDisplays parse_display_configuration(std::string_view text);
ReceiverDisplays load_display_configuration(const std::string& path);
}  // namespace headunit
