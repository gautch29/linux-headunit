// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/iap2_message.hpp"
#include "headunit/route_guidance.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace headunit {

struct Iap2Identity {
  std::string name = "Headunit";
  std::string model = "UH-Prototype";
  std::string manufacturer = "Independent";
  std::string serial = "UH000001";
  std::string firmware = "0.1.0";
  std::string hardware = "Pi";
  bool supports_theme_assets = false;
  bool supports_now_playing = false;
  std::optional<RouteGuidanceConfig> route_guidance;
  std::array<std::uint8_t, 6> bluetooth_address{};
  std::vector<std::uint16_t> messages_sent;
  std::vector<std::uint16_t> messages_received;
};

ControlMessage make_identification_reply(const Iap2Identity& identity);

}  // namespace headunit
