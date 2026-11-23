// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_tlv.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() try {
  const std::vector<headunit::HapTlv> fields{
      {6, {1}}, {0, {0}}, {3, std::vector<std::uint8_t>(384, 0x5a)}, {0xff, {}}, {3, {7}},
  };
  const auto encoded = headunit::encode_hap_tlv(fields);
  check(encoded[0] == 6 && encoded[1] == 1 && encoded[6] == 3 && encoded[7] == 255,
        "TLV8 header or long-value split changed");
  check(headunit::decode_hap_tlv(encoded) == fields,
        "TLV8 long value or separator round-trip failed");

  bool rejected = false;
  try {
    (void)headunit::encode_hap_tlv(
        std::vector<headunit::HapTlv>{{3, std::vector<std::uint8_t>(65030, 0)}});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "fragment headers bypassed TLV8 message limit");

  rejected = false;
  try {
    (void)headunit::decode_hap_tlv({encoded.data(), encoded.size() - 1});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "truncated TLV8 accepted");

  rejected = false;
  const std::vector<std::uint8_t> bad_separator{0xff, 1, 0};
  try {
    (void)headunit::decode_hap_tlv(bad_separator);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "nonempty TLV8 separator accepted");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
