// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

struct HapTlv {
  std::uint8_t type = 0;
  std::vector<std::uint8_t> value;

  bool operator==(const HapTlv&) const = default;
};

// HomeKit/AirPlay TLV8 fields use one-byte type and length. A value longer
// than 255 bytes continues in adjacent fields with the same type.
std::vector<HapTlv> decode_hap_tlv(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_hap_tlv(std::span<const HapTlv> fields);

}  // namespace headunit
