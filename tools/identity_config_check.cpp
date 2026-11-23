// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/airplay_identity.hpp>
#include <iostream>
int main(int argc, char** argv) try {
  if (argc != 2)
    throw std::invalid_argument("usage: headunit-identity-config-check PRIVATE_IDENTITY");
  auto identity = headunit::load_airplay_identity(argv[1]);
  auto accessory = headunit::make_iap2_identity(identity);
  accessory.bluetooth_address = {2, 0, 0, 0, 0, 1};  // Validate encoding without accessing a radio.
  (void)headunit::make_identification_reply(accessory);
  std::cout << "Receiver identity valid\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
