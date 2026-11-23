// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace headunit {
struct PlistValue {
  using Bytes = std::vector<std::uint8_t>;
  using Array = std::vector<PlistValue>;
  using Dictionary = std::map<std::string, PlistValue, std::less<>>;
  std::variant<bool, std::int64_t, double, std::string, Bytes, Array, Dictionary> value;
  bool operator==(const PlistValue&) const = default;
};
// The UI subset: no date, UID, null, set or 128-bit integer objects.
// The decoder bounds the object graph before libplist allocates its tree.
inline constexpr std::size_t plist_wire_limit = 512 * 1024;
inline constexpr std::size_t plist_node_limit = 4096;
inline constexpr std::size_t plist_expansion_limit = 4 * 1024 * 1024;
inline constexpr unsigned plist_depth_limit = 32;
struct BinaryPlistLimits {
  std::size_t wire_bytes = plist_wire_limit;
  std::size_t nodes = plist_node_limit;
  std::size_t expanded_bytes = plist_expansion_limit;
  unsigned depth = plist_depth_limit;
};
// Structural validation before libplist allocation. DATA remains opaque; callers
// must validate a nested plist separately when interpreting its bytes as one.
void preflight_binary_plist(std::span<const std::uint8_t> input, BinaryPlistLimits limits = {});
PlistValue decode_binary_plist(std::span<const std::uint8_t> input);
PlistValue::Bytes encode_binary_plist(const PlistValue& value);
}  // namespace headunit
