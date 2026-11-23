// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_audio_packet.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("audio packet test failed");
}

std::vector<std::uint8_t> seal(std::span<const std::uint8_t> secret, std::uint64_t nonce_value,
                               std::uint32_t sample_time) {
  const auto key =
      headunit::hap_hkdf_sha512(secret, "DataStream-Salt42", "DataStream-Output-Encryption-Key");
  std::array<std::uint8_t, 12> header{0x80,
                                      0x60,
                                      0x01,
                                      0x02,
                                      std::uint8_t(sample_time >> 24),
                                      std::uint8_t(sample_time >> 16),
                                      std::uint8_t(sample_time >> 8),
                                      std::uint8_t(sample_time),
                                      1,
                                      2,
                                      3,
                                      4};
  std::array<std::uint8_t, 8> nonce{};
  for (std::size_t index = 0; index < 8; ++index)
    nonce[index] = std::uint8_t(nonce_value >> (index * 8));
  const std::string nonce_text(reinterpret_cast<const char*>(nonce.data()), nonce.size());
  const std::array<std::uint8_t, 4> payload{'t', 'e', 's', 't'};
  auto encrypted = headunit::hap_encrypt(key, nonce_text, payload, std::span(header).subspan(4, 8));
  std::vector<std::uint8_t> result(header.begin(), header.end());
  result.insert(result.end(), encrypted.begin(), encrypted.end());
  result.insert(result.end(), nonce.begin(), nonce.end());
  return result;
}
}  // namespace

int main() {
  const std::array<std::uint8_t, 32> secret{};
  headunit::AirplayAudioPackets receiver(secret, 42);
  auto first = seal(secret, 10, 48000);
  auto opened = receiver.open(first);
  check(opened && opened->sequence == 0x102 && opened->sample_time == 48000 &&
        opened->bytes.size() == 16 && opened->bytes[12] == 't');
  check(!receiver.open(first));
  auto corrupt = seal(secret, 11, 49000);
  corrupt[12] ^= 1;
  check(!receiver.open(corrupt));
  check(receiver.open(seal(secret, 12, 50000)).has_value());
  check(receiver.open(seal(secret, 11, 49000)).has_value());
  check(!receiver.open(seal(secret, 11, 49000)));
  check(!receiver.open({}));
  return 0;
}
