// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/iap2_message.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace headunit {

struct Iap2WifiConfig {
  std::string ssid;
  std::string passphrase;
  std::uint8_t channel = 36;
  std::uint8_t security = 2;
};

// Builds the Wireless iAP network reply after MFi authentication. Provisioning
// and running the access point are separate responsibilities.
class Iap2WifiHandoff {
 public:
  explicit Iap2WifiHandoff(Iap2WifiConfig config);
  std::optional<ControlMessage> on_message(const ControlMessage& message) const;

 private:
  Iap2WifiConfig config_;
};

}  // namespace headunit
