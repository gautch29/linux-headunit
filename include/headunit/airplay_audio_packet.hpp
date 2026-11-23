// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/hap_crypto.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace headunit {

struct AudioRtpPacket {
  std::uint16_t sequence = 0;
  std::uint32_t sample_time = 0;
  std::vector<std::uint8_t> bytes;
};

// Authenticates one phone-to-head-unit UDP audio datagram. An invalid or replayed
// packet is discarded without changing the stream's replay state.
class AirplayAudioPackets {
 public:
  AirplayAudioPackets(std::span<const std::uint8_t> shared_secret, std::uint64_t connection_id);
  ~AirplayAudioPackets();
  AirplayAudioPackets(const AirplayAudioPackets&) = delete;
  AirplayAudioPackets& operator=(const AirplayAudioPackets&) = delete;

  std::optional<AudioRtpPacket> open(std::span<const std::uint8_t> datagram);

 private:
  bool seen_nonce(std::uint64_t nonce) const;
  void record_nonce(std::uint64_t nonce);

  HapKey32 key_{};
  std::uint64_t latest_nonce_ = 0;
  std::uint64_t replay_window_ = 0;
  bool received_ = false;
};

// Independently keyed microphone uplink. One fresh connection ID owns a
// monotonic 64-bit nonce, never reset by RTP sequence/timestamp wrap.
class AirplayMicrophonePackets {
 public:
  AirplayMicrophonePackets(std::span<const std::uint8_t> secret, std::uint64_t connection_id,
                           unsigned samples_per_packet = 960);
  ~AirplayMicrophonePackets();
  AirplayMicrophonePackets(const AirplayMicrophonePackets&) = delete;
  AirplayMicrophonePackets& operator=(const AirplayMicrophonePackets&) = delete;
  std::vector<std::uint8_t> seal(std::span<const std::uint8_t> payload);
  std::vector<std::uint8_t> seal(std::span<const std::uint8_t> payload,
                                 std::uint32_t capture_sample_time);

 private:
  HapKey32 key_{};
  unsigned samples_per_packet_ = 960;
  std::uint64_t nonce_ = 0;
  std::uint16_t sequence_ = 0;
  std::uint32_t sample_time_ = 0;
};

}  // namespace headunit
