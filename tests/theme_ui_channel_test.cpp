// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_channel.hpp>
#include <plist/plist.h>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace headunit::theme;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
}
Bytes command() {
  auto root = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_uint(0x11));
  plist_dict_set_item(root, "data", plist_new_dict());
  char* raw = nullptr;
  std::uint32_t size = 0;
  plist_to_bin(root, &raw, &size);
  Bytes result(raw, raw + size);
  plist_mem_free(raw);
  plist_free(root);
  return result;
}
Packet peer(MessageType type, std::uint64_t sequence = 1, std::uint64_t ack = 1) {
  Packet p;
  p.type = type;
  p.session = 1;
  p.sequence = sequence;
  p.acknowledgement = ack;
  p.version = 2;
  p.peer_id = "vehicle";
  return p;
}
ui::Routed receive(ui::Channel& channel, const std::string& display, const Packet& p) {
  return channel.receive(ui::encode({display, headunit::theme::encode(p)}));
}
void ready(ui::Channel& channel) {
  channel.start();
  for (const auto* id : {"main", "dash"}) {
    const auto message = ui::decode(*channel.pending());
    CHECK(message.display_uuid == id);
    CHECK(headunit::theme::decode(message.data).type == MessageType::reset);
    channel.sent(0);
  }
  for (const auto* id : {"main", "dash"}) {
    CHECK(receive(channel, id, peer(MessageType::reset_complete)).session.result == Result::ready);
    CHECK(channel.state(id) == State::ready);
  }
  for (const auto* id : {"main", "dash"}) {
    const auto message = ui::decode(*channel.pending());
    CHECK(message.display_uuid == id);
    CHECK(headunit::theme::decode(message.data).type == MessageType::reset_complete);
    channel.sent(1);
  }
  CHECK(!channel.pending());
}
void codec_and_bounds() {
  ui::Message message{"opaque-display-id", headunit::theme::encode(peer(MessageType::reset))};
  CHECK(ui::decode(ui::encode(message)) == message);
  const auto value = opack::decode(ui::encode(message));
  const auto& fields = std::get<opack::Dictionary>(value.data);
  CHECK(std::get<std::string>(fields[0].first.data) == "displayUUID");
  CHECK(std::get<std::string>(fields[1].first.data) == "data");
  rejects([&] { (void)ui::decode(message.data); });  // Bare bplist is not a channel envelope.
  rejects([&] { (void)ui::encode({"", message.data}); });
  rejects([&] { (void)ui::encode({std::string(257, 'x'), message.data}); });
  rejects([&] { (void)ui::encode({std::string("x\0y", 3), message.data}); });
  rejects([&] { (void)ui::encode({"main", {}}); });
  rejects([&] { (void)ui::encode({"main", Bytes(ui::max_payload_bytes + 1)}); });
  auto wrong = fields;
  wrong[0].second = opack::Value{std::int64_t(7)};
  rejects([&] { (void)ui::decode(opack::encode(opack::Value{wrong})); });
  wrong = fields;
  wrong[1].second = opack::Value{std::string("not NSData")};
  rejects([&] { (void)ui::decode(opack::encode(opack::Value{wrong})); });
  wrong = fields;
  wrong.pop_back();
  rejects([&] { (void)ui::decode(opack::encode(opack::Value{wrong})); });
  rejects([] { ui::Channel bad({}); });
  rejects([] { ui::Channel bad({{"same", {}}, {"same", {}}}); });
}
void isolated_sessions_and_fair_output() {
  ui::Channel channel({{"main", "vehicle"}, {"dash", "vehicle"}}, 2);
  ready(channel);
  auto p = peer(MessageType::command, 2, 2);
  p.data = command();
  const auto routed = receive(channel, "dash", p);
  CHECK(routed.display_uuid == "dash" && routed.session.result == Result::command);
  CHECK(routed.session.command == p.data);
  auto reply = ui::decode(*channel.pending());
  CHECK(reply.display_uuid == "dash");
  CHECK(headunit::theme::decode(reply.data).type == MessageType::ack);
  channel.sent(2);
  CHECK(!channel.pending());
  CHECK(receive(channel, "MAIN", p).session.result == Result::ignored);
  CHECK(!channel.state("MAIN") && !channel.send_command("MAIN", command()));
  const auto malformed = channel.receive(ui::encode({"main", Bytes{1, 2, 3}}));
  CHECK(malformed.display_uuid == "main" && malformed.session.result == Result::malformed);
  CHECK(channel.state("main") == State::ready && channel.state("dash") == State::ready);
  CHECK(!channel.receive(Bytes{0xff}).display_uuid);
  CHECK(channel.send_command("main", command()));
  CHECK(channel.send_command("main", command()));
  CHECK(!channel.send_command("main", command()));
  CHECK(channel.send_command("dash", command()));
  for (const auto* id : {"main", "dash", "main"}) {
    const auto* pending = channel.pending();
    const auto copy = *pending;
    CHECK(*channel.pending() == copy && ui::decode(copy).display_uuid == id);
    channel.sent(3);
  }
  CHECK(!channel.pending());
  channel.stop();
  CHECK(!channel.pending());
  CHECK(channel.state("main") == State::stopped);
  CHECK(!channel.send_command("dash", command()));
}
void reset_and_deadline_invalidate_only_their_display() {
  ui::Channel channel({{"main", "vehicle"}, {"dash", "vehicle"}});
  ready(channel);
  CHECK(channel.send_command("main", command()));
  const auto held = *channel.pending();
  auto gap = peer(MessageType::command, 4, 2);
  gap.data = command();
  CHECK(receive(channel, "main", gap).session.result == Result::reset_started);
  const auto replacement = ui::decode(*channel.pending());
  CHECK(replacement.display_uuid == "main" && *channel.pending() != held);
  CHECK(headunit::theme::decode(replacement.data).session == 2);
  CHECK(channel.state("dash") == State::ready);
  channel.sent(10);
  CHECK(channel.send_command("dash", command()));
  const auto dash = *channel.pending();
  CHECK(channel.tick(1010));  // Main's reset times out while dash has a held frame.
  CHECK(*channel.pending() == dash);
  channel.sent(1011);
  const auto reset = ui::decode(*channel.pending());
  CHECK(reset.display_uuid == "main" && headunit::theme::decode(reset.data).session == 3);
  channel.stop();
  CHECK(!channel.tick(50000));
  CHECK(!channel.pending());
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--roundtrip") {
      Bytes input;
      std::array<char, 4096> buffer{};
      while (std::cin) {
        std::cin.read(buffer.data(), buffer.size());
        input.insert(input.end(), buffer.begin(), buffer.begin() + std::cin.gcount());
        if (input.size() > max_packet_bytes)
          throw std::invalid_argument("oversized roundtrip input");
      }
      const auto result = ui::encode(ui::decode(input));
      std::cout.write(reinterpret_cast<const char*>(result.data()), result.size());
      return 0;
    }
    codec_and_bounds();
    isolated_sessions_and_fair_output();
    reset_and_deadline_invalidate_only_their_display();
    std::cout << "3 UI-sync channel codec/routing groups passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
