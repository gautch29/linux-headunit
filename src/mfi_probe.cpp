// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/linux_mfi_bus.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3 && argc != 4) {
    std::cerr << "usage: headunit-mfi-probe /dev/i2c-N 0xADDRESS "
                 "[--certificate-length|--sign-self-test]\n";
    return 2;
  }
  try {
    const std::string option = argc == 4 ? argv[3] : "";
    if (argc == 4 && option != "--certificate-length" && option != "--sign-self-test") {
      throw std::invalid_argument("unknown MFi probe option");
    }
    const auto parsed = std::stoul(argv[2], nullptr, 0);
    if (parsed > 0x7f) throw std::invalid_argument("I2C address must be 7-bit");
    headunit::LinuxMfiBus bus(argv[1], static_cast<std::uint8_t>(parsed));
    headunit::MfiCoprocessor chip(bus);
    const auto info = chip.read_info();
    std::cout << "device_version=" << static_cast<unsigned>(info.device_version)
              << " auth_revision=" << static_cast<unsigned>(info.authentication_revision)
              << " protocol=" << static_cast<unsigned>(info.protocol_major) << '.'
              << static_cast<unsigned>(info.protocol_minor) << '\n';
    if (option == "--certificate-length") {
      const auto certificate = chip.read_certificate();
      std::cout << "certificate_bytes=" << certificate.size() << '\n';
    }
    if (option == "--sign-self-test") {
      const std::array<std::uint8_t, 32> synthetic_digest{};
      const auto signature = chip.sign_digest(synthetic_digest);
      std::cout << "signature_bytes=" << signature.size() << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << "MFi probe failed: " << error.what() << '\n';
    return 1;
  }
}
