// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_control_frame.hpp"
#include "headunit/hap_crypto.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
std::string_view nonce(std::uint64_t count, std::array<char, 8>& bytes) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>(count >> (8 * i));
  }
  return {bytes.data(), bytes.size()};
}
}  // namespace

int main() try {
  headunit::HapKey32 shared{};
  for (std::size_t i = 0; i < shared.size(); ++i)
    shared[i] = i;
  headunit::HapControlFrame receiver(shared);
  const auto phone_key =
      headunit::hap_hkdf_sha512(shared, "Control-Salt", "Control-Write-Encryption-Key");
  const auto receiver_key =
      headunit::hap_hkdf_sha512(shared, "Control-Salt", "Control-Read-Encryption-Key");
  std::vector<std::uint8_t> original(1300, 0x5a);
  std::vector<std::uint8_t> encrypted;
  for (std::size_t offset = 0, count = 0; offset < original.size(); ++count) {
    const auto length = std::min<std::size_t>(1024, original.size() - offset);
    const std::array<std::uint8_t, 2> header{static_cast<std::uint8_t>(length),
                                             static_cast<std::uint8_t>(length >> 8)};
    std::array<char, 8> iv{};
    const auto body = headunit::hap_encrypt(phone_key, nonce(count, iv),
                                            {original.data() + offset, length}, header);
    encrypted.insert(encrypted.end(), header.begin(), header.end());
    encrypted.insert(encrypted.end(), body.begin(), body.end());
    offset += length;
  }
  std::vector<std::uint8_t> recovered;
  for (std::size_t offset = 0; offset < encrypted.size(); offset += 73) {
    const auto length = std::min<std::size_t>(73, encrypted.size() - offset);
    const auto chunk = receiver.receive({encrypted.data() + offset, length});
    recovered.insert(recovered.end(), chunk.begin(), chunk.end());
  }
  check(recovered == original, "fragmented HomeKit frames failed");

  const auto outgoing = receiver.encode(original);
  recovered.clear();
  std::size_t offset = 0;
  for (std::uint64_t counter = 0; offset < outgoing.size(); ++counter) {
    const auto length = static_cast<std::size_t>(outgoing[offset]) |
                        (static_cast<std::size_t>(outgoing[offset + 1]) << 8);
    check(length <= 1024 && offset + 2 + length + 16 <= outgoing.size(),
          "invalid outgoing HomeKit frame shape");
    std::array<char, 8> iv{};
    const auto body = headunit::hap_decrypt(receiver_key, nonce(counter, iv),
                                            {outgoing.data() + offset + 2, length + 16},
                                            {outgoing.data() + offset, 2});
    check(body.has_value(), "receiver HomeKit frame rejected by peer");
    recovered.insert(recovered.end(), body->begin(), body->end());
    offset += length + 18;
  }
  check(recovered == original, "outgoing HomeKit plaintext mismatch");
  check(headunit::HapControlFrame::encoded_size(0) == 0 &&
            headunit::HapControlFrame::encoded_size(1024) == 1042 &&
            headunit::HapControlFrame::encoded_size(1025) == 1061 &&
            headunit::HapControlFrame::encoded_size(65536) == 66688,
        "HomeKit framing admission cost is incorrect");

  // A maximum permitted feed may complete a retained frame and carry further
  // records. Its budget is independent of the preceding incomplete frame.
  headunit::HapControlFrame boundary(shared);
  const std::vector<std::uint8_t> maximum_plain(65536, 0x6b);
  std::vector<std::uint8_t> maximum_wire;
  for (std::size_t at = 0; at < maximum_plain.size(); at += 1024) {
    const std::array<std::uint8_t, 2> header{0, 4};
    std::array<char, 8> iv{};
    const auto body = headunit::hap_encrypt(phone_key, nonce(at / 1024, iv),
                                            std::span(maximum_plain).subspan(at, 1024), header);
    maximum_wire.insert(maximum_wire.end(), header.begin(), header.end());
    maximum_wire.insert(maximum_wire.end(), body.begin(), body.end());
  }
  check(maximum_wire.size() == 66688, "maximum HomeKit wire size changed");
  auto maximum_recovered = boundary.receive(std::span(maximum_wire).first(1152));
  const auto maximum_rest = boundary.receive(std::span(maximum_wire).subspan(1152));
  maximum_recovered.insert(maximum_recovered.end(), maximum_rest.begin(), maximum_rest.end());
  check(maximum_recovered == maximum_plain, "HomeKit feed crossing retained frame rejected");
  check(boundary.encode(maximum_plain).size() == maximum_wire.size(),
        "maximum HomeKit encoder exceeded admission cost");

  headunit::HapControlFrame invalid(shared);
  auto tampered = encrypted;
  tampered[18] ^= 1;
  bool rejected = false;
  try {
    (void)invalid.receive(tampered);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "tampered HomeKit frame accepted");
  rejected = false;
  try {
    (void)invalid.receive(encrypted);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "failed HomeKit frame decoder resumed");

  headunit::HapControlFrame oversized(shared);
  rejected = false;
  const std::array<std::uint8_t, 2> bad_length{1, 4};
  try {
    (void)oversized.receive(bad_length);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "oversized HomeKit frame accepted");
  check(oversized.failed(), "oversized HomeKit frame did not retire keys");
  headunit::HapControlFrame budget(shared);
  rejected = false;
  try {
    (void)budget.receive(std::vector<std::uint8_t>(65537));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected && budget.failed(), "HomeKit input budget fault was recoverable");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
