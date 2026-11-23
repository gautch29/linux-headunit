// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_video_stream.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition) {
  if (!condition) throw std::runtime_error("AirPlay video stream test failed");
}

void length(std::array<std::uint8_t, 128>& header, std::size_t size) {
  for (unsigned index = 0; index < 4; ++index) {
    header[index] = static_cast<std::uint8_t>(size >> (index * 8));
  }
}

void append(std::vector<std::uint8_t>& output, const std::array<std::uint8_t, 128>& header,
            const std::vector<std::uint8_t>& body) {
  output.insert(output.end(), header.begin(), header.end());
  output.insert(output.end(), body.begin(), body.end());
}
}  // namespace

int main() {
  std::array<std::uint8_t, 32> shared{};
  for (unsigned index = 0; index < shared.size(); ++index)
    shared[index] = index;
  const auto key = headunit::derive_airplay_video_key(shared, 42);
  headunit::AirplayVideoStream video(key);
  std::array<std::uint8_t, 128> config_header{};
  config_header[4] = 1;
  const std::vector<std::uint8_t> config{1, 2, 3, 4};
  length(config_header, config.size());
  std::array<std::uint8_t, 128> frame_header{};
  const std::vector<std::uint8_t> frame{0, 0, 0, 2, 0x26, 0x01};
  const std::array<char, 8> nonce{};
  length(frame_header, frame.size() + 16);
  const auto sealed = headunit::hap_encrypt(key, {nonce.data(), nonce.size()}, frame, frame_header);
  std::vector<std::uint8_t> wire;
  append(wire, config_header, config);
  append(wire, frame_header, sealed);
  auto first = video.feed({wire.data(), 37});
  check(first.empty() && video.partial());
  auto packets = video.feed({wire.data() + 37, wire.size() - 37});
  check(packets.size() == 2 && packets[0].kind == headunit::AirplayVideoPacket::Kind::config &&
        packets[0].data == config && packets[1].kind == headunit::AirplayVideoPacket::Kind::frame &&
        packets[1].data == frame && video.frames() == 1 && video.configs() == 1);
  check(!video.partial() && !packets[1].sender_ntp);

  // TCP may split either header, ciphertext or tag at any byte. Both the
  // direct and retained-packet paths must produce identical owning packets.
  for (std::size_t split = 0; split <= wire.size(); ++split) {
    headunit::AirplayVideoStream fragmented(key);
    auto received = fragmented.feed(std::span(wire).first(split));
    const auto rest = fragmented.feed(std::span(wire).subspan(split));
    received.insert(received.end(), rest.begin(), rest.end());
    check(received.size() == 2 && received[0].data == config && received[1].data == frame);
    check(fragmented.configs() == 1 && fragmented.frames() == 1 && !fragmented.partial());
  }
  headunit::AirplayVideoStream bytewise(key);
  std::vector<headunit::AirplayVideoPacket> bytewise_packets;
  for (const auto& byte : wire) {
    auto received = bytewise.feed({&byte, 1});
    bytewise_packets.insert(bytewise_packets.end(), received.begin(), received.end());
  }
  check(bytewise_packets.size() == 2 && bytewise_packets[1].data == frame && !bytewise.partial());

  // A legal maximum-size packet must not fail just because the read completing
  // it also contains the next packet. Only the unfinished packet is retained.
  headunit::AirplayVideoStream boundary(key);
  auto large_header = config_header;
  const std::vector<std::uint8_t> large_body(8 * 1024 * 1024, 0x5a);
  length(large_header, large_body.size());
  check(boundary.feed(large_header).empty() && boundary.partial());
  check(boundary.feed(std::span(large_body).first(large_body.size() - 1)).empty());
  std::vector<std::uint8_t> completion{large_body.back()};
  append(completion, frame_header, sealed);
  const auto completed = boundary.feed(completion);
  check(completed.size() == 2 && completed[0].data == large_body && completed[1].data == frame);
  check(!boundary.partial() && boundary.frames() == 1 && boundary.configs() == 1);

  // A zero-body packet completes when the final header byte arrives.
  headunit::AirplayVideoStream heartbeat(key);
  std::array<std::uint8_t, 128> heartbeat_header{};
  heartbeat_header[4] = 2;
  check(heartbeat.feed(std::span(heartbeat_header).first(127)).empty());
  const auto beat = heartbeat.feed(std::span(heartbeat_header).last(1));
  check(beat.size() == 1 && beat[0].kind == headunit::AirplayVideoPacket::Kind::heartbeat);
  check(!heartbeat.partial() && heartbeat.frames() == 0);

  std::array<std::uint8_t, 128> auxiliary_header{};
  auxiliary_header[4] = 5;
  length(auxiliary_header, 3);
  std::vector<std::uint8_t> later;
  append(later, auxiliary_header, {7, 8, 9});
  std::array<std::uint8_t, 128> second_frame_header{};
  length(second_frame_header, frame.size() + 16);
  const std::array<char, 8> second_nonce{1};
  const auto second_sealed = headunit::hap_encrypt(key, {second_nonce.data(), second_nonce.size()},
                                                   frame, second_frame_header);
  append(later, second_frame_header, second_sealed);
  const auto later_packets = video.feed(later);
  check(later_packets.size() == 2 &&
        later_packets[0].kind == headunit::AirplayVideoPacket::Kind::auxiliary &&
        later_packets[0].data.empty() &&
        later_packets[1].kind == headunit::AirplayVideoPacket::Kind::frame &&
        later_packets[1].data == frame && video.frames() == 2);

  // Only the most recent configuration enables source timestamps. Auxiliary
  // records and frame flags cannot enable them, or advance the cipher nonce.
  for (unsigned index = 0; index < 3; ++index) {
    config_header[6] = index == 1 ? 0 : 2;
    config_header[7] = 1;
    frame_header[6] = index == 1 ? 2 : 0;
    const std::uint64_t stamp = index == 2 ? 0 : 0x123456789abcdef0ULL;
    for (unsigned byte = 0; byte < 8; ++byte)
      frame_header[8 + byte] = static_cast<std::uint8_t>(stamp >> (byte * 8));
    std::array<char, 8> current_nonce{static_cast<char>(index + 2)};
    const auto encrypted = headunit::hap_encrypt(key, {current_nonce.data(), current_nonce.size()},
                                                 frame, frame_header);
    std::vector<std::uint8_t> update;
    append(update, config_header, config);
    append(update, auxiliary_header, {7, 8, 9});
    append(update, frame_header, encrypted);
    const auto received = video.feed(update);
    check(received.size() == 3 && received[0].flags == (index == 1 ? 256 : 258));
    check(!received[0].sender_ntp && !received[1].sender_ntp);
    check(received[2].timestamp == stamp && received[2].data == frame);
    check(received[2].sender_ntp.has_value() == (index != 1));
    if (index != 1) check(*received[2].sender_ntp == stamp);
    check(!video.partial());
  }

  const auto frames = video.frames();
  check(video.feed({wire.data(), 3}).empty() && video.partial());
  video.retire();
  video.retire();
  check(video.retired() && !video.partial() && video.frames() == frames);
  bool closed_rejected = false;
  try {
    (void)video.feed({});
  } catch (const std::invalid_argument&) {
    closed_rejected = true;
  }
  check(closed_rejected);

  auto corrupt = wire;
  corrupt.back() ^= 1;
  headunit::AirplayVideoStream invalid(key);
  bool rejected = false;
  try {
    (void)invalid.feed(corrupt);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
  check(invalid.failed() && !invalid.partial());
  rejected = false;
  try {
    (void)invalid.feed(wire);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
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
  check(!invalid.partial());

  std::array<std::uint8_t, 128> oversized{};
  length(oversized, 8 * 1024 * 1024 + 1);
  headunit::AirplayVideoStream bounded(key);
  rejected = false;
  try {
    (void)bounded.feed(oversized);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
  check(bounded.failed() && !bounded.partial());

  // Budget rejection is terminal even when it happened before buffering.
  headunit::AirplayVideoStream budget(key);
  const std::vector<std::uint8_t> too_large(8 * 1024 * 1024 + 129);
  rejected = false;
  try {
    (void)budget.feed(too_large);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected && budget.failed() && !budget.partial());
  rejected = false;
  try {
    (void)budget.feed(wire);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected && budget.frames() == 0 && budget.configs() == 0);

  for (const bool buffered : {false, true}) {
    headunit::AirplayVideoStream bad_kind(key);
    auto unknown = heartbeat_header;
    unknown[4] = 255;
    if (buffered) check(bad_kind.feed(std::span(unknown).first(127)).empty());
    rejected = false;
    try {
      (void)bad_kind.feed(buffered ? std::span(unknown).last(1) : std::span(unknown));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected && bad_kind.failed() && !bad_kind.partial());
  }
  return 0;
}
