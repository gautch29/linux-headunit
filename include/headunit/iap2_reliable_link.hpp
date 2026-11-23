// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/iap2_link.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace headunit {

// Sequencing for an established iAP2 link. RFCOMM frame decoding and the SYN
// exchange happen before this object is created.
class Iap2ReliableLink {
 public:
  using Clock = std::chrono::steady_clock;
  struct ReceiveResult {
    std::vector<LinkFrame> delivered;
    std::vector<LinkFrame> outgoing;
  };

  Iap2ReliableLink(std::uint8_t local_syn_sequence, std::uint8_t remote_syn_ack_sequence,
                   std::size_t send_window = 8);

  std::optional<LinkFrame> send(std::uint8_t session, std::span<const std::uint8_t> payload,
                                Clock::time_point now);
  ReceiveResult receive(const LinkFrame& frame);
  std::vector<LinkFrame> tick(Clock::time_point now);

  std::size_t unacknowledged() const { return pending_.size(); }
  bool timed_out() const { return timed_out_; }
  std::uint8_t last_received_sequence() const { return last_received_; }

 private:
  struct Pending {
    LinkFrame frame;
    Clock::time_point sent_at;
    unsigned attempts = 1;
  };
  LinkFrame acknowledgement() const;
  void apply_acknowledgement(std::uint8_t acknowledgement);

  std::uint8_t next_local_;
  std::uint8_t last_local_;
  std::uint8_t last_received_;
  std::uint8_t expected_remote_;
  std::size_t send_window_;
  std::vector<Pending> pending_;
  bool timed_out_ = false;
};

}  // namespace headunit
