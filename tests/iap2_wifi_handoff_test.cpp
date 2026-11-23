// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_wifi_handoff.hpp"
#include "headunit/iap2_parameter.hpp"

#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() {
  const headunit::Iap2WifiHandoff handoff({"test-ap", "testpassphrase", 36, 2});
  check(!handoff.on_message({0x1d00, {}}), "unrelated message generated a reply");
  const auto reply = handoff.on_message({0x5702, {}});
  check(reply && reply->identifier == 0x5703, "wrong Wi-Fi reply ID");
  const auto fields = headunit::decode_iap2_parameters(reply->parameters);
  check(fields.size() == 4, "wrong Wi-Fi field count");
  check(fields[0].identifier == 1 && fields[0].value.back() == 0 && fields[0].value.size() == 8,
        "SSID field shape");
  check(fields[1].identifier == 2 && fields[1].value.back() == 0 && fields[1].value.size() == 15,
        "passphrase field shape");
  check(fields[2].identifier == 3 && fields[2].value == std::vector<std::uint8_t>{2},
        "security field");
  check(fields[3].identifier == 4 && fields[3].value == std::vector<std::uint8_t>{36},
        "channel field");
  bool rejected = false;
  try {
    (void)handoff.on_message({0x5702, {1}});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "nonempty Wi-Fi request accepted");
  return 0;
}
