// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_iap_stream.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition) {
  if (!condition) throw std::runtime_error("AirPlay iAP stream test failed");
}

std::vector<std::uint8_t> package(std::span<const std::uint8_t> message) {
  std::vector<std::uint8_t> bytes(32 + message.size());
  const auto size = static_cast<std::uint32_t>(bytes.size());
  for (unsigned index = 0; index < 4; ++index) {
    bytes[index] = static_cast<std::uint8_t>(size >> ((3 - index) * 8));
  }
  bytes[16] = 'c';
  bytes[17] = 'o';
  bytes[18] = 'm';
  bytes[19] = 'm';
  for (std::size_t index = 0; index < message.size(); ++index) {
    bytes[index + 32] = message[index];
  }
  return bytes;
}

std::vector<std::uint8_t> frame(const headunit::HapKey32& key, std::uint64_t counter,
                                std::span<const std::uint8_t> clear) {
  std::array<std::uint8_t, 2> length{static_cast<std::uint8_t>(clear.size()),
                                     static_cast<std::uint8_t>(clear.size() >> 8)};
  std::array<char, 8> nonce{};
  for (unsigned index = 0; index < 8; ++index) {
    nonce[index] = static_cast<char>(counter >> (index * 8));
  }
  const auto sealed = headunit::hap_encrypt(key, {nonce.data(), nonce.size()}, clear, length);
  std::vector<std::uint8_t> wire(length.begin(), length.end());
  wire.insert(wire.end(), sealed.begin(), sealed.end());
  return wire;
}
}  // namespace

int main() {
  std::array<std::uint8_t, 32> secret{};
  for (unsigned index = 0; index < secret.size(); ++index)
    secret[index] = index;
  const auto key =
      headunit::hap_hkdf_sha512(secret, "DataStream-Salt19", "DataStream-Output-Encryption-Key");
  headunit::AirplayIapStream reader(secret, 19);
  const std::vector<std::uint8_t> first_message{0xff, 0x5a, 0x01};
  const std::vector<std::uint8_t> second_message{0xff, 0x5a, 0x02, 0x03};
  const auto first_package = package(first_message);
  const auto second_package = package(second_message);
  const auto first_frame = frame(key, 0, {first_package.data(), 11});
  std::vector<std::uint8_t> remainder(first_package.begin() + 11, first_package.end());
  remainder.insert(remainder.end(), second_package.begin(), second_package.end());
  const auto second_frame = frame(key, 1, remainder);
  std::vector<std::uint8_t> wire = first_frame;
  wire.insert(wire.end(), second_frame.begin(), second_frame.end());
  check(reader.feed({wire.data(), 3}).empty());
  const auto messages = reader.feed({wire.data() + 3, wire.size() - 3});
  check(messages.size() == 2 && messages[0] == first_message && messages[1] == second_message &&
        reader.messages() == 2 && reader.message_bytes() == 7);
  check(reader.feed({wire.data(), 3}).empty());
  reader.retire();
  reader.retire();
  check(reader.retired() && reader.messages() == 2 && reader.message_bytes() == 7);
  bool closed_rejected = false;
  try {
    (void)reader.feed({});
  } catch (const std::invalid_argument&) {
    closed_rejected = true;
  }
  check(closed_rejected);

  // Completing the largest allowed package and starting the next package in
  // one TCP read must not accumulate both before draining the first one.
  headunit::AirplayIapStream burst(secret, 19);
  const std::vector<std::uint8_t> large_message(4 * 1024 * 1024 - 32, 0x5a);
  const auto large_package = package(large_message);
  std::uint64_t counter = 0;
  for (std::size_t offset = 0; offset < large_package.size() - 16384; offset += 16384) {
    check(
        burst.feed(frame(key, counter++, std::span(large_package).subspan(offset, 16384))).empty());
  }
  auto completion = frame(key, counter++, std::span(large_package).last(16384));
  const std::vector<std::uint8_t> following_message(32768 - 32, 0x6b);
  const auto following_package = package(following_message);
  for (std::size_t offset = 0; offset < following_package.size(); offset += 16384) {
    const auto next = frame(key, counter++, std::span(following_package).subspan(offset, 16384));
    completion.insert(completion.end(), next.begin(), next.end());
  }
  const auto burst_messages = burst.feed(completion);
  check(burst_messages.size() == 2 && burst_messages[0] == large_message &&
        burst_messages[1] == following_message);

  for (std::size_t split = 0; split <= wire.size(); ++split) {
    headunit::AirplayIapStream fragmented(secret, 19);
    auto received = fragmented.feed(std::span(wire).first(split));
    const auto rest = fragmented.feed(std::span(wire).subspan(split));
    received.insert(received.end(), rest.begin(), rest.end());
    check(received == std::vector<std::vector<std::uint8_t>>({first_message, second_message}));
  }

  // The 64-KiB feed budget does not include a preceding partial record.
  headunit::AirplayIapStream wire_boundary(secret, 19);
  const std::vector<std::uint8_t> record_message(16384 - 32, 0x7c);
  const auto record_package = package(record_message);
  std::vector<std::uint8_t> records;
  for (unsigned index = 0; index < 4; ++index) {
    const auto next = frame(key, index, record_package);
    records.insert(records.end(), next.begin(), next.end());
  }
  check(records.size() == 65536 + 72);
  check(wire_boundary.feed(std::span(records).first(72)).empty());
  const auto boundary_messages = wire_boundary.feed(std::span(records).subspan(72));
  check(boundary_messages == std::vector<std::vector<std::uint8_t>>(4, record_message));

  auto tampered = wire;
  tampered[first_frame.size() - 1] ^= 1;
  headunit::AirplayIapStream invalid(secret, 19);
  bool rejected = false;
  try {
    (void)invalid.feed(tampered);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
  check(invalid.failed());
  rejected = false;
  try {
    (void)invalid.feed(wire);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected && invalid.messages() == 0);

  for (const auto length : {31u, 4u * 1024u * 1024u + 1u}) {
    auto malformed = first_package;
    for (unsigned byte = 0; byte < 4; ++byte) {
      malformed[byte] = static_cast<std::uint8_t>(length >> ((3 - byte) * 8));
    }
    headunit::AirplayIapStream bad_package(secret, 19);
    rejected = false;
    try {
      (void)bad_package.feed(frame(key, 0, malformed));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected && bad_package.failed());
  }

  headunit::AirplayIapStream budget(secret, 19);
  rejected = false;
  try {
    (void)budget.feed(std::vector<std::uint8_t>(65537));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected && budget.failed());

  // A later record fault must not expose a successfully decoded earlier
  // package from the same call, or permit nonce reuse on a retry.
  headunit::AirplayIapStream partial_success(secret, 19);
  auto progress = frame(key, 0, first_package);
  auto broken = frame(key, 1, second_package);
  broken.back() ^= 1;
  progress.insert(progress.end(), broken.begin(), broken.end());
  rejected = false;
  try {
    (void)partial_success.feed(progress);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected && partial_success.failed());
  check(invalid.retired());
  for (const auto& retry : std::vector<std::vector<std::uint8_t>>{{}, wire}) {
    rejected = false;
    try {
      (void)invalid.feed(retry);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected && invalid.retired());
  }

  return 0;
}
