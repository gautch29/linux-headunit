// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/iap2_handshake.hpp>

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
  using headunit::encode_link_frame;
  using headunit::LinkFrame;
  using headunit::LinkHandshake;
  using headunit::LinkHandshakeState;

  const auto start = LinkHandshake::Clock::now();
  LinkHandshake handshake(0x63);
  constexpr std::uint8_t sync[]{0xff, 0x55, 0x02, 0x00, 0xee, 0x10};
  check(LinkHandshake::sync_preamble() ==
            std::array<std::uint8_t, 6>{0xff, 0x55, 0x02, 0x00, 0xee, 0x10},
        "accessory synchronization marker differs from capture");
  check(handshake.feed(std::span(sync).first(2), start).empty(),
        "partial synchronization started a handshake");
  const auto offer = handshake.feed(std::span(sync).subspan(2), start);
  check(offer.size() == 1 && offer[0].control == 0x80 && offer[0].sequence == 0x63 &&
            offer[0].payload.size() == 19,
        "synchronization did not produce a link offer");
  const auto offered_bytes = encode_link_frame(offer[0]);
  check(offered_bytes.size() == 29 && offered_bytes[8] == 0xa7 && offered_bytes.back() == 0x29,
        "link offer did not match observed wire checksums");
  check(handshake.state() == LinkHandshakeState::waiting_for_syn_ack,
        "link state did not advance after synchronization");
  check(handshake.tick(start + 499ms).empty(), "offer retried too early");
  check(handshake.tick(start + 500ms) == offer, "offer was not retried");

  const LinkFrame wrong_reply{0xc0, 0x87, 0x62, 0, offer[0].payload};
  check(handshake.feed(encode_link_frame(wrong_reply), start + 501ms).empty(),
        "reply with wrong acknowledgement was accepted");
  auto remote_parameters = offer[0].payload;
  remote_parameters[1] = 0x7f;  // The phone's observed offer differs here.
  const LinkFrame reply{0xc0, 0x87, 0x63, 0, remote_parameters};
  const auto answer = handshake.feed(encode_link_frame(reply), start + 502ms);
  check(answer == std::vector<LinkFrame>({LinkFrame{0x40, 0x63, 0x87, 0, {}}}),
        "SYN acknowledgement did not produce the final ACK");
  check(handshake.state() == LinkHandshakeState::established,
        "valid reply did not establish the link");
  const auto answer_bytes = encode_link_frame(answer[0]);
  check(answer_bytes.size() == 9 && answer_bytes.back() == 0x74,
        "final acknowledgement did not match the observed wire header");
  check(handshake.feed(encode_link_frame(reply), start + 503ms) == answer,
        "duplicate SYN acknowledgement was not acknowledged");
  check(handshake.tick(start + 10s).empty(), "established link retried its offer");

  LinkHandshake never_replied(5);
  check(never_replied.feed(sync, start).size() == 1, "second handshake did not start");
  for (int attempt = 1; attempt < 5; ++attempt) {
    const auto packets = never_replied.tick(start + attempt * 500ms);
    check(packets.size() == (attempt < 5 ? 1u : 0u), "retry count was wrong");
  }
  check(never_replied.tick(start + 2500ms).empty() &&
            never_replied.state() == LinkHandshakeState::timed_out,
        "unanswered link did not time out");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
