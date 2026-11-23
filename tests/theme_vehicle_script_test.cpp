// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/script_runtime.hpp>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace headunit::theme;
namespace v = vehicle;
namespace op = opack;
using op::Value;
Value text(const char* s) {
  return Value{std::string(s)};
}
Value num(std::int64_t n) {
  return Value{n};
}
Value dict(std::initializer_list<op::DictionaryEntry> v) {
  return Value{op::Dictionary(v)};
}
Value arr(std::initializer_list<Value> v) {
  return Value{op::Array(v)};
}
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception&) {
    rejected = true;
  }
  CHECK(rejected);
}
std::shared_ptr<v::ReadOnlyService> model() {
  // Test IDs and paths are synthetic. They are not real vehicle characteristics.
  auto characteristic = [](int id, bool invalid) {
    return dict({{text("iid"), num(id)},
                 {text("type"), num(id)},
                 {text("format"), text("synthetic")},
                 {text("supportsInvalid"), Value{invalid}},
                 {text("units"), num(17)}});
  };
  auto profile =
      dict({{text("accessories"),
             arr({dict(
                 {{text("iid"), num(1)},
                  {text("type"), num(1)},
                  {text("services"),
                   arr({dict({{text("iid"), num(2)},
                              {text("type"), num(2)},
                              {text("characteristics"),
                               arr({characteristic(3, true), characteristic(4, false),
                                    characteristic(5, true), characteristic(6, true)})}})})}})})}});
  return std::make_shared<v::ReadOnlyService>(
      std::move(profile),
      std::vector<v::SignalPolicy>{{3, v::ValueKind::real, 100, 0.0, 200.0},
                                   {4, v::ValueKind::integer, 100, 0.0, 10000.0},
                                   {5, v::ValueKind::boolean, 100, {}, {}},
                                   {6, v::ValueKind::bytes, 100, {}, {}}});
}
std::vector<v::ScriptBinding> bindings() {
  return {{3, {"fixture", "measurement", "reading"}, 4},
          {4, {"fixture", "measurement", "counter"}, {}},
          {5, {"fixture", "measurement", "flag"}, {}},
          {6, {"fixture", "measurement", "data"}, {}}};
}
void mapping_validation() {
  const auto source = model();
  rejects([&] {
    auto b = bindings();
    b[0].iid = 99;
    v::ScriptView bad(source, b);
  });
  rejects([&] {
    auto b = bindings();
    b[0].path[0] = "fixture.other";
    v::ScriptView bad(source, b);
  });
  rejects([&] {
    auto b = bindings();
    b[1].path = b[0].path;
    v::ScriptView bad(source, b);
  });
  rejects([&] {
    auto b = bindings();
    b[1].iid = b[0].iid;
    v::ScriptView bad(source, b);
  });
  rejects([&] {
    auto b = bindings();
    b[0].format = -1;
    v::ScriptView bad(source, b);
  });
  rejects([&] {
    auto b = bindings();
    b[0].instance = "left";
    v::ScriptView bad(source, b);
  });
  for (const auto& invalid : {std::string{}, std::string{"bad.instance"}, std::string(129, 'x')})
    rejects([&] {
      auto b = bindings();
      for (auto& entry : b)
        entry.instance = invalid;
      v::ScriptView bad(source, b);
    });
  v::ScriptView valid(source, bindings());
  const std::array<std::int64_t, 1> unknown{99};
  rejects([&] { valid.read(unknown, 0); });
  CHECK(valid.reject_writes(unknown).errors.at(99) == -6727);
  const std::array<std::int64_t, 1> known{3};
  CHECK(valid.reject_writes(known).errors.at(3) == -6710);
}
void repeated_services(const std::filesystem::path& directory) {
  script::TrustedDirectory asset;
  asset.content_path = directory;
  auto source = model();
  std::uint64_t now = 1000;
  source->publish(3, Value{0.0}, now, now);
  source->publish(4, num(100), now, now);
  source->publish(5, Value{false}, now, now);
  source->publish(6, Value{Bytes{1, 2}}, now, now);
  auto mapping = bindings();
  for (auto& entry : mapping)
    entry.instance = entry.iid < 5 ? "left" : "right";
  mapping[2].path = mapping[0].path;
  mapping[3].path = mapping[1].path;
  std::reverse(mapping.begin(), mapping.end());  // Input order must not change instance identity.
  const auto original = source->profile();
  v::ScriptView view(source, mapping);
  (void)view.configuration(now);
  CHECK(source->profile() == original);
  rejects([&] {
    auto bad = mapping;
    bad[0].instance = "left";
    v::ScriptView duplicate(source, bad);
  });
  script::Runtime runtime(asset, {"main", 10, 10});
  runtime.bind_vehicle(source, mapping, [&] { return now; });
  runtime.evaluate(R"(
    local services=CP.getVehicleDataConfig().fixture.measurement
    assert(#services==2 and services.left==nil and services.reading==nil)
    assert(services[1].reading.iid==3 and services[1].reading.initialValue==0)
    assert(services[1].counter.iid==4 and services[1].counter.initialValue==100)
    assert(services[2].reading.iid==5 and services[2].reading.initialValue==false)
    assert(services[2].counter.iid==6 and #services[2].counter.initialValue==2)
    services[1].reading.iid=999
    assert(CP.getVehicleDataConfig().fixture.measurement[1].reading.iid==3)
    CP.registerVehicleDataTypes({3,4,5,6})
    local initial=CP.getVehicleDataUpdates();assert(initial[3]==0 and initial[4]==100 and initial[5]==false and #initial[6]==2)
  )",
                   "repeated-services");
  ++now;
  source->publish(5, Value{true}, now, now);
  runtime.evaluate(
      "local update=CP.getVehicleDataUpdates();assert(update[3]==nil and update[4]==nil and "
      "update[5]==true and #update[6]==2);assert(next(CP.getVehicleDataUpdates())==nil)");
  now += 99;
  runtime.evaluate(R"(
    local update=CP.getVehicleDataUpdates()
    assert(update[3]==CP.INVALIDVALUE and update[4]==CP.INVALIDVALUE)
    assert(update[5]==true and update[6]==CP.INVALIDVALUE)
    CP.registerVehicleDataTypes({5})
    local subset=CP.getVehicleDataUpdates();assert(subset[5]==true and subset[6]==nil)
  )");
}
void script_bridge(const std::filesystem::path& directory) {
  script::TrustedDirectory asset;
  asset.content_path = directory;
  auto source = model();
  std::uint64_t now = 1000;
  source->publish(3, Value{0.0}, now, now);
  source->publish(4, num(100), now, now);
  source->publish(5, Value{false}, now, now);
  source->publish(6, Value{Bytes{1, 2}}, now, now);
  script::Runtime a(asset, {"main", 10, 10}), b(asset, {"cluster", 10, 10});
  a.bind_vehicle(source, bindings(), [&] { return now; });
  b.bind_vehicle(source, bindings(), [&] { return now; });
  a.evaluate(R"(
    local config=CP.getVehicleDataConfig()
    assert(config.fixture.measurement.reading.iid==3 and config.fixture.measurement.reading.initialValue==0)
    assert(config.fixture.measurement.reading.format==4 and config.fixture.measurement.reading.units==17)
    assert(config.fixture.measurement.counter.format==nil and config.fixture.measurement.flag.initialValue==false)
    assert(type(CP.INVALIDVALUE)=='userdata' and CP.INVALIDVALUE~=nil)
    assert(not pcall(function() CP.INVALIDVALUE.x=1 end))
    local data=CP.getVehicleData({3,4,5,6})
    assert(data[3]==0 and data[4]==100 and data[5]==false and #data[6]==2)
    assert(#CP.parsePlist(CP.createPlist({bytes=data[6]})).bytes==2)
    config.fixture.measurement.reading.iid=999
    assert(CP.getVehicleDataConfig().fixture.measurement.reading.iid==3)
    CP.registerVehicleDataTypes({3,4,3})
    local initial=CP.getVehicleDataUpdates();assert(initial[3]==0 and initial[4]==100 and initial[5]==nil)
    assert(next(CP.getVehicleDataUpdates())==nil)
    for _,invalid in ipairs({{true},{'3'},{[2]=3},{[1]=3,extra=4},{-1},{0/0},{1.25},{9007199254740992},{99}}) do
      assert(not pcall(CP.registerVehicleDataTypes,invalid))
    end
    local accepted=CP.setVehicleData({[3]=70,[99]=1})
    assert(next(accepted)==nil)
    assert(CP.getVehicleData({3})[3]==0)
  )",
             "vehicle-initial");
  b.evaluate("CP.registerVehicleDataTypes({3});assert(CP.getVehicleDataUpdates()[3]==0)");
  auto phone = v::encode_outgoing({"registerRequest", 1, arr({num(3)}), {}, {}}, 0);
  CHECK(source->handle(phone, now));
  CHECK(source->subscription_count() == 1);
  ++now;
  source->publish(3, Value{12.5}, now, now);
  a.evaluate(
      "assert(CP.getVehicleDataUpdates()[3]==12.5);assert(next(CP.getVehicleDataUpdates())==nil)");
  b.evaluate("assert(CP.getVehicleDataUpdates()[3]==12.5)");
  const auto update = source->updates(now);
  CHECK(update);
  CHECK(v::decode_incoming(*update).command.values == dict({{num(3), Value{12.5}}}));
  now = 1100;
  a.evaluate(R"(
    local data=CP.getVehicleDataUpdates()
    assert(data[3]==nil and data[4]==CP.INVALIDVALUE)
    assert(next(CP.getVehicleDataUpdates())==nil)
  )",
             "vehicle-partial-expiry");
  now = 1101;
  a.evaluate(R"(
    local data=CP.getVehicleDataUpdates()
    assert(data[3]==CP.INVALIDVALUE and data[4]==nil)
    local config=CP.getVehicleDataConfig()
    assert(config.fixture.measurement.reading.initialValue==CP.INVALIDVALUE)
    assert(config.fixture.measurement.counter.initialValue==nil)
  )",
             "vehicle-expiry");
  b.evaluate("assert(CP.getVehicleDataUpdates()[3]==CP.INVALIDVALUE)");
  ++now;
  source->publish(3, Value{1.5}, now, now);
  a.evaluate(
      "assert(CP.getVehicleDataUpdates()[3]==1.5);CP.registerVehicleDataTypes({});assert(next(CP."
      "getVehicleDataUpdates())==nil)");
  b.evaluate("assert(CP.getVehicleDataUpdates()[3]==1.5)");
  rejects([&] { a.bind_vehicle(source, bindings()); });
  ++now;
  source->invalidate(3, now, now);
  b.evaluate("assert(CP.getVehicleDataUpdates()[3]==CP.INVALIDVALUE)");
}
void content_dismissal(const std::filesystem::path& directory) {
  script::TrustedDirectory asset;
  asset.content_path = directory;
  auto source = model();
  auto mapping = bindings();
  mapping[2].path = {"automakerRequestContent", "requestContent", "on"};
  std::uint64_t now = 1000;
  unsigned dismissals = 0;
  source->publish(5, Value{true}, now, now);
  v::ScriptView::Dismissals callbacks{{5, [&](std::uint64_t date) {
                                         CHECK(date == now);
                                         ++dismissals;
                                         source->deactivate_boolean(5, date);
                                       }}};
  rejects([&] { (void)v::ScriptView(source, bindings(), callbacks); });
  script::Runtime runtime(asset, {"cluster-dismissal", 10, 10});
  runtime.bind_vehicle(source, mapping, [&] { return now; }, callbacks);
  runtime.evaluate(R"(
    assert(CP.getVehicleData({5})[5] == true)
    for _, value in ipairs({true, 0, 'false', {}, CP.INVALIDVALUE}) do
      assert(next(CP.setVehicleData({[5]=value})) == nil)
      assert(CP.getVehicleData({5})[5] == true)
    end
    local accepted = CP.setVehicleData({[5]=false, [3]=false, [99]=false})
    assert(accepted[5] == false and accepted[3] == nil and accepted[99] == nil)
    assert(CP.getVehicleData({5})[5] == false)
  )",
                   "content-dismissal");
  CHECK(dismissals == 1);
}
int main() {
  char pattern[] = "/tmp/headunit-vehicle-script-XXXXXX";
  const auto* dir = ::mkdtemp(pattern);
  if (!dir) return 1;
  try {
    mapping_validation();
    script_bridge(dir);
    repeated_services(dir);
    content_dismissal(dir);
    std::filesystem::remove_all(dir);
    std::cout << "Vehicle script mapping, repeated services, typed values, independent "
                 "subscriptions, expiry, invalidation and read-only writes passed\n";
  } catch (const std::exception& e) {
    std::filesystem::remove_all(dir);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
