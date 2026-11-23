// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

// Certificate and digest signing belong to one receiver-owned provider.
// The protocol callers retain challenge derivation and session retirement.
class MfiAuthenticator {
 public:
  virtual ~MfiAuthenticator() = default;
  virtual std::uint8_t protocol_major() = 0;
  virtual std::vector<std::uint8_t> read_certificate() = 0;
  virtual std::array<std::uint8_t, 64> sign_digest(
      std::span<const std::uint8_t, 32> digest,
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) = 0;
};

}  // namespace headunit
