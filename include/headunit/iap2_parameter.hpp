// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

struct Iap2Parameter {
  std::uint16_t identifier = 0;
  std::vector<std::uint8_t> value;
  bool operator==(const Iap2Parameter&) const = default;
};

std::vector<Iap2Parameter> decode_iap2_parameters(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_iap2_parameters(std::span<const Iap2Parameter> parameters);

}  // namespace headunit
