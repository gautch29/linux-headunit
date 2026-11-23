// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/native_hud.hpp>
#include <string>
#include <string_view>

namespace headunit {
// A local output, independent of the two phone-advertised signed panels.
struct HudConfiguration {
  std::string output_name;
  std::string wayland_display = "wayland-0";
  std::string main_output = "HDMI-A-1", cluster_output = "HDMI-A-2";
  unsigned width = 960, height = 360, fps = 20;
  std::string country{};  // Empty/auto: await provider; explicit fallback otherwise.
  HudCalibration calibration;
};
HudConfiguration parse_hud_configuration(std::string_view);
HudConfiguration load_hud_configuration(const std::string& path);
}  // namespace headunit
