// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_wifi_handoff.hpp"

#include "headunit/iap2_parameter.hpp"

#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace headunit {
namespace {
std::vector<std::uint8_t> terminated(const std::string& text) {
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  bytes.push_back(0);
  return bytes;
}
}  // namespace

Iap2WifiHandoff::Iap2WifiHandoff(Iap2WifiConfig config) : config_(std::move(config)) {
  if (config_.ssid.empty() || config_.ssid.size() > 32 ||
      config_.ssid.find('\0') != std::string::npos) {
    throw std::invalid_argument("Wi-Fi SSID must contain 1..32 non-NUL bytes");
  }
  if (config_.passphrase.size() < 8 || config_.passphrase.size() > 63 ||
      config_.passphrase.find('\0') != std::string::npos) {
    throw std::invalid_argument("Wi-Fi passphrase must contain 8..63 non-NUL bytes");
  }
  if (config_.channel == 0 || config_.security != 2) {
    throw std::invalid_argument("unsupported Wi-Fi channel or security mode");
  }
}

std::optional<ControlMessage> Iap2WifiHandoff::on_message(const ControlMessage& message) const {
  if (message.identifier != 0x5702) return std::nullopt;
  if (!message.parameters.empty()) {
    throw std::invalid_argument("unexpected Wi-Fi handoff parameters");
  }
  const std::array<Iap2Parameter, 4> parameters{{
      {1, terminated(config_.ssid)},
      {2, terminated(config_.passphrase)},
      {3, {config_.security}},
      {4, {config_.channel}},
  }};
  return ControlMessage{0x5703, encode_iap2_parameters(parameters)};
}

}  // namespace headunit
