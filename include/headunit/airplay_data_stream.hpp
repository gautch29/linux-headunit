// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/hap_crypto.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

enum class DataMessageKind : std::uint32_t {
  asynchronous = 0x6173796e,
  synchronous = 0x73796e63,
  reply = 0x72706c79
};
struct DataMessage {
  DataMessageKind kind = DataMessageKind::asynchronous;
  std::uint64_t group = 0;
  std::uint32_t type = 0x636d6e64;  // cmnd; payload semantics belong to the channel.
  std::uint64_t token = 0;
  std::int32_t error = 0;
  std::vector<std::uint8_t> payload;
  bool operator==(const DataMessage&) const = default;
};

// Receiver-side authenticated DataStream/APTransport, independent of the
// service UUID. A seed is single-use for a paired session; reconnects must
// negotiate a fresh seed, never reset an instance's nonce counters.
class AirplayDataStream {
 public:
  static constexpr std::size_t max_package = 1024 * 1024;
  static constexpr std::size_t max_feed = 65536;
  AirplayDataStream(std::span<const std::uint8_t> paired_secret, std::uint64_t seed);
  ~AirplayDataStream();
  AirplayDataStream(const AirplayDataStream&) = delete;
  AirplayDataStream& operator=(const AirplayDataStream&) = delete;
  std::vector<DataMessage> feed(std::span<const std::uint8_t> bytes);
  std::vector<std::uint8_t> encode(const DataMessage& message);
  bool failed() const { return failed_; }

 private:
  void fail();
  void packages(std::vector<DataMessage>& output);
  HapKey32 read_key_{}, write_key_{};
  std::uint64_t read_nonce_ = 0, write_nonce_ = 0;
  bool failed_ = false;
  std::vector<std::uint8_t> encrypted_, plain_;
};

}  // namespace headunit
