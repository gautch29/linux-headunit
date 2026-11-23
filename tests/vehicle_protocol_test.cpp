// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_bench.hpp>
#include <headunit/theme/vehicle_transport.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <algorithm>
using namespace headunit;
namespace v = headunit::theme::vehicle;
namespace op = headunit::theme::opack;
using op::Value;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
Value text(std::string s) {
  return Value{std::move(s)};
}
Value number(std::int64_t n) {
  return Value{n};
}
Value dict(std::initializer_list<op::DictionaryEntry> pairs) {
  return Value{op::Dictionary(pairs)};
}
Value array(std::initializer_list<Value> values) {
  return Value{op::Array(values)};
}
const Value& field(const Value& value, std::string_view name) {
  for (const auto& [k, v] : std::get<op::Dictionary>(value.data))
    if (k == text(std::string(name))) return v;
  throw std::runtime_error("missing field");
}
const Value& find(const Value& config, std::int64_t iid) {
  for (const auto& a : std::get<op::Array>(field(config, "accessories").data)) {
    if (field(a, "iid") == number(iid)) return a;
    for (const auto& s : std::get<op::Array>(field(a, "services").data)) {
      if (field(s, "iid") == number(iid)) return s;
      for (const auto& c : std::get<op::Array>(field(s, "characteristics").data))
        if (field(c, "iid") == number(iid)) return c;
    }
  }
  throw std::runtime_error("missing iid");
}
void rejects(auto f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid input accepted");
}
v::Command request(v::Service& s, std::string name, std::optional<Value> values,
                   std::uint64_t now) {
  auto result =
      s.handle(v::encode_outgoing({std::move(name), 1, std::move(values), {}, {}}, 0), now);
  CHECK(result);
  return v::decode_incoming(*result).command;
}
DataMessage message(std::string name, std::optional<Value> values = {}) {
  DataMessage m;
  m.kind = DataMessageKind::synchronous;
  m.group = 23;
  m.token = 42;
  m.type = 0x636f6d6d;
  m.payload = v::encode_outgoing({std::move(name), 1, std::move(values), {}, {}}, 0);
  return m;
}
int main() try {
  v::Bench bench(v::BenchProfile::combined);
  bench.refresh(1000);
  const auto config = bench.service().configuration_snapshot(1000);
  CHECK(field(find(config, 100), "type") == number(0x01000001));
  CHECK(field(find(config, 101), "type") == number(0x11000002));
  CHECK(field(find(config, 112), "type") == number(0x31000012));
  CHECK(field(find(config, 103), "format") == number(9));
  CHECK(field(find(config, 103), "units") == number(1082));
  CHECK(field(find(config, 105), "initialValue") == text("seat_front_left"));
  CHECK(field(find(config, 200), "type") == number(0x05000001));
  CHECK(field(find(config, 201), "type") == number(0x16000001));
  auto r = request(bench.service(), "configRequest", {}, 1000);
  CHECK(r.values == config);
  request(bench.service(), "registerRequest", text("*"), 1000);
  CHECK(!bench.service().updates(1000));
  r = request(bench.service(), "writeRequest", dict({{number(103), Value{22.5}}}), 1001);
  CHECK(!r.error && !r.errors && bench.target_c() == 22.5);
  bench.refresh(1100);
  r = v::decode_incoming(*bench.service().updates(1100)).command;
  CHECK(r.values == dict({{number(102), Value{22.5}}, {number(103), Value{22.5}}}));
  for (const auto& invalid : {Value{31.0}, Value{22.25}, number(22), text("22")}) {
    r = request(bench.service(), "writeRequest", dict({{number(103), invalid}}), 1101);
    CHECK(r.errors && bench.target_c() == 22.5);
  }
  rejects([&] {
    request(bench.service(), "writeRequest",
            dict({{number(103), Value{std::numeric_limits<double>::quiet_NaN()}}}), 1101);
  });
  r = request(bench.service(), "writeRequest", dict({{number(112), number(100)}}), 1102);
  CHECK(!r.errors && bench.fan_level() == 100);
  r = request(bench.service(), "writeRequest", dict({{number(112), number(101)}}), 1103);
  CHECK(r.errors && bench.fan_level() == 100);
  r = request(bench.service(), "writeRequest", dict({{number(202), Value{true}}}), 1104);
  CHECK(!r.errors && bench.comfort_lighting());
  r = request(bench.service(), "writeRequest", dict({{number(102), Value{24.0}}}), 1105);
  CHECK(r.errors);
  r = request(bench.service(), "writeRequest",
              dict({{number(103), Value{24.0}}, {number(112), number(10)}}), 1105);
  CHECK(!r.error && !r.errors && bench.target_c() == 24.0 && bench.fan_level() == 10 &&
        r.values == dict({{number(103), Value{24.0}}, {number(112), number(10)}}));
  bench.disconnect();
  bench.refresh(0);  // Reconnect republishes immediately, preserving settings.
  CHECK(bench.service().subscription_count() == 0);
  CHECK(field(find(bench.service().configuration_snapshot(0), 103), "initialValue") == Value{24.0});
  rejects([&] {
    bench.refresh(0 - std::uint64_t(1));
    bench.refresh(0);
  });
  bench.disconnect();
  bench.refresh(1000);
  for (const auto profile :
       {v::BenchProfile::empty, v::BenchProfile::climate, v::BenchProfile::settings}) {
    v::Bench b(profile);
    b.refresh(0);
    const auto& accessories = field(b.service().profile(), "accessories");
    CHECK(std::get<op::Array>(accessories.data).size() ==
          (profile == v::BenchProfile::empty ? 0 : 1));
  }
  rejects([] { v::bench_profile("climate"); });
  std::vector<std::pair<v::Command, v::Command>> observed_writes;
  v::Transport normal(bench.service(),
                      [&](const auto& request, const auto& response) {
                        observed_writes.emplace_back(request, response);
                      }),
      high(bench.service());
  CHECK(normal.receive(message("configRequest"), 1000));
  CHECK(normal.pending()->kind == DataMessageKind::reply && normal.pending()->token == 42 &&
        normal.pending()->group == 23 && !normal.pending()->error);
  normal.sent();
  CHECK(normal.pending()->kind == DataMessageKind::asynchronous && normal.pending()->token == 0);
  CHECK(v::decode_incoming(normal.pending()->payload).command.values ==
        bench.service().configuration_snapshot(1000));
  normal.sent();
  CHECK(high.receive(message("registerRequest", array({number(103)})), 1000));
  while (high.pending())
    high.sent();
  CHECK(normal.receive(message("writeRequest", dict({{number(103), Value{23.0}}})), 1001));
  CHECK(bench.target_c() == 23.0 && normal.writes() == 1);
  CHECK(observed_writes.size() == 1 && observed_writes.back().first.name == "writeRequest");
  CHECK(observed_writes.back().first.values == dict({{number(103), Value{23.0}}}));
  CHECK(observed_writes.back().second.name == "writeResponse" &&
        !observed_writes.back().second.errors);
  auto update = bench.service().updates(1001);
  CHECK(update && high.notify(*update));
  CHECK(v::decode_incoming(high.pending()->payload).command.values ==
        dict({{number(103), Value{23.0}}}));
  high.sent();
  while (normal.pending())
    normal.sent();
  auto unsupported = message("configRequest");
  unsupported.type = 0x12345678;
  CHECK(normal.receive(unsupported, 1001));
  CHECK(normal.pending()->error == -6710);
  normal.sent();
  CHECK(!normal.pending());
  auto asynchronous = message("readRequest", array({number(103)}));
  asynchronous.kind = DataMessageKind::asynchronous;
  CHECK(normal.receive(asynchronous, 1001));
  CHECK(normal.pending()->kind == DataMessageKind::asynchronous);
  normal.sent();
  CHECK(!normal.pending());
  auto malformed = message("readRequest");
  malformed.payload = {0xff};
  rejects([&] { normal.receive(malformed, 1001); });
  CHECK(!normal.pending());
  // A queued command is retried intact after bounded output pressure, not applied twice.
  for (unsigned i = 0; i < 31; ++i)
    CHECK(normal.notify(*update));
  const auto write = message("writeRequest", dict({{number(103), Value{24.0}}}));
  CHECK(!normal.receive(write, 1002));
  CHECK(bench.target_c() == 23.0 && normal.writes() == 1);
  CHECK(observed_writes.size() == 1);  // A blocked retry has no diagnostic side effects either.
  normal.sent();
  CHECK(normal.receive(write, 1002));
  CHECK(bench.target_c() == 24.0 && normal.writes() == 2);
  CHECK(observed_writes.size() == 2);
  while (normal.pending())
    normal.sent();
  CHECK(normal.receive(message("writeRequest", dict({{number(103), Value{99.0}}})), 1003));
  CHECK(observed_writes.size() == 3 && observed_writes.back().second.errors &&
        bench.target_c() == 24.0);
  while (normal.pending())
    normal.sent();
  // Diagnostics cannot turn an applied write into a failed transport transaction.
  v::Transport broken_observer(bench.service(), [](const auto&, const auto&) { throw 1; });
  CHECK(broken_observer.receive(message("writeRequest", dict({{number(103), Value{25.0}}})), 1004));
  CHECK(bench.target_c() == 25.0 && broken_observer.pending());
  // /info uses the same numeric metadata and initialized values as the socket profile.
  const auto info = encode_binary_plist(
      PlistValue{PlistValue::Dictionary{{"name", PlistValue{std::string("Bench")}}}});
  const auto advertised = v::add_server_info(info, bench.service(), 1004);
  const auto parsed = decode_binary_plist(advertised);
  const auto& root = std::get<PlistValue::Dictionary>(parsed.value);
  CHECK(std::get<std::string>(root.at("name").value) == "Bench");
  const auto& protocol =
      std::get<PlistValue::Dictionary>(root.at("vehicleStateProtocolInfo").value);
  const auto& plugin = std::get<PlistValue::Dictionary>(
      std::get<PlistValue::Array>(protocol.at("pluginConfigs").value).front().value);
  CHECK(std::get<std::int64_t>(plugin.at("pluginID").value) == 0);
  CHECK(std::get<PlistValue::Array>(plugin.at("accessories").value).size() == 2);
  rejects([&] { v::add_server_info(advertised, bench.service(), 1004); });
  {
    v::Bench model(v::BenchProfile::combined);
    model.refresh(2000);
    auto coverage = std::make_shared<v::Coverage>(model.service().profile());
    v::Transport ordinary(model.service(), {}, {}, coverage, 0);
    v::Transport priority(model.service(), {}, {}, coverage, 1);
    const auto row = [&](std::int64_t iid) {
      const auto resources = coverage->resources();
      const auto found = std::find_if(resources.begin(), resources.end(),
                                      [&](const auto& entry) { return entry.iid == iid; });
      CHECK(found != resources.end());
      return *found;
    };
    const auto drain = [](v::Transport& transport) {
      while (transport.pending())
        transport.sent();
    };
    CHECK(ordinary.receive(message("readRequest", array({number(103), number(103)})), 2000));
    CHECK(row(103).channels[0].reads == 1 && row(103).channels[0].returned_values == 1 &&
          row(103).channels[0].transferred_values == 0);
    drain(ordinary);
    CHECK(row(103).service_iid == 101 && row(103).service_type == 0x11000002 &&
          row(103).channels[0].transferred_values == 1);
    CHECK(priority.receive(
        message("writeRequest", dict({{number(103), Value{22.0}}, {number(112), number(999)}})),
        2001));
    CHECK(model.target_c() == 22.0 && model.fan_level() != 999);
    CHECK(row(103).channels[1].confirmed_writes == 1 &&
          row(103).channels[1].transferred_values == 0 &&
          row(112).channels[1].returned_errors == 1);
    drain(priority);
    CHECK(row(103).channels[1].transferred_values == 1 &&
          row(112).channels[1].transferred_errors == 1);
    const auto before = row(103);
    auto other_plugin = message("readRequest", array({number(103)}));
    other_plugin.payload = v::encode_outgoing({"readRequest", 1, array({number(103)}), {}, {}}, 1);
    CHECK(ordinary.receive(other_plugin, 2001));
    drain(ordinary);
    CHECK(row(103) == before && coverage->summary().channels[0].response_errors == 1);
    CHECK(ordinary.notify(
        v::encode_outgoing({"updateNotify", {}, dict({{number(103), Value{12.0}}}), {}, {}}, 1)));
    drain(ordinary);
    CHECK(row(103) == before);
    CHECK(priority.receive(message("registerRequest", text("*")), 2001));
    drain(priority);
    CHECK(row(103).channels[1].registrations == 1);
    CHECK(priority.receive(message("unregisterRequest", text("*")), 2001));
    drain(priority);
    CHECK(coverage->summary().channels[1].requests.unregistration == 1);
    // A wildcard unregister has no returned IID set. Do not invent one.
    CHECK(row(103).channels[1].unregistrations == 0);
    for (std::int64_t iid = 9000; iid < 9049; ++iid) {
      CHECK(ordinary.receive(message("readRequest", array({number(iid)})), 2001));
      drain(ordinary);
    }
    const auto resources = coverage->resources();
    CHECK(std::count_if(resources.begin(), resources.end(), [](const auto& entry) {
            return !entry.service_iid;
          }) == v::Coverage::max_unknown);
    CHECK(coverage->summary().unknown_evictions == 17 && row(9048).channels[0].reads == 1 &&
          row(9048).channels[0].transferred_errors == 1 && row(103).channels[0].reads == 1);
    const auto update_message =
        v::encode_outgoing({"updateNotify", {}, dict({{number(103), Value{22.0}}}), {}, {}}, 0);
    for (unsigned i = 0; i < 31; ++i)
      CHECK(ordinary.notify(update_message));
    const auto applied = row(103).channels[0].confirmed_writes;
    const auto change = message("writeRequest", dict({{number(103), Value{23.0}}}));
    CHECK(!ordinary.receive(change, 2002));
    CHECK(row(103).channels[0].confirmed_writes == applied && model.target_c() == 22.0 &&
          coverage->summary().channels[0].retries == 1);
    ordinary.sent();
    CHECK(ordinary.receive(change, 2002));
    drain(ordinary);
    CHECK(row(103).channels[0].confirmed_writes == applied + 1 && model.target_c() == 23.0 &&
          coverage->summary().channels[0].writes == 1);
  }
  std::cout << "Vehicle bench and transport: numeric profiles, applied writes, snapshots, priority "
               "sharing and bounded retries passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
