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

enum class HapFrameChannel { control, events };

// Receiver-side AirPlay control framing after pair-verify or transient setup.
// `receive` accepts arbitrary TCP fragments and returns authenticated bytes.
class HapControlFrame {
 public:
  explicit HapControlFrame(std::span<const std::uint8_t> shared_secret,
                           HapFrameChannel channel = HapFrameChannel::control);
  ~HapControlFrame();
  HapControlFrame(const HapControlFrame&) = delete;
  HapControlFrame& operator=(const HapControlFrame&) = delete;

  std::vector<std::uint8_t> encode(std::span<const std::uint8_t> plaintext);
  std::vector<std::uint8_t> receive(std::span<const std::uint8_t> encrypted);
  static std::size_t encoded_size(std::size_t plaintext_bytes);
  bool failed() const { return failed_; }

 private:
  void fail() noexcept;
  HapKey32 read_key_{};
  HapKey32 write_key_{};
  std::vector<std::uint8_t> buffered_;
  std::uint64_t read_count_ = 0;
  std::uint64_t write_count_ = 0;
  bool failed_ = false;
};

}  // namespace headunit
