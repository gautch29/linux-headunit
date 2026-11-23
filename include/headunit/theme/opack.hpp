// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/protocol.hpp>
#include <array>
#include <variant>

namespace headunit::theme::opack {
struct Value;
struct DictionaryEntry;
using Array = std::vector<Value>;
// Not std::pair: Clang with libstdc++ evaluates pair's constructor constraints
// recursively through Value's variant and rejects the incomplete type.
using Dictionary = std::vector<DictionaryEntry>;
struct UUID {
  std::array<std::uint8_t, 16> bytes;
  bool operator==(const UUID&) const = default;
};
// Preserve the date payload without interpreting its epoch or precision.
struct DateBits {
  std::uint64_t bits;
  bool operator==(const DateBits&) const = default;
};
struct Value {
  using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes, UUID,
                               DateBits, Array, Dictionary>;
  Storage data;
  bool operator==(const Value&) const = default;
};
struct DictionaryEntry {
  Value first;
  Value second;
  bool operator==(const DictionaryEntry&) const = default;
};
// Bounded OPACK subset used by the SDK's vehicle-data dictionary codec. Accepts
// scalar references; does not interpret commands or provide a vehicle service.
Value decode(std::span<const std::uint8_t> wire);
Bytes encode(const Value& value);
inline constexpr std::size_t max_depth = 32;
inline constexpr std::size_t max_nodes = 16384;
inline constexpr std::size_t max_expanded_bytes = 4 * max_packet_bytes;
}  // namespace headunit::theme::opack
