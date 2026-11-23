// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/mfi_coprocessor.hpp"
#include "headunit/airplay_auth_setup.hpp"
#include "headunit/hap_crypto.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct FakeBus final : headunit::MfiRegisterBus {
  std::map<std::uint8_t, std::vector<std::uint8_t>> registers;
  std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> writes;
  FakeBus() {
    registers[0x00] = {7};
    registers[0x01] = {2};
    registers[0x02] = {3};
    registers[0x03] = {0};
    registers[0x05] = {0};
    registers[0x10] = {0x10};
    registers[0x11] = {0, 64};
    registers[0x12] = std::vector<std::uint8_t>(64, 0x5a);
    registers[0x30] = {0x02, 0x5f};
    registers[0x31] = std::vector<std::uint8_t>(607, 0x5b);
  }
  void read(std::uint8_t reg, std::span<std::uint8_t> out) override {
    const auto& value = registers.at(reg);
    check(value.size() >= out.size(), "fake MFi register is too short");
    std::copy_n(value.begin(), out.size(), out.begin());
  }
  void write(std::uint8_t reg, std::span<const std::uint8_t> value) override {
    writes.emplace_back(reg, std::vector<std::uint8_t>(value.begin(), value.end()));
  }
};

std::array<std::uint8_t, 20> sha1_key(std::string_view prefix,
                                      std::span<const std::uint8_t> shared) {
  std::vector<std::uint8_t> input(prefix.begin(), prefix.end());
  input.insert(input.end(), shared.begin(), shared.end());
  std::array<std::uint8_t, 20> output{};
  unsigned size = 0;
  check(EVP_Digest(input.data(), input.size(), output.data(), &size, EVP_sha1(), nullptr) == 1 &&
            size == output.size(),
        "test SHA-1 failed");
  return output;
}

std::array<std::uint8_t, 32> sha256_keys(std::span<const std::uint8_t> first,
                                         std::span<const std::uint8_t> second) {
  std::vector<std::uint8_t> input(first.begin(), first.end());
  input.insert(input.end(), second.begin(), second.end());
  std::array<std::uint8_t, 32> output{};
  unsigned size = 0;
  check(EVP_Digest(input.data(), input.size(), output.data(), &size, EVP_sha256(), nullptr) == 1 &&
            size == output.size(),
        "test SHA-256 failed");
  return output;
}

std::uint32_t be32(std::span<const std::uint8_t> bytes) {
  check(bytes.size() == 4, "invalid response length field");
  return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
         (std::uint32_t(bytes[2]) << 8) | bytes[3];
}
}  // namespace

int main() try {
  FakeBus bus;
  headunit::MfiCoprocessor chip(bus);
  headunit::AirplayAuthSetup receiver(chip);
  headunit::HapX25519Ephemeral controller;
  const auto controller_public = controller.public_key();
  std::array<std::uint8_t, 33> request{};
  request[0] = 1;
  std::copy(controller_public.begin(), controller_public.end(), request.begin() + 1);
  const auto response = receiver.respond(request);
  check(response.size() == 711, "unexpected MFiSAP response length");
  const headunit::HapKey32 accessory_public = [&] {
    headunit::HapKey32 key{};
    std::copy_n(response.begin(), key.size(), key.begin());
    return key;
  }();
  const auto shared = controller.shared_secret(accessory_public);
  check(shared.has_value(), "test X25519 failed");
  check(be32(std::span(response).subspan(32, 4)) == 607 &&
            std::all_of(response.begin() + 36, response.begin() + 643,
                        [](auto byte) { return byte == 0x5b; }),
        "MFi certificate or its length is wrong");
  check(be32(std::span(response).subspan(643, 4)) == 64, "MFi signature length is wrong");
  const auto key = sha1_key("AES-KEY", *shared);
  const auto iv = sha1_key("AES-IV", *shared);
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> decryptor(EVP_CIPHER_CTX_new(),
                                                                            EVP_CIPHER_CTX_free);
  std::array<std::uint8_t, 64> signature{};
  int written = 0;
  int final = 0;
  check(decryptor &&
            EVP_DecryptInit_ex(decryptor.get(), EVP_aes_128_ctr(), nullptr, key.data(),
                               iv.data()) == 1 &&
            EVP_DecryptUpdate(decryptor.get(), signature.data(), &written, response.data() + 647,
                              64) == 1 &&
            EVP_DecryptFinal_ex(decryptor.get(), signature.data() + written, &final) == 1 &&
            written + final == 64 &&
            std::all_of(signature.begin(), signature.end(), [](auto byte) { return byte == 0x5a; }),
        "MFi signature encryption is wrong");
  const auto expected_challenge = sha256_keys(accessory_public, controller_public);
  check(bus.writes.size() == 3 && bus.writes[1].first == 0x21 &&
            bus.writes[1].second ==
                std::vector<std::uint8_t>(expected_challenge.begin(), expected_challenge.end()),
        "MFi chip received the wrong public-key digest");

  for (const auto& malformed : {std::vector<std::uint8_t>(32, 1), std::vector<std::uint8_t>(33, 0),
                                std::vector<std::uint8_t>{1}}) {
    bool rejected = false;
    try {
      (void)receiver.respond(malformed);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "malformed auth-setup request accepted");
  }
  std::array<std::uint8_t, 33> zero_peer{};
  zero_peer[0] = 1;
  bool rejected = false;
  try {
    (void)receiver.respond(zero_peer);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "zero X25519 key accepted");
  bus.registers[0x02] = {2};
  rejected = false;
  try {
    (void)receiver.respond(request);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "unsupported MFi chip version accepted");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
