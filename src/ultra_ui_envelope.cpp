// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/ultra_ui_envelope.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace headunit {
namespace {
constexpr std::size_t payload_limit = 512 * 1024;
constexpr std::array<std::uint8_t, 8> plist_magic{'b', 'p', 'l', 'i', 's', 't', '0', '0'};

bool uuid(const std::string& value) {
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto c = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}
void validate(const UltraUiEnvelope& message) {
  // Physical panel IDs come from the signed asset and need not be UUIDs.
  const auto& id = message.display_uuid;
  if (id.empty() || id.size() > 128 ||
      id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
          id.npos ||
      message.binary_plist.size() < plist_magic.size() ||
      message.binary_plist.size() > payload_limit ||
      !std::equal(plist_magic.begin(), plist_magic.end(), message.binary_plist.begin()))
    throw std::invalid_argument("invalid Ultra UI envelope");
}
char lower_hex(char c) {
  return c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c;
}
bool same_uuid(const std::string& a, const std::string& b) {
  if (!uuid(a) || !uuid(b)) return a == b;
  return std::equal(a.begin(), a.end(), b.begin(),
                    [](char x, char y) { return lower_hex(x) == lower_hex(y); });
}
}  // namespace

OpackValue::Bytes encode_ultra_ui_envelope(const UltraUiEnvelope& message) {
  validate(message);
  return encode_opack(
      OpackValue{OpackValue::Dictionary{{"displayUUID", OpackValue{message.display_uuid}},
                                        {"data", OpackValue{message.binary_plist}}}});
}

UltraUiEnvelope decode_ultra_ui_envelope(std::span<const std::uint8_t> message,
                                         std::span<const std::string> configured_displays) {
  const auto object = decode_opack(message);
  const auto* dictionary = std::get_if<OpackValue::Dictionary>(&object.value);
  if (!dictionary || dictionary->size() != 2 || !dictionary->contains("displayUUID") ||
      !dictionary->contains("data"))
    throw std::invalid_argument("invalid Ultra UI envelope fields");
  const auto* id = std::get_if<std::string>(&dictionary->at("displayUUID").value);
  const auto* data = std::get_if<OpackValue::Bytes>(&dictionary->at("data").value);
  if (!id || !data) throw std::invalid_argument("invalid Ultra UI envelope types");
  UltraUiEnvelope result{*id, *data};
  validate(result);
  const auto found = std::find_if(configured_displays.begin(), configured_displays.end(),
                                  [&](const auto& expected) { return same_uuid(*id, expected); });
  if (found == configured_displays.end()) throw std::invalid_argument("unknown Ultra UI display");
  result.display_uuid = *found;
  return result;
}
}  // namespace headunit
