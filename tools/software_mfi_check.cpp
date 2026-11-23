// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/software_mfi_authenticator.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) try {
  if (argc != 3)
    throw std::invalid_argument("usage: headunit-software-mfi-check PRIVATE.pk8 CERTIFICATE.p7b");
  headunit::SoftwareMfiAuthenticator authentication(argv[1], argv[2]);
  const std::array<std::uint8_t, 32> digest{};
  (void)authentication.sign_digest(digest);
  std::cout << "Software MFi identity consistent; protocol=3; certificate_bytes="
            << authentication.read_certificate().size() << "; phone acceptance unverified\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
