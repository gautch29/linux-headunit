// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/hap_crypto.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

// Receives the phone-to-accessory iAP DataStream. Each TCP record is
// authenticated before any APTransport package is exposed to the caller.
class AirplayIapStream {
 public:
  AirplayIapStream(std::span<const std::uint8_t> pair_secret, std::uint64_t seed);
  ~AirplayIapStream();
  AirplayIapStream(const AirplayIapStream&) = delete;
  AirplayIapStream& operator=(const AirplayIapStream&) = delete;

  std::vector<std::vector<std::uint8_t>> feed(std::span<const std::uint8_t> bytes);
  // Retirement is terminal and erases keys/partial buffers; counters survive.
  void retire() noexcept;
  bool retired() const { return failed_; }
  std::size_t messages() const { return messages_; }
  std::size_t message_bytes() const { return message_bytes_; }
  bool failed() const { return failed_; }

 private:
  void fail() noexcept;
  void packages(std::vector<std::vector<std::uint8_t>>& messages);
  HapKey32 key_{};
  std::uint64_t nonce_ = 0;
  std::vector<std::uint8_t> encrypted_;
  std::vector<std::uint8_t> plain_;
  std::size_t messages_ = 0;
  std::size_t message_bytes_ = 0;
  bool failed_ = false;
};

}  // namespace headunit
