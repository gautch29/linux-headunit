// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/iap2_link.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

enum class LinkHandshakeState { waiting_for_sync, waiting_for_syn_ack, established, timed_out };

// Only the initial RFCOMM link handshake. It does not provide iAP2 sessions,
// message authentication, retransmission of data frames, or CarPlay services.
class LinkHandshake {
 public:
  using Clock = std::chrono::steady_clock;

  explicit LinkHandshake(std::uint8_t initial_sequence);
  static std::array<std::uint8_t, 6> sync_preamble();
  std::vector<LinkFrame> feed(std::span<const std::uint8_t> bytes, Clock::time_point now);
  std::vector<LinkFrame> tick(Clock::time_point now);
  LinkHandshakeState state() const { return state_; }

 private:
  LinkFrame make_syn() const;
  LinkFrame make_ack(std::uint8_t remote_sequence) const;
  LinkHandshakeState state_ = LinkHandshakeState::waiting_for_sync;
  std::uint8_t initial_sequence_;
  std::array<std::uint8_t, 6> sync_window_{};
  std::size_t sync_count_ = 0;
  unsigned attempts_ = 0;
  Clock::time_point last_sent_{};
  LinkFrameReader reader_;
};

}  // namespace headunit
