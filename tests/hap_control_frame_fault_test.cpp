// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_control_frame.hpp"
#include "headunit/hap_crypto.hpp"
#include "headunit/airplay_event_channel.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

// Implemented in a separate translation unit so optimized GCC does not inline
// replacement delete/free into standard new calls and report a false mismatch.
void fail_allocation_after(int count);

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void check_closed(headunit::HapControlFrame& frame) {
  check(frame.failed(), "allocation failure did not retire HomeKit framing");
  bool rejected = false;
  try {
    (void)frame.encode({});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "retired HomeKit encoder resumed");
  rejected = false;
  try {
    (void)frame.receive({});
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "retired HomeKit decoder resumed");
}
}  // namespace

int main() try {
  headunit::HapKey32 secret{};
  const std::vector<std::uint8_t> plain(1300, 42);
  unsigned encoder_faults = 0;
  for (int fail_at = 0; fail_at < 16; ++fail_at) {
    headunit::HapControlFrame frame(secret);
    fail_allocation_after(fail_at);
    bool injected = false;
    try {
      (void)frame.encode(plain);
    } catch (const std::bad_alloc&) {
      injected = true;
    }
    fail_allocation_after(-1);
    if (!injected) continue;
    ++encoder_faults;
    check_closed(frame);
  }
  check(encoder_faults >= 3, "did not inject early and later encoding failures");

  const auto phone_key =
      headunit::hap_hkdf_sha512(secret, "Control-Salt", "Control-Write-Encryption-Key");
  std::vector<std::uint8_t> wire;
  for (std::size_t offset = 0, counter = 0; offset < plain.size(); ++counter) {
    const auto length = std::min<std::size_t>(1024, plain.size() - offset);
    const std::array<std::uint8_t, 2> header{static_cast<std::uint8_t>(length),
                                             static_cast<std::uint8_t>(length >> 8)};
    std::array<char, 8> nonce{};
    nonce[0] = static_cast<char>(counter);
    const auto sealed = headunit::hap_encrypt(phone_key, {nonce.data(), nonce.size()},
                                              std::span(plain).subspan(offset, length), header);
    wire.insert(wire.end(), header.begin(), header.end());
    wire.insert(wire.end(), sealed.begin(), sealed.end());
    offset += length;
  }
  unsigned decoder_faults = 0;
  for (int fail_at = 0; fail_at < 16; ++fail_at) {
    headunit::HapControlFrame frame(secret);
    fail_allocation_after(fail_at);
    bool injected = false;
    try {
      (void)frame.receive(wire);
    } catch (const std::bad_alloc&) {
      injected = true;
    }
    fail_allocation_after(-1);
    if (!injected) continue;
    ++decoder_faults;
    check_closed(frame);
  }
  check(decoder_faults >= 5, "did not inject receive faults after partial progress");
  headunit::AirplayEventChannel reference(secret);
  const auto expected = reference.send_iap_message(plain);
  unsigned event_faults = 0;
  for (int fail_at = 0; fail_at < 32; ++fail_at) {
    headunit::AirplayEventChannel channel(secret);
    fail_allocation_after(fail_at);
    bool injected = false;
    try {
      (void)channel.send_iap_message(plain);
    } catch (const std::bad_alloc&) {
      injected = true;
    }
    fail_allocation_after(-1);
    if (!injected) continue;
    ++event_faults;
    try {
      // Before framing starts, failed packaging is retryable without skipping
      // CSeq/nonces. Once framing has started, an error retires that channel.
      check(channel.send_iap_message(plain) == expected,
            "event allocation failure skipped sequence or nonce");
    } catch (const std::invalid_argument&) {
      // Terminal encryption failure correctly prevents retry.
    }
  }
  check(event_faults >= 3, "did not inject packaging and encryption failures");
  std::cout << "HomeKit allocation failure retirement passed: encode=" << encoder_faults
            << " receive=" << decoder_faults << " event=" << event_faults << '\n';
} catch (const std::exception& error) {
  fail_allocation_after(-1);
  std::cerr << error.what() << '\n';
  return 1;
}
