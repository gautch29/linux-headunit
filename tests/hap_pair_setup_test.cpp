// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_pair_setup.hpp"
#include "headunit/hap_tlv.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
const headunit::HapTlv* field(const std::vector<headunit::HapTlv>& fields, std::uint8_t type) {
  for (const auto& item : fields)
    if (item.type == type) return &item;
  return nullptr;
}
}  // namespace

int main() try {
  headunit::HapPairSetup pairing("123-45-678");
  const auto m1 =
      headunit::encode_hap_tlv(std::vector<headunit::HapTlv>{{6, {1}}, {0, {0}}, {0x13, {0x10}}});
  const auto m2 = headunit::decode_hap_tlv(pairing.respond(m1));
  check(field(m2, 6) && field(m2, 6)->value == std::vector<std::uint8_t>{2},
        "pair-setup M2 state missing");
  check(field(m2, 2) && field(m2, 2)->value.size() == 16, "pair-setup salt missing");
  check(field(m2, 3) && field(m2, 3)->value.size() <= 384 && !field(m2, 3)->value.empty() &&
            field(m2, 3)->value[0] != 0,
        "pair-setup public key missing");
  check(!pairing.proof_verified(), "pairing succeeded without controller proof");

  bool rejected = false;
  try {
    (void)pairing.respond(m1);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "repeated M1 accepted");
  rejected = false;
  const auto bad_m3 =
      headunit::encode_hap_tlv(std::vector<headunit::HapTlv>{{6, {3}}, {3, {1}}, {4, {0}}});
  try {
    (void)pairing.respond(bad_m3);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected && !pairing.proof_verified(), "invalid M3 proof accepted");

  headunit::HapPairSetup malformed("123-45-678");
  rejected = false;
  const auto duplicate_state =
      headunit::encode_hap_tlv(std::vector<headunit::HapTlv>{{6, {1}}, {6, {1}}, {0, {0}}});
  try {
    (void)malformed.respond(duplicate_state);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "duplicate pairing state accepted");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
