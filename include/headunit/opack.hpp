// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace headunit {

// The OPACK subset used by Ultra dictionaries. UUID/date objects, 128-bit
// integers and fragmented data are rejected until independently verified.
struct OpackValue {
  using Bytes = std::vector<std::uint8_t>;
  using Array = std::vector<OpackValue>;
  using Dictionary = std::map<std::string, OpackValue, std::less<>>;
  std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes, Array, Dictionary>
      value;
  bool operator==(const OpackValue&) const = default;
};

struct OpackLimits {
  std::size_t wire_bytes = 1024 * 1024;
  std::size_t expanded_bytes = 4 * 1024 * 1024;
  std::size_t nodes = 4096;
  unsigned depth = 32;
};

OpackValue decode_opack(std::span<const std::uint8_t> input, OpackLimits limits = {});
std::vector<std::uint8_t> encode_opack(const OpackValue& value, OpackLimits limits = {});

}  // namespace headunit
