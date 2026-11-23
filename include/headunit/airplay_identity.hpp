// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/iap2_identity.hpp"
#include <cstdint>
#include <string>

namespace headunit {

struct AirplayIdentity {
  std::string name;
  std::string device_id;
  std::string features_text;
  std::string flags_text;
  std::string model;
  std::string protocol_version;
  std::string source_version;
  std::string pairing_id;
  std::string public_key_hex;
  std::string main_display_id;
  std::string cluster_display_id;
  std::uint64_t features = 0;
  std::uint64_t flags = 0;
  std::string manufacturer = "Independent";
  std::string serial = "UH000001";
  std::string firmware = "0.1.0";
  std::string hardware = "Pi";
  std::string touchscreen_name = "Independent Touchscreen";
};

Iap2Identity make_iap2_identity(const AirplayIdentity& identity);

AirplayIdentity load_airplay_identity(const std::string& path);

}  // namespace headunit
