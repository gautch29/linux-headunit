// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_transport.hpp>
#include <headunit/ultra_ui_envelope.hpp>
#include <headunit/binary_plist.hpp>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace headunit;
using namespace headunit::theme::script;
using B = std::vector<std::uint8_t>;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::exception&) {
    rejected = true;
  }
  CHECK(rejected);
}
const std::string main_id = "aabbccdd-0000-0000-0000-000000000001";
const std::string dash_id = "aabbccdd-0000-0000-0000-000000000002";
ResourceReader reader = [](std::string_view) -> std::string {
  throw std::runtime_error("no fixture resource");
};
std::vector<DisplayRoute> routes() {
  return {{main_id, "Center_Display"}, {dash_id, "Cluster_Display"}};
}
B plist() {
  return encode_binary_plist(
      PlistValue{PlistValue::Dictionary{{"n", PlistValue{std::int64_t{42}}}}});
}
DataMessage message(const std::string& id, const B& bytes) {
  DataMessage m;
  m.type = 0x636f6d6d;
  m.payload = encode_ultra_ui_envelope({id, bytes});
  return m;
}
B cycle() {
  // Independently laid out root array refers to itself; libplist must never see it.
  B b{'b', 'p', 'l', 'i', 's', 't', '0', '0', 0xa1, 0, 8};
  b.insert(b.end(), 6, 0);
  b.push_back(1);
  b.push_back(1);
  for (auto n : {1, 0, 10}) {
    b.insert(b.end(), 7, 0);
    b.push_back(n);
  }
  return b;
}
void routing_and_flow() {
  Channel channel(reader, {{"Center_Display"}, {"Cluster_Display"}});
  rejects([&] { UiTransport bad(channel, 0, routes()); });
  rejects([&] { UiTransport bad(channel, 3, {{main_id, "missing"}}); });
  rejects([&] {
    UiTransport bad(
        channel, 3,
        {{main_id, "Center_Display"}, {"AABBCCDD-0000-0000-0000-000000000001", "Cluster_Display"}});
  });
  UiTransport transport(channel, 3, routes());
  auto m = message(main_id, plist());
  CHECK(transport.receive(3, m) == TransportDelivery::closed);
  transport.start();
  CHECK(transport.receive(4, m) == TransportDelivery::wrong_stream);
  auto bad = m;
  bad.kind = DataMessageKind::synchronous;
  CHECK(transport.receive(3, bad) == TransportDelivery::unsupported);
  bad = m;
  bad.type = 0x636d6e64;
  CHECK(transport.receive(3, bad) == TransportDelivery::unsupported);
  bad = m;
  bad.type = 0;
  CHECK(transport.receive(3, bad) == TransportDelivery::unsupported);
  bad = m;
  bad.error = -1;
  CHECK(transport.receive(3, bad) == TransportDelivery::unsupported);
  CHECK(transport.receive(3, message("aabbccdd-0000-0000-0000-000000000099", plist())) ==
        TransportDelivery::malformed);
  CHECK(transport.receive(3, message(main_id, cycle())) == TransportDelivery::malformed);
  const auto nested =
      encode_binary_plist(PlistValue{PlistValue::Dictionary{{"data", PlistValue{cycle()}}}});
  CHECK(transport.receive(3, message(main_id, nested)) == TransportDelivery::malformed);
  CHECK(transport.receive(3, message("AABBCCDD-0000-0000-0000-000000000001", plist())) ==
        TransportDelivery::accepted);
  channel.runtime("Cluster_Display").evaluate("assert(CP.receiveMessage()==nil)");
  channel.runtime("Center_Display")
      .evaluate(
          "local "
          "m=CP.parsePlist(CP.receiveMessage());assert(m.n==42);assert(CP.sendMessage(CP."
          "createPlist(m)))");
  CHECK(transport.pending());
  const auto held = *transport.pending();
  CHECK(*transport.pending() == held);
  CHECK(held.payload == m.payload);
  CHECK(held.type == 0x636d6e64 && m.type == 0x636f6d6d);
  channel.runtime("Cluster_Display").evaluate("assert(CP.sendMessage(CP.createPlist({n=43})))");
  CHECK(*transport.pending() == held);
  transport.accepted();
  const std::array ids{main_id, dash_id};
  CHECK(decode_ultra_ui_envelope(transport.pending()->payload, ids).display_uuid == dash_id);
  transport.accepted();
  CHECK(!transport.pending());
  rejects([&] { transport.accepted(); });
  for (int i = 0; i < 64; ++i)
    CHECK(transport.receive(3, m) == TransportDelivery::accepted);
  CHECK(transport.receive(3, m) == TransportDelivery::backpressure);
  CHECK(transport.receive(3, message(dash_id, plist())) == TransportDelivery::accepted);
  channel.runtime("Cluster_Display").evaluate("assert(CP.sendMessage(CP.createPlist({n=44})))");
  CHECK(transport.pending());
  transport.close();
  CHECK(!transport.pending());
  CHECK(transport.receive(3, m) == TransportDelivery::closed);
  rejects([&] { transport.start(); });
  channel.runtime("Center_Display")
      .evaluate("assert(CP.receiveMessage()==nil);assert(not CP.sendMessage(CP.createPlist({})))");
}
void encrypted_delivery() {
  Channel channel(reader, {{"Center_Display"}, {"Cluster_Display"}});
  UiTransport transport(channel, 7, routes());
  transport.start();
  HapKey32 secret{};
  secret.fill(0x31);
  AirplayDataStream wire(secret, 91);
  auto m = message(dash_id, plist());
  B plain(32, 0);
  const auto length = plain.size() + m.payload.size();
  for (unsigned i = 0; i < 4; ++i)
    plain[i] = length >> (8 * (3 - i));
  plain[4] = 'a';
  plain[5] = 's';
  plain[6] = 'y';
  plain[7] = 'n';
  plain[16] = 'c';
  plain[17] = 'o';
  plain[18] = 'm';
  plain[19] = 'm';
  plain.insert(plain.end(), m.payload.begin(), m.payload.end());
  const auto key = hap_hkdf_sha512(secret, "DataStream-Salt91", "DataStream-Output-Encryption-Key");
  B incoming{static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8)};
  const auto encrypted = hap_encrypt(key, std::string(8, 0), plain, incoming);
  incoming.insert(incoming.end(), encrypted.begin(), encrypted.end());
  std::size_t delivered = 0;
  for (const auto byte : incoming)
    for (const auto& value : wire.feed(std::span(&byte, 1))) {
      CHECK(transport.receive(7, value) == TransportDelivery::accepted);
      ++delivered;
    }
  CHECK(delivered == 1);
  channel.runtime("Cluster_Display")
      .evaluate(
          "local "
          "m=CP.parsePlist(CP.receiveMessage());assert(m.n==42);assert(CP.sendMessage(CP."
          "createPlist(m)))");
  const auto* reply = transport.pending();
  CHECK(reply);
  const auto output = wire.encode(*reply);
  transport.accepted();
  const auto write_key =
      hap_hkdf_sha512(secret, "DataStream-Salt91", "DataStream-Input-Encryption-Key");
  const auto recovered = hap_decrypt(write_key, std::string(8, 0), std::span(output).subspan(2),
                                     std::span(output).first(2));
  // Directional labels from the working authored bridge: comm in, cmnd out.
  auto expected = plain;
  expected[17] = 'm';
  expected[18] = 'n';
  expected[19] = 'd';
  CHECK(recovered && *recovered == expected);
  // Replay fails authentication before any message can reach the VM.
  rejects([&] { (void)wire.feed(incoming); });
  CHECK(wire.failed());
  channel.runtime("Cluster_Display").evaluate("assert(CP.receiveMessage()==nil)");
}
void nested_parse_guard() {
  Channel channel(reader, {{"Center_Display"}});
  channel.set_connected(true);
  const auto wrapped =
      encode_binary_plist(PlistValue{PlistValue::Dictionary{{"bad", PlistValue{cycle()}}}});
  CHECK(channel.runtime("Center_Display").receive(wrapped));
  channel.runtime("Center_Display")
      .evaluate(
          "local m=CP.parsePlist(CP.receiveMessage());assert(not pcall(CP.parsePlist,m.bad))");
  CHECK(!channel.runtime("Center_Display").stats().faulted);
}
void signed_panel_routes() {
  Channel channel(reader, {{"Center_Display"}, {"Cluster_Display"}});
  UiTransport transport(
      channel, 3, {{"Center_Display", "Center_Display"}, {"Cluster_Display", "Cluster_Display"}});
  transport.start();
  CHECK(transport.receive(3, message("center_Display", plist())) == TransportDelivery::malformed);
  CHECK(transport.rejection_reason() == "unknown Ultra UI display");
  auto input = message("Center_Display", plist());
  CHECK(transport.receive(3, input) == TransportDelivery::accepted &&
        transport.rejection_reason().empty());
  channel.runtime("Center_Display").evaluate("assert(CP.sendMessage(CP.receiveMessage()))");
  auto expected = input;
  expected.type = 0x636d6e64;
  CHECK(transport.pending() && *transport.pending() == expected);
  channel.runtime("Cluster_Display").evaluate("assert(CP.receiveMessage()==nil)");
}
void reset_invalidates_only_its_display() {
  Channel channel(reader, {{"Center_Display"}, {"Cluster_Display"}});
  UiTransport transport(channel, 3, routes());
  transport.start();
  auto& main = channel.runtime("Center_Display");
  auto& dash = channel.runtime("Cluster_Display");
  main.evaluate("assert(CP.sendMessage(CP.createPlist({n=1})))");
  CHECK(transport.pending());
  const auto old = *transport.pending();
  main.evaluate("CP.reset();assert(CP.sendMessage(CP.createPlist({n=2})))");
  CHECK(transport.pending() && *transport.pending() != old);
  transport.accepted();
  dash.evaluate("assert(CP.sendMessage(CP.createPlist({n=3})))");
  CHECK(transport.pending());
  const auto kept = *transport.pending();
  main.evaluate("CP.reset()");
  CHECK(*transport.pending() == kept);
  transport.accepted();
  main.evaluate("assert(CP.sendMessage(CP.createPlist({n=4})))");
  CHECK(transport.pending());
  main.evaluate("CP.reset()");
  rejects([&] { transport.accepted(); });
  CHECK(!transport.pending());
  main.evaluate("assert(CP.sendMessage(CP.createPlist({n=5})))");
  CHECK(transport.pending());
  rejects([&] { main.evaluate("error('fault')"); });
  CHECK(!transport.pending());
}
B command_plist(std::int64_t type, PlistValue::Dictionary data) {
  // Original synthetic command fixture, independent of signed resources.
  const auto command = encode_binary_plist(PlistValue{
      PlistValue::Dictionary{{"type", PlistValue{type}}, {"data", PlistValue{std::move(data)}}}});
  return encode_binary_plist(PlistValue{PlistValue::Dictionary{
      {"type", PlistValue{std::int64_t(2)}}, {"data", PlistValue{command}}}});
}
void transition_timestamps() {
  Channel channel(reader, {{"Center_Display"}, {"Cluster_Display"}});
  std::vector<UiCommandDiagnostic> events;
  UiTransport transport(channel, 3, routes(),
                        [&](const UiCommandDiagnostic& event) { events.push_back(event); });
  transport.start();
  auto& runtime = channel.runtime("Cluster_Display");
  // A source 32.32 timestamp has the same seconds/fraction as phone video.
  constexpr std::int64_t ntp = (123LL << 32) | 0x40000000;
  for (std::int64_t type : {1, 2, 3}) {
    const auto bytes = command_plist(type, {{"current_ntp_time", PlistValue{ntp}},
                                            {"regions", PlistValue{PlistValue::Array{}}}});
    CHECK(transport.receive(3, message(dash_id, bytes)) == TransportDelivery::accepted);
    CHECK(events.back().converted_transition_timestamp);
    runtime.evaluate(R"(
      local session=CP.parsePlist(CP.receiveMessage())
      local command=CP.parsePlist(session.data)
      assert(command.data.pts==123.25)
      assert(command.data.current_ntp_time==529354719232)
      assert(#command.data.regions==0)
      assert(CP.sendMessage(CP.createPlist(session)))
    )");
    CHECK(transport.pending());
    transport.accepted();
    CHECK(!events.back().converted_transition_timestamp);  // Outbound never converts.
  }
  const std::vector<PlistValue::Dictionary> unchanged{
      {{"current_ntp_time", PlistValue{ntp}}, {"pts", PlistValue{4.5}}},
      {{"current_ntp_time", PlistValue{ntp}}, {"pts", PlistValue{false}}},
      {{"current_ntp_time", PlistValue{std::int64_t(0)}}},
      {{"current_ntp_time", PlistValue{std::int64_t(-1)}}},
      {{"current_ntp_time", PlistValue{123.25}}},
      {{"current_ntp_time", PlistValue{std::string("529354719232")}}},
      {{"regions", PlistValue{PlistValue::Array{}}}}};
  for (const auto& data : unchanged) {
    auto input = message(dash_id, command_plist(2, data));
    CHECK(transport.receive(3, input) == TransportDelivery::accepted);
    CHECK(!events.back().converted_transition_timestamp);
    runtime.evaluate("assert(CP.sendMessage(CP.receiveMessage()))");
    input.type = 0x636d6e64;
    CHECK(transport.pending() && *transport.pending() == input);
    transport.accepted();
  }
  auto input = message(dash_id, command_plist(9, {{"current_ntp_time", PlistValue{ntp}}}));
  CHECK(transport.receive(3, input) == TransportDelivery::accepted);
  CHECK(!events.back().converted_transition_timestamp);
  runtime.evaluate("assert(CP.sendMessage(CP.receiveMessage()))");
  input.type = 0x636d6e64;
  CHECK(transport.pending() && *transport.pending() == input);
}
void command_diagnostics() {
  Channel channel(reader, {{"Center_Display"}, {"Cluster_Display"}});
  std::vector<UiCommandDiagnostic> events;
  UiTransport transport(
      channel, 3, routes(), [&](const auto& event) { events.push_back(event); }, 8);
  transport.start();
  const auto focus =
      command_plist(8, {{"tokenID", PlistValue{std::int64_t(77)}},
                        {"unknownPrivateField", PlistValue{std::string("never emitted")}}});
  auto input = message(main_id, focus);
  CHECK(transport.receive(4, input) == TransportDelivery::wrong_stream);
  auto unsupported = input;
  unsupported.kind = DataMessageKind::synchronous;
  CHECK(transport.receive(3, unsupported) == TransportDelivery::unsupported);
  CHECK(transport.receive(3, message(main_id, cycle())) == TransportDelivery::malformed);
  CHECK(transport.receive(3, message("aabbccdd-0000-0000-0000-000000000099", focus)) ==
        TransportDelivery::malformed);
  CHECK(events.empty());
  const auto generation = channel.runtime("Center_Display").message_generation();
  CHECK(transport.receive(3, input) == TransportDelivery::accepted && events.size() == 1);
  const auto& first = events.front();
  CHECK(first.panel == 0 && first.direction == UiCommandDiagnostic::Direction::incoming_vm_queue &&
        first.generation == generation && first.count == 1 && first.monotonic_ns > 0 &&
        !first.budget_exhausted);
  CHECK(first.command_type == 8 && first.focus_token == 77 &&
        first.type_shape == UiFieldShape::integer && first.data_shape == UiFieldShape::dictionary &&
        first.fields[0] == UiFieldShape::integer && !first.transitioning);
  CHECK(transport.receive(3, message(dash_id, focus)) == TransportDelivery::accepted &&
        events.size() == 2);
  CHECK(events.back().panel == 1 && events.back().count == 1);
  auto& main = channel.runtime("Center_Display");
  auto& dash = channel.runtime("Cluster_Display");
  main.evaluate("assert(CP.sendMessage(CP.receiveMessage()))");
  dash.evaluate("assert(CP.sendMessage(CP.receiveMessage()))");
  CHECK(transport.pending());
  const auto held = *transport.pending();
  for (unsigned n = 0; n < 4; ++n)
    CHECK(*transport.pending() == held && events.size() == 2);
  transport.accepted();
  CHECK(events.size() == 3 && events.back().panel == 0 && events.back().count == 2 &&
        events.back().direction == UiCommandDiagnostic::Direction::outgoing_worker_queue &&
        events.back().focus_token == 77);
  CHECK(transport.pending());
  transport.accepted();
  CHECK(events.size() == 4 && events.back().panel == 1 &&
        events.back().direction == UiCommandDiagnostic::Direction::outgoing_worker_queue);
  // Shape inspection is descriptive: a semantically malformed token remains
  // delivered unchanged to the asset, and no arbitrary string leaves the route.
  const auto malformed = command_plist(7, {{"tokenID", PlistValue{std::string("not a token")}}});
  CHECK(transport.receive(3, message(main_id, malformed)) == TransportDelivery::accepted);
  CHECK(events.back().command_type == 7 && !events.back().focus_token &&
        events.back().fields[0] == UiFieldShape::string);
  main.evaluate(
      "local m=CP.parsePlist(CP.receiveMessage());local "
      "c=CP.parsePlist(m.data);assert(c.data.tokenID=='not a token')");
  const auto transition =
      command_plist(9, {{"pts", PlistValue{1.25}},
                        {"layoutID", PlistValue{std::string("private layout")}},
                        {"activeComponents", PlistValue{PlistValue::Dictionary{}}},
                        {"targetComponents", PlistValue{PlistValue::Array{}}},
                        {"presentStream", PlistValue{B{1, 2}}},
                        {"dismissStream", PlistValue{true}},
                        {"isTransitioning", PlistValue{false}}});
  CHECK(transport.receive(3, message(main_id, transition)) == TransportDelivery::accepted);
  const auto expected_shapes = std::array{
      UiFieldShape::absent, UiFieldShape::real,  UiFieldShape::string,  UiFieldShape::dictionary,
      UiFieldShape::array,  UiFieldShape::bytes, UiFieldShape::boolean, UiFieldShape::boolean};
  CHECK(events.back().fields == expected_shapes);
  CHECK(events.back().transitioning == false && !events.back().focus_token);
  // VM reset retires held output; it neither emits acceptance nor refills the
  // lifetime budget. A new event carries the actual replacement generation.
  main.evaluate("assert(CP.sendMessage(CP.receiveMessage()))");
  CHECK(transport.pending());
  const auto before_reset = events.size();
  main.evaluate("CP.reset()");
  rejects([&] { transport.accepted(); });
  CHECK(events.size() == before_reset && !transport.pending());
  CHECK(transport.receive(3, input) == TransportDelivery::accepted);
  CHECK(events.back().generation != generation && events.back().count == 5);
  for (unsigned n = 0; n < 8; ++n)
    CHECK(transport.receive(3, input) == TransportDelivery::accepted);
  CHECK(events.back().panel == 0 && events.back().count == 8 && events.back().budget_exhausted);
  const auto capped = events.size();
  main.evaluate("CP.reset()");
  CHECK(transport.receive(3, input) == TransportDelivery::accepted && events.size() == capped);
  CHECK(transport.receive(3, message(dash_id, focus)) == TransportDelivery::accepted &&
        events.size() == capped + 1);
  CHECK(events.back().panel == 1 && events.back().count == 3 && !events.back().budget_exhausted);
}
void diagnostic_failures_and_backpressure() {
  Channel channel(reader, {{"Center_Display"}, {"Cluster_Display"}});
  rejects([&] { UiTransport bad(channel, 3, routes(), {}, 0); });
  rejects([&] { UiTransport bad(channel, 3, routes(), {}, 257); });
  std::size_t calls = 0;
  UiTransport transport(channel, 3, routes(), [&](const auto&) {
    ++calls;
    throw std::runtime_error("sink failure");
  });
  transport.start();
  const auto focus =
      message(main_id, command_plist(7, {{"tokenID", PlistValue{std::int64_t(11)}}}));
  for (unsigned n = 0; n < 64; ++n)
    CHECK(transport.receive(3, focus) == TransportDelivery::accepted);
  CHECK(calls == 64 && transport.receive(3, focus) == TransportDelivery::backpressure &&
        calls == 64);
  auto& main = channel.runtime("Center_Display");
  main.evaluate("assert(CP.sendMessage(CP.receiveMessage()))");
  CHECK(transport.pending());
  transport.accepted();
  CHECK(calls == 65 && !main.stats().faulted);
  CHECK(transport.receive(3, focus) == TransportDelivery::accepted && calls == 66);
  transport.close();
  CHECK(transport.receive(3, focus) == TransportDelivery::closed && calls == 66);
}
int main() try {
  routing_and_flow();
  encrypted_delivery();
  nested_parse_guard();
  signed_panel_routes();
  reset_invalidates_only_its_display();
  command_diagnostics();
  transition_timestamps();
  diagnostic_failures_and_backpressure();
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
