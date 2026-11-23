// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_tlv.hpp"

#include <algorithm>
#include <stdexcept>

namespace headunit {
namespace {
constexpr std::size_t maximum_message_bytes = 65536;
constexpr std::uint8_t separator_type = 0xff;
}  // namespace

std::vector<HapTlv> decode_hap_tlv(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > maximum_message_bytes) {
    throw std::invalid_argument("HomeKit TLV message exceeds limit");
  }
  std::vector<HapTlv> fields;
  std::size_t cursor = 0;
  bool continuation_allowed = false;
  while (cursor < bytes.size()) {
    if (bytes.size() - cursor < 2) {
      throw std::invalid_argument("truncated HomeKit TLV header");
    }
    const auto type = bytes[cursor++];
    const auto length = bytes[cursor++];
    if (bytes.size() - cursor < length) {
      throw std::invalid_argument("truncated HomeKit TLV value");
    }
    if (type == separator_type && length != 0) {
      throw std::invalid_argument("HomeKit TLV separator must be empty");
    }
    if (continuation_allowed && !fields.empty() && fields.back().type == type &&
        type != separator_type) {
      auto& value = fields.back().value;
      if (value.size() + length > maximum_message_bytes) {
        throw std::invalid_argument("HomeKit TLV value exceeds limit");
      }
      value.insert(value.end(), bytes.begin() + cursor, bytes.begin() + cursor + length);
    } else {
      fields.push_back({type, {bytes.begin() + cursor, bytes.begin() + cursor + length}});
    }
    continuation_allowed = length == 255;
    cursor += length;
  }
  return fields;
}

std::vector<std::uint8_t> encode_hap_tlv(std::span<const HapTlv> fields) {
  std::vector<std::uint8_t> bytes;
  for (const auto& field : fields) {
    if (field.type == separator_type && !field.value.empty()) {
      throw std::invalid_argument("HomeKit TLV separator must be empty");
    }
    const auto chunks = field.value.empty() ? 1 : 1 + (field.value.size() - 1) / 255;
    const auto encoded_length = field.value.size() + 2 * chunks;
    if (field.value.size() > maximum_message_bytes ||
        encoded_length > maximum_message_bytes - bytes.size()) {
      throw std::invalid_argument("HomeKit TLV message exceeds limit");
    }
    if (field.value.empty()) {
      bytes.insert(bytes.end(), {field.type, 0});
      continue;
    }
    for (std::size_t offset = 0; offset < field.value.size();) {
      const auto count = std::min<std::size_t>(255, field.value.size() - offset);
      bytes.push_back(field.type);
      bytes.push_back(static_cast<std::uint8_t>(count));
      bytes.insert(bytes.end(), field.value.begin() + offset, field.value.begin() + offset + count);
      offset += count;
    }
  }
  return bytes;
}

}  // namespace headunit
