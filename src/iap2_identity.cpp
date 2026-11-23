// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_identity.hpp"

#include "headunit/iap2_parameter.hpp"

#include <algorithm>
#include <stdexcept>

namespace headunit {
namespace {
std::vector<std::uint8_t> string_value(const std::string& value) {
  if (value.empty() || value.size() > 127 || value.find('\0') != std::string::npos) {
    throw std::invalid_argument("iAP2 identity string must contain 1..127 non-NUL bytes");
  }
  std::vector<std::uint8_t> bytes(value.begin(), value.end());
  bytes.push_back(0);
  return bytes;
}

std::vector<std::uint8_t> word(std::uint16_t value) {
  return {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
}

std::vector<std::uint8_t> message_list(const std::vector<std::uint16_t>& identifiers) {
  if (identifiers.size() > 128) throw std::invalid_argument("too many iAP2 message IDs");
  std::vector<std::uint8_t> bytes;
  bytes.reserve(identifiers.size() * 2);
  for (const auto id : identifiers) {
    bytes.push_back(static_cast<std::uint8_t>(id >> 8));
    bytes.push_back(static_cast<std::uint8_t>(id));
  }
  return bytes;
}
}  // namespace

ControlMessage make_identification_reply(const Iap2Identity& identity) {
  if (std::all_of(identity.bluetooth_address.begin(), identity.bluetooth_address.end(),
                  [](auto byte) { return byte == 0; })) {
    throw std::invalid_argument("Bluetooth address is required for iAP2 identity");
  }
  const std::array<Iap2Parameter, 6> strings{{
      {0, string_value(identity.name)},
      {1, string_value(identity.model)},
      {2, string_value(identity.manufacturer)},
      {3, string_value(identity.serial)},
      {4, string_value(identity.firmware)},
      {5, string_value(identity.hardware)},
  }};
  std::vector<Iap2Parameter> top(strings.begin(), strings.end());
  auto sent = identity.messages_sent, received = identity.messages_received;
  if (identity.supports_now_playing) {
    for (auto id : {0x5000, 0x5002})
      if (std::find(sent.begin(), sent.end(), id) == sent.end()) sent.push_back(id);
    if (std::find(received.begin(), received.end(), 0x5001) == received.end())
      received.push_back(0x5001);
  }
  if (identity.route_guidance) {
    for (auto id : {0x5200, 0x5203})
      if (std::find(sent.begin(), sent.end(), id) == sent.end()) sent.push_back(id);
    for (auto id : {0x5201, 0x5202})
      if (std::find(received.begin(), received.end(), id) == received.end()) received.push_back(id);
    top.push_back(route_guidance_component(*identity.route_guidance));
  }
  top.push_back({6, message_list(sent)});
  top.push_back({7, message_list(received)});
  top.push_back({8, {0}});  // No power supplied to the phone over Bluetooth.
  top.push_back({9, {0, 0}});
  top.push_back({12, string_value("en")});
  top.push_back({13, string_value("en")});

  const std::array<Iap2Parameter, 4> bluetooth{{
      {0, word(1)},
      {1, string_value("Radio")},
      {2, {}},
      {3, {identity.bluetooth_address.begin(), identity.bluetooth_address.end()}},
  }};
  top.push_back({17, encode_iap2_parameters(bluetooth)});

  std::vector<Iap2Parameter> wireless{
      {0, word(2)},
      {1, string_value("WiFi")},
      {2, {}},
      {4, {}},
  };
  if (identity.supports_theme_assets) wireless.push_back({5, {}});
  top.push_back({24, encode_iap2_parameters(wireless)});
  return {0x1d01, encode_iap2_parameters(top)};
}

}  // namespace headunit
