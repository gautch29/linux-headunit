// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/iap2_handshake.hpp>

#include <algorithm>
#include <stdexcept>

namespace headunit {
namespace {

constexpr std::array<std::uint8_t, 6> sync_marker{0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
constexpr auto retry_interval = std::chrono::milliseconds(500);
constexpr unsigned maximum_attempts = 5;

// Link-offer bytes observed in an accessory-to-phone setup packet. They are
// connection parameters, not a device identity or authentication material.
constexpr std::array<std::uint8_t, 19> wireless_link_offer{0x01, 0x04, 0xff, 0xff, 0x0f, 0xa0, 0x01,
                                                           0xf4, 0x04, 0x03, 0x0a, 0x00, 0x02, 0x0b,
                                                           0x02, 0x01, 0x0c, 0x01, 0x02};

}  // namespace

LinkHandshake::LinkHandshake(std::uint8_t initial_sequence) : initial_sequence_(initial_sequence) {}

std::array<std::uint8_t, 6> LinkHandshake::sync_preamble() {
  return sync_marker;
}

LinkFrame LinkHandshake::make_syn() const {
  return LinkFrame{
      0x80, initial_sequence_, 0, 0, {wireless_link_offer.begin(), wireless_link_offer.end()}};
}

LinkFrame LinkHandshake::make_ack(std::uint8_t remote_sequence) const {
  return LinkFrame{0x40, initial_sequence_, remote_sequence, 0, {}};
}

std::vector<LinkFrame> LinkHandshake::feed(std::span<const std::uint8_t> bytes,
                                           Clock::time_point now) {
  std::vector<LinkFrame> outgoing;
  if (state_ == LinkHandshakeState::waiting_for_sync) {
    for (const auto byte : bytes) {
      if (sync_count_ < sync_window_.size()) {
        sync_window_[sync_count_++] = byte;
      } else {
        std::move(sync_window_.begin() + 1, sync_window_.end(), sync_window_.begin());
        sync_window_.back() = byte;
      }
      if (sync_count_ == sync_window_.size() && sync_window_ == sync_marker) {
        outgoing.push_back(make_syn());
        state_ = LinkHandshakeState::waiting_for_syn_ack;
        last_sent_ = now;
        attempts_ = 1;
        break;
      }
    }
  }
  for (const auto& frame : reader_.feed(bytes)) {
    if (frame.session != 0 || frame.control != 0xc0 || frame.acknowledgement != initial_sequence_)
      continue;
    if (state_ == LinkHandshakeState::waiting_for_syn_ack) {
      state_ = LinkHandshakeState::established;
      outgoing.push_back(make_ack(frame.sequence));
    } else if (state_ == LinkHandshakeState::established) {
      outgoing.push_back(make_ack(frame.sequence));
    }
  }
  return outgoing;
}

std::vector<LinkFrame> LinkHandshake::tick(Clock::time_point now) {
  if (state_ != LinkHandshakeState::waiting_for_syn_ack || now < last_sent_ ||
      now - last_sent_ < retry_interval)
    return {};
  if (attempts_ >= maximum_attempts) {
    state_ = LinkHandshakeState::timed_out;
    return {};
  }
  ++attempts_;
  last_sent_ = now;
  return {make_syn()};
}

}  // namespace headunit
