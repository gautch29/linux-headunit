// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_reliable_link.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() try {
  using namespace std::chrono_literals;
  using headunit::Iap2ReliableLink;
  using headunit::LinkFrame;
  const auto now = Iap2ReliableLink::Clock::now();

  // The recorded connection started with local SYN=99 and remote SYNACK=135.
  Iap2ReliableLink link(99, 135, 2);
  const LinkFrame request{0x40, 136, 99, 10, {0x40, 0x40, 0, 6, 0x1d, 0}};
  auto received = link.receive(request);
  check(received.delivered == std::vector<LinkFrame>{request},
        "first phone control message was lost");
  check(received.outgoing == std::vector<LinkFrame>({{0x40, 99, 136, 0, {}}}),
        "initial acknowledgement differed from recorded sequence numbers");

  const std::vector<std::uint8_t> reply{0x40, 0x40, 0, 6, 0x1d, 1};
  const auto sent = link.send(10, reply, now);
  check(sent && sent->sequence == 100 && sent->acknowledgement == 136,
        "first accessory control sequence differed from capture");
  check(link.unacknowledged() == 1, "sent frame was not tracked");
  check(link.tick(now + 499ms).empty(), "frame retransmitted early");
  const auto retry = link.tick(now + 500ms);
  check(retry.size() == 1 && retry[0].sequence == 100, "unacknowledged frame was not retried");

  received = link.receive({0x40, 137, 100, 10, {0x40, 0x40, 0, 6, 0x1d, 2}});
  check(received.delivered.size() == 1 && link.unacknowledged() == 0,
        "phone acknowledgement did not release pending frame");
  check(link.last_received_sequence() == 137, "phone sequence did not advance");

  // A gap asks the phone to retry 138; a duplicate is never delivered twice.
  received = link.receive({0x40, 139, 100, 10, {1}});
  check(received.delivered.empty() && received.outgoing[0].acknowledgement == 137,
        "out-of-order frame was accepted");
  received = link.receive({0x40, 138, 100, 10, {2}});
  check(received.delivered.size() == 1 && received.outgoing[0].acknowledgement == 138,
        "missing frame did not restore contiguous sequence");
  received = link.receive({0x40, 138, 100, 10, {2}});
  check(received.delivered.empty() && received.outgoing[0].acknowledgement == 138,
        "duplicate frame was delivered again");
  received = link.receive({0x40, 138, 100, 0, {}});
  check(received.delivered.empty() && received.outgoing.empty(),
        "acknowledgement-only frame started an acknowledgement loop");

  // Window capacity and cumulative acknowledgement.
  check(link.send(10, reply, now + 1s)->sequence == 101, "next sequence was wrong");
  check(link.send(10, reply, now + 1s)->sequence == 102, "next sequence was wrong");
  check(!link.send(10, reply, now + 1s), "send window was exceeded");
  link.receive({0x40, 138, 102, 0, {}});
  check(link.unacknowledged() == 0, "cumulative ACK did not clear the window");

  // Rejected data frames must not acknowledge pending transmissions. Their
  // checksum-valid headers do not make an invalid session/payload pair usable.
  Iap2ReliableLink malformed(1, 2);
  check(malformed.send(10, reply, now).has_value(), "malformed-frame setup failed");
  for (const auto& frame : std::vector<LinkFrame>{{0x40, 3, 2, 0, {1}}, {0x40, 3, 2, 10, {}}}) {
    received = malformed.receive(frame);
    check(received.delivered.empty() && received.outgoing.empty() &&
              malformed.unacknowledged() == 1 && malformed.last_received_sequence() == 2,
          "rejected data frame acknowledged a pending transmission");
  }
  check(malformed.tick(now + 500ms).size() == 1, "rejected data frame suppressed retransmission");
  malformed.receive({0x40, 2, 2, 0, {}});
  check(malformed.unacknowledged() == 0, "valid pure ACK was rejected after malformed data");

  Iap2ReliableLink wrapping(255, 254);
  check(wrapping.send(10, reply, now)->sequence == 0, "local sequence failed to wrap");
  received = wrapping.receive({0x40, 255, 0, 10, {3}});
  check(received.delivered.size() == 1 && received.outgoing[0].acknowledgement == 255,
        "remote sequence failed to wrap");

  Iap2ReliableLink expiry(1, 2);
  check(expiry.send(10, reply, now).has_value(), "expiry setup failed");
  for (int attempt = 1; attempt < 5; ++attempt) {
    check(expiry.tick(now + attempt * 500ms).size() == 1, "bounded retry did not send");
  }
  check(expiry.tick(now + 2500ms).empty() && expiry.timed_out(),
        "unacknowledged link did not time out");
  const auto pending_at_timeout = expiry.unacknowledged();
  received = expiry.receive({0x40, 3, 2, 10, {1}});
  check(received.delivered.empty() && received.outgoing.empty() &&
            expiry.unacknowledged() == pending_at_timeout && expiry.last_received_sequence() == 2,
        "timed-out link accepted data or an acknowledgement");
  check(!expiry.send(10, reply, now + 3s) && expiry.tick(now + 3s).empty(),
        "timed-out link resumed transmission");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
