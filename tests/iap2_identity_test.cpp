// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_identity.hpp"
#include "headunit/iap2_parameter.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() try {
  headunit::Iap2Identity identity;
  identity.name = "Independent Test";
  identity.bluetooth_address = {0xe4, 0x5f, 0x01, 0x27, 0xe1, 0x09};
  identity.messages_sent = {0x5703};
  identity.messages_received = {0x5702};
  const auto reply = headunit::make_identification_reply(identity);
  check(reply.identifier == 0x1d01, "identification message ID");
  const auto fields = headunit::decode_iap2_parameters(reply.parameters);
  check(fields.size() == 14, "unexpected top-level identity shape");
  check(fields[0].value.back() == 0 && fields[0].value[0] == 'I', "name was not NUL terminated");
  check(fields[6].identifier == 6 && fields[6].value == std::vector<std::uint8_t>({0x57, 0x03}),
        "outgoing message list was not encoded");
  check(fields[7].identifier == 7 && fields[7].value == std::vector<std::uint8_t>({0x57, 0x02}),
        "incoming message list was not encoded");
  const auto bluetooth = headunit::decode_iap2_parameters(fields[12].value);
  check(bluetooth.size() == 4 && bluetooth[3].identifier == 3 &&
            bluetooth[3].value == std::vector<std::uint8_t>(identity.bluetooth_address.begin(),
                                                            identity.bluetooth_address.end()),
        "Bluetooth component did not contain adapter address");
  const auto wireless = headunit::decode_iap2_parameters(fields[13].value);
  check(wireless.size() == 4 && wireless[3].identifier == 4 && wireless[3].value.empty(),
        "wireless component is missing CarPlay support flag");

  identity.bluetooth_address = {};
  bool rejected = false;
  try {
    (void)headunit::make_identification_reply(identity);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "empty Bluetooth address was accepted");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
