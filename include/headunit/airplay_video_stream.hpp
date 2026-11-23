// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/hap_crypto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace headunit {

struct AirplayVideoPacket {
  enum class Kind { config, frame, heartbeat, auxiliary } kind;
  std::uint64_t timestamp = 0;
  std::vector<std::uint8_t> data;
  std::uint16_t flags = 0;
  // Source time exists only when enabled by the latest screen configuration.
  // The raw timestamp field above is retained for existing diagnostic captures.
  std::optional<std::uint64_t> sender_ntp = {};
};

// Incremental framing and authenticated decryption for one screen connection.
// Decoder/presentation policy belongs to the consumer, not this parser.
// Each feed is limited to 8 MiB + 128 bytes; at most one incomplete packet
// is retained. Any feed fault clears retained input and permanently closes
// the parser, so a new connection must construct a new instance.
class AirplayVideoStream {
 public:
  explicit AirplayVideoStream(HapKey32 key);
  ~AirplayVideoStream();
  AirplayVideoStream(const AirplayVideoStream&) = delete;
  AirplayVideoStream& operator=(const AirplayVideoStream&) = delete;

  std::vector<AirplayVideoPacket> feed(std::span<const std::uint8_t> bytes);
  std::size_t frames() const { return frames_; }
  std::size_t configs() const { return configs_; }
  // Retirement is terminal and erases keys/partial buffers; counters survive.
  void retire() noexcept;
  bool retired() const { return failed_; }
  bool partial() const { return !pending_.empty(); }
  bool failed() const { return failed_; }

 private:
  AirplayVideoPacket packet(std::span<const std::uint8_t> bytes);
  void fail() noexcept;
  HapKey32 key_;
  std::vector<std::uint8_t> pending_;
  std::uint64_t nonce_ = 0;
  std::size_t frames_ = 0;
  std::size_t configs_ = 0;
  bool sender_timestamps_ = false;
  bool failed_ = false;
};

HapKey32 derive_airplay_video_key(std::span<const std::uint8_t> pair_secret,
                                  std::uint64_t stream_connection_id);

}  // namespace headunit
