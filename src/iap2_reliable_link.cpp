// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_reliable_link.hpp"

#include <algorithm>
#include <stdexcept>

namespace headunit {
namespace {
constexpr auto retry_interval = std::chrono::milliseconds(500);
constexpr unsigned maximum_attempts = 5;
}  // namespace

Iap2ReliableLink::Iap2ReliableLink(std::uint8_t local_syn_sequence,
                                   std::uint8_t remote_syn_ack_sequence, std::size_t send_window)
    : next_local_(static_cast<std::uint8_t>(local_syn_sequence + 1)),
      last_local_(local_syn_sequence),
      last_received_(remote_syn_ack_sequence),
      expected_remote_(static_cast<std::uint8_t>(remote_syn_ack_sequence + 1)),
      send_window_(send_window) {
  if (send_window == 0 || send_window > 64) {
    throw std::invalid_argument("iAP2 send window must be 1..64");
  }
}

LinkFrame Iap2ReliableLink::acknowledgement() const {
  return {0x40, last_local_, last_received_, 0, {}};
}

std::optional<LinkFrame> Iap2ReliableLink::send(std::uint8_t session,
                                                std::span<const std::uint8_t> payload,
                                                Clock::time_point now) {
  if (session == 0 || payload.empty()) {
    throw std::invalid_argument("data frame needs a session and payload");
  }
  if (payload.size() > 65525) {
    throw std::invalid_argument("iAP2 data payload exceeds frame limit");
  }
  if (timed_out_ || pending_.size() >= send_window_) return std::nullopt;
  LinkFrame frame{0x40, next_local_, last_received_, session, {payload.begin(), payload.end()}};
  pending_.push_back({frame, now, 1});
  last_local_ = next_local_;
  next_local_ = static_cast<std::uint8_t>(next_local_ + 1);
  return frame;
}

void Iap2ReliableLink::apply_acknowledgement(std::uint8_t acknowledgement) {
  const auto found =
      std::find_if(pending_.begin(), pending_.end(), [acknowledgement](const Pending& pending) {
        return pending.frame.sequence == acknowledgement;
      });
  if (found != pending_.end()) pending_.erase(pending_.begin(), found + 1);
}

Iap2ReliableLink::ReceiveResult Iap2ReliableLink::receive(const LinkFrame& frame) {
  ReceiveResult result;
  if (timed_out_ || frame.control != 0x40) return result;
  const bool acknowledgement_only = frame.session == 0 && frame.payload.empty();
  if (!acknowledgement_only && (frame.session == 0 || frame.payload.empty())) return result;
  // A rejected data frame cannot confirm delivery of our pending transmissions.
  apply_acknowledgement(frame.acknowledgement);
  if (acknowledgement_only) return result;

  if (frame.sequence == expected_remote_) {
    last_received_ = frame.sequence;
    expected_remote_ = static_cast<std::uint8_t>(expected_remote_ + 1);
    result.delivered.push_back(frame);
  }
  // Duplicates and gaps are acknowledged at the last contiguous sequence.
  // Pure acknowledgements are not themselves acknowledged, avoiding a loop.
  result.outgoing.push_back(acknowledgement());
  return result;
}

std::vector<LinkFrame> Iap2ReliableLink::tick(Clock::time_point now) {
  std::vector<LinkFrame> retransmissions;
  if (timed_out_) return retransmissions;
  for (auto& pending : pending_) {
    if (now < pending.sent_at || now - pending.sent_at < retry_interval) continue;
    if (pending.attempts >= maximum_attempts) {
      timed_out_ = true;
      return {};
    }
    ++pending.attempts;
    pending.sent_at = now;
    pending.frame.acknowledgement = last_received_;
    retransmissions.push_back(pending.frame);
  }
  return retransmissions;
}

}  // namespace headunit
