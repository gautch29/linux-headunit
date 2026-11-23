// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/protocol.hpp>
#include <plist/plist.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace headunit::theme;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)

Bytes command(std::uint64_t type = 0x11, double pts = 0, bool moving = false) {
  auto root = plist_new_dict();
  auto data = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_uint(type));
  if (type == 2) plist_dict_set_item(data, "pts", plist_new_real(pts));
  if (type == 9) plist_dict_set_item(data, "isTransitioning", plist_new_bool(moving));
  plist_dict_set_item(root, "data", data);
  char* raw = nullptr;
  std::uint32_t size = 0;
  plist_to_bin(root, &raw, &size);
  Bytes bytes(raw, raw + size);
  plist_mem_free(raw);
  plist_free(root);
  return bytes;
}
Packet peer(MessageType type, std::uint64_t seq, std::uint64_t ack = 2) {
  Packet p;
  p.type = type;
  p.session = 1;
  p.sequence = seq;
  p.acknowledgement = ack;
  p.version = 2;
  p.peer_id = "bench-peer";
  return p;
}
void ready(Session& session, std::uint64_t version = 2) {
  session.start();
  CHECK(session.pending());
  session.sent(0);
  auto p = peer(MessageType::reset_complete, 1, 1);
  p.version = version;
  if (version == 1) p.peer_id.reset();
  CHECK(session.receive(encode(p)).result == Result::ready);
  auto reply = decode(*session.pending());
  CHECK(reply.type == MessageType::reset_complete && reply.sequence == 2 &&
        reply.acknowledgement == 1);
  CHECK(reply.version == version);
  CHECK(version != 1 || !reply.peer_id);
  session.sent(1);
}

void handshake_and_delivery() {
  Session session("bench-peer");
  CHECK(!session.pending());
  ready(session);
  CHECK(session.state() == State::ready);
  auto p = peer(MessageType::command, 2);
  p.data = command();
  CHECK(session.receive(encode(p)).command == p.data);
  CHECK(session.receive(encode(p)).result == Result::ignored);
  auto reply = decode(*session.pending());
  CHECK(reply.type == MessageType::ack && reply.sequence == 3 && reply.acknowledgement == 2);
  session.sent(2);
  CHECK(session.receive(encode(peer(MessageType::ack, 3, 3))).result == Result::acknowledgement);
  CHECK(!session.pending());
  Session legacy("bench-peer");
  ready(legacy, 1);
  CHECK(legacy.version() == 1);
}
void invalid_peer_and_gap() {
  Session session("bench-peer");
  session.start();
  session.pending();
  session.sent(0);
  auto p = peer(MessageType::reset_complete, 1, 1);
  p.peer_id = "different-peer";
  CHECK(session.receive(encode(p)).result == Result::ignored);
  CHECK(session.state() == State::awaiting_reset);
  p.peer_id = "bench-peer";
  p.version = 99;
  CHECK(session.receive(encode(p)).result == Result::ready);
  CHECK(session.version() == 2);
  session.pending();
  session.sent(1);
  auto c = peer(MessageType::command, 2, 88);
  c.data = command();
  CHECK(session.receive(encode(c)).result == Result::ignored);
  c.acknowledgement = 2;
  CHECK(session.receive(encode(c)).result == Result::command);
  c.sequence = 4;
  CHECK(session.receive(encode(c)).result == Result::reset_started);
  CHECK(session.session_number() == 2 && session.state() == State::awaiting_reset);
  CHECK(decode(*session.pending()).type == MessageType::reset);
}
void timeout_and_stop() {
  Session session;
  session.start();
  auto first = *session.pending();
  CHECK(*session.pending() == first);
  CHECK(!session.tick(99999));  // Timer starts on transmission, not preparation.
  session.sent(500);
  CHECK(!session.tick(1499));
  CHECK(session.tick(1500));
  CHECK(session.session_number() == 2);
  CHECK(decode(*session.pending()).sequence == 1);
  session.stop();
  CHECK(!session.pending());
  CHECK(!session.send_command(command()));
  CHECK(!session.tick(99999));
}
void bounded_queue_and_backpressure() {
  Session session("bench-peer", 2);
  ready(session);
  CHECK(session.send_command(command()));
  CHECK(session.send_command(command()));
  CHECK(!session.send_command(command()));
  CHECK(session.queued() == 2);
  auto first = *session.pending();
  CHECK(*session.pending() == first);
  session.sent(3);
  CHECK(session.queued() == 1);
  session.pending();
  session.sent(4);

  Session delayed("bench-peer");
  ready(delayed);
  auto c = peer(MessageType::command, 2);
  c.data = command();
  CHECK(delayed.receive(encode(c)).result == Result::command);
  const auto held_ack = *delayed.pending();
  c.sequence = 3;
  CHECK(delayed.receive(encode(c)).result == Result::command);
  CHECK(*delayed.pending() == held_ack);  // Transport-owned bytes stay stable.
  delayed.sent(5);
  CHECK(delayed.pending());
  CHECK(decode(*delayed.pending()).acknowledgement == 3);
  delayed.sent(6);
  CHECK(!delayed.pending());

  Session no_ack("bench-peer");
  ready(no_ack);
  c.sequence = 2;
  c.no_ack = true;
  CHECK(no_ack.receive(encode(c)).result == Result::command);
  CHECK(!no_ack.pending());
}
void malformed() {
  Session session("bench-peer");
  ready(session);
  CHECK(session.receive(Bytes{1, 2, 3}).result == Result::malformed);
  CHECK(session.receive(Bytes(max_packet_bytes + 1)).result == Result::malformed);
  auto c = peer(MessageType::command, 2);
  c.data = command();
  auto bytes = encode(c);
  bytes.resize(bytes.size() - 3);
  CHECK(session.receive(bytes).result == Result::malformed);
  CHECK(session.state() == State::ready);
  CHECK(!session.send_command(Bytes{1, 2, 3}));
  CHECK(!session.pending());
}
void presentation_windows() {
  Timeline timeline;
  CHECK(timeline.push(command(9, 0, true)));
  CHECK(timeline.push(command(2, 1005)));
  CHECK(timeline.push(command(2, 1016)));
  CHECK(timeline.push(command(9, 0, false)));
  CHECK(timeline.frame(NAN, 0.016).empty());
  CHECK(timeline.queued() == 4);
  CHECK(timeline.frame(1, 0).empty());
  auto due = timeline.frame(1, 0.016);
  CHECK(due.size() == 2 && due[1].pts_ms == 1005);
  CHECK(timeline.queued() == 2 && timeline.transitioning());
  due = timeline.frame(1.016, 0.016);
  CHECK(due.size() == 2 && due[0].pts_ms == 1016 && !timeline.transitioning());
  CHECK(timeline.push(command(2, 1030)));
  CHECK(timeline.frame(1.032, 0.016).empty());  // Outside transition markers.
  CHECK(!timeline.push(command(2, NAN)));
  CHECK(!timeline.push(command(2, -1)));
  CHECK(!timeline.push(command(0x10)));  // Palette changes are out-of-band.
  CHECK(timeline.push(command(9, 0, true)));
  timeline.reset();
  CHECK(timeline.queued() == 0 && !timeline.transitioning());
  Timeline limited(1);
  CHECK(limited.push(command(9, 0, true)));
  CHECK(!limited.push(command(9, 0, false)));
}
int main() {
  try {
    handshake_and_delivery();
    invalid_peer_and_gap();
    timeout_and_stop();
    bounded_queue_and_backpressure();
    malformed();
    presentation_windows();
    std::cout << "6 protocol/timeline test groups passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
