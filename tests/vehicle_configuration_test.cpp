// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_configuration.hpp>
#include <headunit/theme/vehicle_transport.hpp>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace headunit;
namespace v = headunit::theme::vehicle;
namespace op = headunit::theme::opack;
using op::Value;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
void rejects(auto f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid configuration accepted");
}
const auto source = std::filesystem::path(HEADUNIT_SOURCE_DIR);
v::Configuration load(std::string climate = "climate.example.json",
                      std::string settings = "car-settings.example.json") {
  return v::load_configuration(source / "config" / climate, source / "config" / settings);
}
std::int64_t iid(const v::Configuration& c, std::string_view key, bool writable = true) {
  for (const auto& s : c.signals)
    if (s.key == key && s.writable == writable) return s.policy.iid;
  throw std::runtime_error("missing binding");
}
v::Command write(v::ConfiguredModel& model, std::int64_t id, Value value, std::uint64_t now) {
  auto response = model.service().handle(
      v::encode_outgoing(
          {"writeRequest", 1, Value{op::Dictionary{{Value{id}, std::move(value)}}}, {}, {}}, 0),
      now);
  CHECK(response);
  return v::decode_incoming(*response).command;
}
void snapshot(v::ConfiguredModel& model, std::int64_t id, Value expected, std::uint64_t now) {
  const std::array<std::int64_t, 1> ids{id};
  CHECK(model.service().snapshot(ids, now).at(id).value == expected);
}
void contains_type(const Value& profile, std::int64_t type, bool expected) {
  bool found = false;
  const auto visit = [&](const auto& self, const Value& value) -> void {
    if (const auto* d = std::get_if<op::Dictionary>(&value.data))
      for (const auto& [k, v] : *d) {
        if (k == Value{std::string("type")} && v == Value{type}) found = true;
        self(self, v);
      }
    else if (const auto* a = std::get_if<op::Array>(&value.data))
      for (const auto& v : *a)
        self(self, v);
  };
  visit(visit, profile);
  CHECK(found == expected);
}
const Value& field(const Value& object, std::string_view key) {
  const auto& entries = std::get<op::Dictionary>(object.data);
  for (const auto& [name, value] : entries)
    if (name == Value{std::string(key)}) return value;
  throw std::runtime_error("missing profile field");
}
const Value& typed(const Value& entries, std::int64_t type) {
  for (const auto& value : std::get<op::Array>(entries.data))
    if (field(value, "type") == Value{type}) return value;
  throw std::runtime_error("missing profile type");
}
struct TestProvider final : v::Provider {
  std::map<std::string, v::Sample> samples;
  bool fail_write = false;
  Value applied{22.0};
  unsigned reads = 0, writes = 0;
  std::optional<v::Sample> read(std::string_view key, std::uint64_t) override {
    ++reads;
    const auto i = samples.find(std::string(key));
    return i == samples.end() ? std::nullopt : std::optional<v::Sample>{i->second};
  }
  Value apply(std::string_view, const Value&, std::uint64_t) override {
    ++writes;
    if (fail_write) throw std::runtime_error("hardware absent");
    return applied;
  }
};
struct AliasProvider final : v::Provider {
  unsigned calls = 0;
  bool absent = false, fail = false;
  std::optional<v::Sample> read(std::string_view, std::uint64_t now) override {
    ++calls;
    if (fail) throw std::runtime_error("provider disconnected");
    if (absent) return {};
    return v::Sample{Value{21.0 + 0.5 * (calls - 1)}, now};
  }
  Value apply(std::string_view, const Value& value, std::uint64_t) override { return value; }
};
void synchronous_confirmation_lifetime() {
  // Controlled synchronous provider, not a physical HVAC executor. Exercise
  // the production configuration, OPACK endpoint and subscription owner together.
  const auto config = v::parse_configuration(R"({"version":1,"zones":[{
    "id":"driver","layout":"seat_front_left","temperature":{}}]})",
                                             "");
  const std::string key = "climate.driver.temperature.target";
  const auto target = iid(config, key), current = iid(config, key, false);
  const std::array ids{target, current};
  const auto both = [&](const Value& value) {
    op::Dictionary values;
    for (const auto id : std::set{target, current})
      values.emplace_back(Value{id}, value);
    return Value{std::move(values)};
  };
  const auto unavailable = [&](v::ConfiguredModel& model, std::uint64_t now) {
    for (const auto& [id, reading] : model.service().snapshot(ids, now)) {
      (void)id;
      CHECK(!reading.value && reading.error == -6727);
    }
  };
  const auto update = [](v::ConfiguredModel& model, std::uint64_t now) {
    const auto bytes = model.service().updates(now);
    CHECK(bytes);
    auto command = v::decode_incoming(*bytes).command;
    CHECK(command.name == "updateNotify" && !command.transaction && !command.error);
    return command;
  };
  auto provider = std::make_shared<TestProvider>();
  provider->samples[key] = {Value{21.0}, 1000};
  {
    v::ConfiguredModel model(config, provider);
    model.refresh(1000);
    CHECK(provider->reads == 1);
    const auto bytes = model.service().handle(
        v::encode_outgoing(
            {"registerRequest", 7, Value{op::Array{Value{target}, Value{current}}}, {}, {}}, 0),
        1000);
    CHECK(bytes);
    const auto registered = v::decode_incoming(*bytes).command;
    CHECK(registered.name == "registerResponse" && registered.transaction == 7);
    CHECK(registered.values == both(Value{21.0}) && !registered.errors && !registered.error);
    CHECK(!model.service().updates(1000));
    for (const auto& invalid : {Value{22.25}, Value{31.0}, Value{std::int64_t(22)}, Value{false}})
      CHECK(write(model, target, invalid, 1001).errors);
    CHECK(write(model, current, Value{22.0}, 1001).errors);
    CHECK(provider->writes == 0);
    unsigned attempts = 0;
    for (const auto& invalid :
         {Value{22.25}, Value{31.0}, Value{std::int64_t(22)}, Value{false}, Value{}}) {
      provider->applied = invalid;
      const auto rejected = write(model, target, Value{24.0}, 1002);
      CHECK(rejected.name == "writeResponse" && rejected.transaction == 1);
      CHECK(!rejected.error && !rejected.values);
      CHECK(
          (rejected.errors == Value{op::Dictionary{{Value{target}, Value{std::int64_t(-6705)}}}}));
      CHECK(provider->writes == ++attempts);
      snapshot(model, target, Value{21.0}, 1002);
      snapshot(model, current, Value{21.0}, 1002);
      CHECK(!model.service().updates(1002));
    }
    // Failed confirmations cannot poison the original acquisition frontier.
    model.refresh(1100);
    CHECK(provider->reads == 2);
    snapshot(model, target, Value{21.0}, 1100);
    snapshot(model, current, Value{21.0}, 1100);
    CHECK(!model.service().updates(1100));
    provider->fail_write = true;
    const auto rejected = write(model, target, Value{24.0}, 1101);
    CHECK(rejected.errors && !rejected.values && provider->writes == ++attempts);
    CHECK(!model.service().updates(1101));
    provider->fail_write = false;
    provider->applied = Value{22.0};
    const auto applied = write(model, target, Value{24.0}, 1102);
    CHECK(!applied.error && !applied.errors);
    CHECK((applied.values == Value{op::Dictionary{{Value{target}, Value{22.0}}}}));
    CHECK(provider->writes == ++attempts);
    snapshot(model, current, Value{21.0}, 1102);
    const auto target_update = update(model, 1102);
    CHECK(target_update.values == applied.values && !target_update.errors);
    // Preserve existing notification timing: the read-only alias changes only
    // when its provider observation arrives on the regular sampling turn.
    provider->samples[key] = {Value{22.0}, 1150};
    model.refresh(1200);
    CHECK(provider->reads == 3);
    const auto alias_update = update(model, 1200);
    CHECK((alias_update.values == Value{op::Dictionary{{Value{current}, Value{22.0}}}}));
    CHECK(!alias_update.errors && !model.service().updates(1200));
    provider->samples.clear();
    model.refresh(1300);
    CHECK(provider->reads == 4);
    unavailable(model, 1300);
    const auto lost = update(model, 1300);
    CHECK(!lost.values && lost.errors == both(Value{std::int64_t(-6727)}));
    provider->fail_write = true;
    const auto absent = write(model, target, Value{24.0}, 1301);
    CHECK(absent.errors && !absent.values && provider->writes == ++attempts);
    unavailable(model, 1301);
    CHECK(!model.service().updates(1301));
    provider->fail_write = false;
    provider->samples[key] = {Value{22.0}, 1150};
    model.refresh(1400);
    unavailable(model, 1400);
    CHECK(!model.service().updates(1400));
    provider->samples[key] = {Value{23.0}, 1450};
    model.refresh(1500);
    const auto recovered = update(model, 1500);
    CHECK(recovered.values == both(Value{23.0}) && !recovered.errors);
    model.disconnect();
    unavailable(model, 1500);
    CHECK(model.service().subscription_count() == 0 && !model.service().updates(1500));
    // Reopening the same model retains provider state, never old subscriptions.
    provider->samples[key] = {Value{24.0}, 1550};
    model.refresh(1600);
    snapshot(model, target, Value{24.0}, 1600);
    snapshot(model, current, Value{24.0}, 1600);
    CHECK(!model.service().updates(1600));
  }
  // A destroyed synchronous model has no pending confirmation. Retaining and
  // changing its old provider cannot populate a different successor binding.
  provider->samples[key] = {Value{25.0}, 1700};
  auto replacement = std::make_shared<TestProvider>();
  v::ConfiguredModel successor(config, replacement);
  successor.refresh(1700);
  unavailable(successor, 1700);
  CHECK(successor.service().subscription_count() == 0 && !successor.service().updates(1700));
  replacement->samples[key] = {Value{18.0}, 1750};
  successor.refresh(1800);
  snapshot(successor, target, Value{18.0}, 1800);
  snapshot(successor, current, Value{18.0}, 1800);
  CHECK(!successor.service().updates(1800));
  CHECK(provider->reads == 7 && replacement->reads == 2);
}
void resource_variants() {
  const auto config = v::load_configuration(
      std::nullopt, source / "config/car-settings.vehicle-resources.example.json");
  CHECK(config.experimental);
  const auto& accessory = typed(field(config.profile, "accessories"), 0x01200002);
  const auto& service = typed(field(accessory, "services"), 0x20000001);
  CHECK(std::get<op::Array>(field(service, "controls").data).empty());
  const auto& chars = field(service, "characteristics");
  CHECK(std::get<op::Array>(chars.data).size() == 3);
  auto provider = std::make_shared<TestProvider>();
  v::ConfiguredModel model(config, provider);
  model.refresh(1000);
  for (const auto type : {0x48000003, 0x48000004, 0x48000005}) {
    const auto& characteristic = typed(chars, type);
    const auto id = std::get<std::int64_t>(field(characteristic, "iid").data);
    CHECK(field(characteristic, "mutable") == Value{false});
    CHECK(field(characteristic, "writable") == Value{false});
    CHECK(field(characteristic, "format") == Value{std::int64_t(10)});
    snapshot(model, id, Value{std::string("DBX")}, 1001);
    const auto rejected = write(model, id, Value{std::string("Other")}, 1001);
    CHECK(rejected.error == -6710 && !rejected.values);
    snapshot(model, id, Value{std::string("DBX")}, 1001);
  }
  model.disconnect();
  model.refresh(100000);
  const auto response =
      model.service().handle(v::encode_outgoing({"configRequest", 2, {}, {}, {}}, 0), 100000);
  CHECK(response);
  const auto reply = v::decode_incoming(*response).command;
  CHECK(reply.name == "configResponse" && reply.values && !reply.error);
  const auto& advertised = typed(field(*reply.values, "accessories"), 0x01200002);
  const auto& restored = typed(field(advertised, "services"), 0x20000001);
  for (const auto& c : std::get<op::Array>(field(restored, "characteristics").data))
    CHECK(field(c, "initialValue") == Value{std::string("DBX")});
  // Resource metadata does not turn missing tyre observations into readings.
  for (const auto& s : config.signals) {
    CHECK(std::holds_alternative<std::monostate>(s.initial.data));
    const std::array<std::int64_t, 1> ids{s.policy.iid};
    const auto missing = model.service().snapshot(ids, 100000).at(s.policy.iid);
    CHECK(missing.value == Value{} && !missing.error);
  }
  const auto parse = [](std::string_view resources, bool experimental = true) {
    return v::parse_configuration(
        "", std::string(R"({"version":1,"sections":[],"settings":[],"experimental":)") +
                (experimental ? "true" : "false") +
                ",\"vehicle_resources\":" + std::string(resources) + "}");
  };
  const auto valid = R"({"exterior_style":"DBX","interior_style":"DBX","vehicle_layout":"DBX"})";
  rejects([&] { parse(valid, false); });
  for (
      const auto* invalid :
      {R"({})", R"(null)", R"({"exterior_style":"DBX","interior_style":"DBX"})",
       R"({"exterior_style":"DBX","interior_style":"DBX","vehicle_layout":"../DBX"})",
       R"({"exterior_style":"DBX","interior_style":"DBX","vehicle_layout":1})",
       R"({"exterior_style":"DBX","interior_style":"DBX","vehicle_layout":"DBX","image_archive":"guess.zip"})"})
    rejects([&] { parse(invalid); });
}
void media_presence() {
  const auto parse = [](std::string_view media, bool experimental = true) {
    return v::parse_configuration(
        "", std::string(R"({"version":1,"sections":[],"settings":[],"experimental":)") +
                (experimental ? "true" : "false") + ",\"media\":" + std::string(media) + "}");
  };
  const auto valid = R"({"source_id":"iphone","source_label":"iPhone"})";
  const auto config = parse(valid);
  CHECK(config.experimental && config.signals.empty());
  const auto& media = typed(field(config.profile, "accessories"), 0x03000001);
  const auto& source = typed(field(media, "services"), 0x14000006);
  const auto& information = typed(field(config.profile, "accessories"), 0x0F000001);
  const auto& now_playing = typed(field(information, "services"), 0x14000001);
  const auto& current = std::get<op::Array>(field(now_playing, "characteristics").data);
  CHECK(current.size() == 1 && field(current.front(), "type") == Value{std::int64_t{0x32000023}});
  CHECK(std::get<op::Array>(field(source, "characteristics").data).size() == 2);
  // No phone metadata, playback state or guessed enum is declared.
  contains_type(config.profile, 0x32000003, false);
  contains_type(config.profile, 0x32000034, false);
  contains_type(config.profile, 0x32000025, false);
  rejects([&] { parse(valid, false); });
  for (const auto* invalid : {R"({})", R"(null)", R"({"source_id":"iphone"})",
                              R"({"source_id":"../iphone","source_label":"iPhone"})",
                              R"({"source_id":"iphone","source_label":""})",
                              R"({"source_id":"iphone","source_label":"iPhone","title":"Song"})"})
    rejects([&] { parse(invalid); });
}
int main() try {
  synchronous_confirmation_lifetime();
  resource_variants();
  media_presence();
  const auto minimal = load();
  contains_type(minimal.profile, 0x01200002, false);
  CHECK(!minimal.experimental && minimal.signals.size() == 7);
  contains_type(minimal.profile, 0x01000001, true);
  contains_type(minimal.profile, 0x01500001, false);
  contains_type(minimal.profile, 0x22000001, false);
  contains_type(minimal.profile, 0x22000002, false);
  auto provider = v::simulated_provider(minimal);
  v::ConfiguredModel model(minimal, provider);
  model.refresh(1000);
  const auto target = iid(minimal, "climate.driver.temperature.target"),
             current = iid(minimal, "climate.driver.temperature.target", false);
  CHECK(!write(model, target, Value{22.5}, 1001).errors);
  model.refresh(1100);
  snapshot(model, current, Value{22.5}, 1100);
  for (const auto& invalid : {Value{22.25}, Value{31.0}, Value{std::int64_t(22)}, Value{false}})
    CHECK(write(model, target, invalid, 1101).errors);
  snapshot(model, target, Value{22.5}, 1101);
  CHECK(write(model, current, Value{23.0}, 1102).errors);
  model.disconnect();
  model.refresh(1200);
  snapshot(model, target, Value{22.5}, 1200);
  v::ConfiguredModel reconnect(minimal, provider);
  reconnect.refresh(1200);
  snapshot(reconnect, target, Value{22.5}, 1200);
  // Fresh /info must use retained provider state, not startup defaults.
  const auto retained_info = v::add_server_info(
      encode_binary_plist(PlistValue{PlistValue::Dictionary{}}), reconnect.service(), 1200);
  const auto retained = decode_binary_plist(retained_info);
  const auto& root = std::get<PlistValue::Dictionary>(retained.value);
  const auto& protocol =
      std::get<PlistValue::Dictionary>(root.at("vehicleStateProtocolInfo").value);
  const auto& plugin = std::get<PlistValue::Dictionary>(
      std::get<PlistValue::Array>(protocol.at("pluginConfigs").value).front().value);
  bool retained_value = false;
  for (const auto& a : std::get<PlistValue::Array>(plugin.at("accessories").value))
    for (const auto& s : std::get<PlistValue::Array>(
             std::get<PlistValue::Dictionary>(a.value).at("services").value))
      for (const auto& c : std::get<PlistValue::Array>(
               std::get<PlistValue::Dictionary>(s.value).at("characteristics").value)) {
        const auto& fields = std::get<PlistValue::Dictionary>(c.value);
        if (std::get<std::int64_t>(fields.at("iid").value) == target)
          retained_value = std::get<double>(fields.at("initialValue").value) == 22.5;
      }
  CHECK(retained_value);
  const auto full = load("climate.full.example.json", "car-settings.full.example.json");
  CHECK(full.experimental);
  contains_type(full.profile, 0x01500001, true);
  contains_type(full.profile, 0x22000001, true);
  contains_type(full.profile, 0x22000002, true);
  contains_type(full.profile, 0x11000006, true);
  v::ConfiguredModel expanded(full, v::simulated_provider(full));
  expanded.refresh(2000);
  const auto seats = iid(full, "climate.driver.seat_heating_cooling.level");
  CHECK(!write(expanded, seats, Value{std::int64_t(-3)}, 2001).errors);
  CHECK(write(expanded, seats, Value{std::int64_t(-4)}, 2002).errors);
  const auto ventilation = iid(full, "climate.driver.seat_fan.level");
  CHECK(!write(expanded, ventilation, Value{std::int64_t(3)}, 2003).errors);
  CHECK(write(expanded, ventilation, Value{std::int64_t(4)}, 2004).errors);
  const auto selection = iid(full, "settings.comfort.theme");
  CHECK(!write(expanded, selection, Value{std::int64_t(1)}, 2005).errors);
  CHECK(write(expanded, selection, Value{std::int64_t(2)}, 2006).errors);
  const auto multi = iid(full, "settings.comfort.notifications");
  CHECK(!write(expanded, multi, Value{op::Array{Value{std::int64_t(0)}, Value{std::int64_t(1)}}},
               2007)
             .errors);
  CHECK(
      write(expanded, multi, Value{op::Array{Value{std::int64_t(0)}, Value{std::int64_t(0)}}}, 2008)
          .errors);
  CHECK(write(expanded, multi, Value{op::Array{Value{std::int64_t(2)}}}, 2009).errors);
  CHECK(write(expanded, multi, Value{std::int64_t(1)}, 2010).errors);
  // The simulated applied level and power state describe the same actuator.
  const auto seat_on = iid(full, "climate.driver.seat_heating_cooling.on");
  expanded.refresh(2100);
  snapshot(expanded, seat_on, Value{true}, 2100);
  CHECK(!write(expanded, seat_on, Value{false}, 2101).errors);
  expanded.refresh(2200);
  snapshot(expanded, seats, Value{std::int64_t(0)}, 2200);
  CHECK(!write(expanded, seat_on, Value{true}, 2201).errors);
  expanded.refresh(2300);
  snapshot(expanded, seats, Value{std::int64_t(-3)}, 2300);
  for (const auto& config : {minimal, full}) {
    v::ConfiguredModel m(config, v::simulated_provider(config));
    m.refresh(0);
    const auto info = encode_binary_plist(PlistValue{PlistValue::Dictionary{}});
    CHECK(!v::add_server_info(info, m.service(), 0).empty());
  }
  // Reject typos, duplicate escaped keys, oversized/unbounded input and invalid capabilities.
  for (const auto& text :
       {"{\"version\":1,\"vers\\u0069on\":1}", "{\"version\":2}",
        "{\"version\":1,\"heated_seat\":true}", "{\"version\":true}", "{\"version\":1,}",
        "{\"version\":01}", "[]", "{\"version\":1,\"zones\":null}", "{\"version\":1,\"\\u0000\":1}",
        "{\"version\":1,\"\\ud800\":1}", "{\"version\":1,\"\\udfff\":1}",
        "{\"version\":1,\"zones\":[{\"id\":\"driver\",\"layout\":\"seat_front_left\"}]}",
        "{\"version\":1,\"zones\":[{\"id\":\"driver\",\"layout\":\"seat_front_left\",\"seat_"
        "heating_cooling\":{}}]}",
        "{\"version\":1,\"zones\":[{\"id\":\"driver\",\"layout\":\"seat_front_left\",\"fan\":{"
        "\"initial\":2.5}}]}",
        "{\"version\":1,\"zones\":[{\"id\":\"driver\",\"layout\":\"seat_front_left\","
        "\"temperature\":{\"step\":0}}]}"})
    rejects([&] { v::parse_configuration(text, ""); });
  rejects([] { v::parse_configuration(std::string(65537, ' '), ""); });
  rejects(
      [] { v::parse_configuration(std::string("{\"version\":1,\"") + char(0xff) + "\":1}", ""); });
  rejects([] {
    v::parse_configuration(
        "{\"version\":1,\"zones\":[{\"id\":\"x\",\"layout\":\"seat_front_left\",\"temperature\":{"
        "\"initial\":1e400}}]}",
        "");
  });
  rejects([] {
    v::parse_configuration("",
                           "{\"version\":1,\"sections\":[],\"settings\":[{\"id\":\"a\",\"section\":"
                           "\"absent\",\"label\":\"A\",\"kind\":\"boolean\"}]}");
  });
  const auto empty = v::parse_configuration("{\"version\":1}", "");
  CHECK(empty.signals.empty());
  rejects([&] { v::ConfiguredModel m(minimal, {}); });
  // A physical provider must confirm writes; stale, absent and invalid readings disappear.
  auto physical = std::make_shared<TestProvider>();
  physical->samples["climate.driver.temperature.target"] = {Value{21.0}, 3000};
  v::ConfiguredModel hardware(minimal, physical);
  hardware.refresh(3000);
  snapshot(hardware, current, Value{21.0}, 3000);
  CHECK(!write(hardware, target, Value{23.0}, 3001).errors);
  snapshot(hardware, target, Value{22.0}, 3001);
  physical->fail_write = true;
  CHECK(write(hardware, target, Value{24.0}, 3002).errors);
  hardware.refresh(4100);
  const std::array<std::int64_t, 1> ids{current};
  CHECK(!hardware.service().snapshot(ids, 4100).at(current).value);
  physical->samples["climate.driver.temperature.target"] = {Value{25.0}, 4200};
  hardware.refresh(4200);
  snapshot(hardware, current, Value{25.0}, 4200);
  physical->samples["climate.driver.temperature.target"] = {Value{999.0}, 4300};
  hardware.refresh(4300);
  CHECK(!hardware.service().snapshot(ids, 4300).at(current).value);
  // Setpoint increments must not constrain an independent physical reading.
  // Existing profiles keep the setpoint alias; hardware opts into its own
  // read-only binding/range/precision without adding a new wire characteristic.
  const auto measured = v::parse_configuration(R"({"version":1,"zones":[{
    "id":"driver","layout":"seat_front_left","temperature":{
      "initial":21,"current":{"binding":"hvac.driver.temperature",
        "minimum":-40,"maximum":85,"step":0.1,"initial":5.3}}}]})",
                                               "");
  const auto measured_target = iid(measured, "climate.driver.temperature.target");
  const auto measured_current = iid(measured, "hvac.driver.temperature", false);
  auto cabin = std::make_shared<TestProvider>();
  cabin->samples["climate.driver.temperature.target"] = {Value{21.0}, 5000};
  cabin->samples["hvac.driver.temperature"] = {Value{5.3}, 5000};
  v::ConfiguredModel cabin_model(measured, cabin);
  cabin_model.refresh(5000);
  snapshot(cabin_model, measured_current, Value{5.3}, 5000);
  CHECK(write(cabin_model, measured_current, Value{22.0}, 5001).errors);
  CHECK(write(cabin_model, measured_target, Value{21.1}, 5002).errors);
  cabin->samples["hvac.driver.temperature"] = {Value{-12.7}, 5100};
  cabin_model.refresh(5100);
  snapshot(cabin_model, measured_current, Value{-12.7}, 5100);
  cabin->samples["hvac.driver.temperature"] = {Value{22.35}, 5200};
  cabin_model.refresh(5200);
  const std::array<std::int64_t, 1> measured_ids{measured_current};
  CHECK(!cabin_model.service().snapshot(measured_ids, 5200).at(measured_current).value);
  cabin->samples["hvac.driver.temperature"] = {Value{86.0}, 5300};
  cabin_model.refresh(5300);
  CHECK(!cabin_model.service().snapshot(measured_ids, 5300).at(measured_current).value);
  rejects([] {
    v::parse_configuration(R"({"version":1,"zones":[{
    "id":"driver","layout":"seat_front_left","temperature":{
      "current":{"minimum":-40,"maximum":85,"step":0.1}}}]})",
                           "");
  });
  rejects([] {
    v::parse_configuration(R"({"version":1,"zones":[{
    "id":"driver","layout":"seat_front_left","temperature":{
      "current":{"binding":"sensor.temperature","writable":true}}}]})",
                           "");
  });
  // A provider's absence/invalid read has no sensor timestamp. It must not
  // advance the observation frontier to poll time and block delayed recovery.
  auto recovering = std::make_shared<TestProvider>();
  recovering->samples["climate.driver.temperature.target"] = {Value{21.0}, 5000};
  recovering->samples["hvac.driver.temperature"] = {Value{5.3}, 5000};
  v::ConfiguredModel recovery(measured, recovering);
  recovery.refresh(5100);
  snapshot(recovery, measured_current, Value{5.3}, 5100);
  recovering->samples.erase("hvac.driver.temperature");
  recovery.refresh(5200);
  CHECK(!recovery.service().snapshot(measured_ids, 5200).at(measured_current).value);
  recovering->samples["hvac.driver.temperature"] = {Value{5.4}, 5100};
  recovery.refresh(5300);
  snapshot(recovery, measured_current, Value{5.4}, 5300);
  recovering->samples["hvac.driver.temperature"] = {Value{999.0}, 5400};
  recovery.refresh(5400);
  CHECK(!recovery.service().snapshot(measured_ids, 5400).at(measured_current).value);
  recovering->samples["hvac.driver.temperature"] = {Value{5.4}, 5100};
  recovery.refresh(5500);
  CHECK(!recovery.service().snapshot(measured_ids, 5500).at(measured_current).value);
  recovering->samples["hvac.driver.temperature"] = {Value{5.5}, 5400};
  recovery.refresh(5600);
  snapshot(recovery, measured_current, Value{5.5}, 5600);
  recovering->samples["hvac.driver.temperature"] = {Value{5.6}, 5701};
  recovery.refresh(5700);
  CHECK(!recovery.service().snapshot(measured_ids, 5700).at(measured_current).value);
  recovering->samples["hvac.driver.temperature"] = {Value{5.6}, 5600};
  recovery.refresh(5800);
  snapshot(recovery, measured_current, Value{5.6}, 5800);
  // Conflicting equal-date observations withdraw the old displayed value.
  // Returning the original acquisition cannot revive it; recovery needs a
  // genuinely newer reading, even while the original would still be fresh.
  recovering->samples["hvac.driver.temperature"] = {Value{6.0}, 5600};
  recovery.refresh(5900);
  CHECK(!recovery.service().snapshot(measured_ids, 5900).at(measured_current).value);
  recovering->samples["hvac.driver.temperature"] = {Value{5.6}, 5600};
  recovery.refresh(6000);
  CHECK(!recovery.service().snapshot(measured_ids, 6000).at(measured_current).value);
  recovering->samples["hvac.driver.temperature"] = {Value{5.7}, 5900};
  recovery.refresh(6100);
  snapshot(recovery, measured_current, Value{5.7}, 6100);
  recovery.refresh(6200);
  snapshot(recovery, measured_current, Value{5.7}, 6200);
  // One sampling turn produces one coherent observation for aliases, even if
  // the provider changes on every read. Absence/failure are shared as well.
  const auto aliases = v::parse_configuration(R"({"version":1,"zones":[{
    "id":"driver","layout":"seat_front_left","temperature":{}}]})",
                                              "");
  auto alias_provider = std::make_shared<AliasProvider>();
  v::ConfiguredModel alias_model(aliases, alias_provider);
  alias_model.refresh(6000);
  CHECK(alias_provider->calls == 1);
  const auto alias_target = iid(aliases, "climate.driver.temperature.target");
  const auto alias_current = iid(aliases, "climate.driver.temperature.target", false);
  snapshot(alias_model, alias_target, Value{21.0}, 6000);
  snapshot(alias_model, alias_current, Value{21.0}, 6000);
  alias_model.refresh(6100);
  CHECK(alias_provider->calls == 2);
  snapshot(alias_model, alias_target, Value{21.5}, 6100);
  snapshot(alias_model, alias_current, Value{21.5}, 6100);
  const std::array<std::int64_t, 2> alias_ids{alias_target, alias_current};
  alias_provider->absent = true;
  alias_model.refresh(6200);
  CHECK(alias_provider->calls == 3);
  for (const auto& [id, reading] : alias_model.service().snapshot(alias_ids, 6200)) {
    (void)id;
    CHECK(!reading.value);
  }
  alias_provider->absent = false;
  alias_provider->fail = true;
  alias_model.refresh(6300);
  CHECK(alias_provider->calls == 4);
  for (const auto& [id, reading] : alias_model.service().snapshot(alias_ids, 6300)) {
    (void)id;
    CHECK(!reading.value);
  }
  alias_provider->fail = false;
  alias_model.refresh(6400);
  CHECK(alias_provider->calls == 5);
  snapshot(alias_model, alias_target, Value{23.0}, 6400);
  snapshot(alias_model, alias_current, Value{23.0}, 6400);
  // The next Vent presentation test declares only its required controller,
  // keeping optional power/auto and unrelated advanced controls out of the
  // comparison. Offline retention is not phone layout qualification.
  const auto vent_fixture =
      v::load_configuration(source / "config/climate.vent-isolation.example.json", std::nullopt);
  contains_type(vent_fixture.profile, 0x11000006, true);
  contains_type(vent_fixture.profile, 0x01500001, false);
  contains_type(vent_fixture.profile, 0x11000003, false);
  for (const auto& signal : vent_fixture.signals)
    CHECK(signal.key != "climate.driver.vents.on" && signal.key != "climate.driver.vents.auto");
  const auto vent = iid(vent_fixture, "climate.driver.vents.index");
  v::ConfiguredModel vent_model(vent_fixture, v::simulated_provider(vent_fixture));
  vent_model.refresh(6500);
  for (std::int64_t index = 0; index < 4; ++index) {
    const auto now = 6600 + static_cast<std::uint64_t>(index) * 100;
    CHECK(!write(vent_model, vent, Value{index}, now).errors);
    snapshot(vent_model, vent, Value{index}, now);
  }
  CHECK(write(vent_model, vent, Value{std::int64_t(4)}, 7000).errors);
  CHECK(write(vent_model, vent, Value{std::int64_t(-1)}, 7001).errors);
  CHECK(write(vent_model, vent, Value{1.0}, 7002).errors);
  // Exterior camera is a separate opt-in capability. Its two phone-facing On
  // values alias one read-only, initially false provider observation.
  const auto camera = v::load_configuration(std::nullopt, std::nullopt,
                                            source / "config/camera-state.example.json");
  CHECK(camera.experimental && camera.camera && camera.signals.size() == 2);
  CHECK(camera.camera->stream_identifier == "camera" && camera.camera->binding == "camera.visible");
  CHECK(camera.camera->max_age_ms == 151);
  const auto input_on = camera.camera->input_stream_on_iid;
  const auto general_on = camera.camera->camera_general_on_iid;
  CHECK(input_on != general_on);
  for (const auto& signal : camera.signals) {
    CHECK(signal.key == camera.camera->binding && !signal.writable);
    CHECK(signal.policy.kind == v::ValueKind::boolean && signal.policy.max_age_ms == 151);
    CHECK(signal.initial == Value{false});
  }
  const auto& accessories = field(camera.profile, "accessories");
  CHECK(std::get<op::Array>(accessories.data).size() == 2);
  const auto& stream = typed(accessories, 0x0C000001);
  const auto& critical = typed(field(stream, "services"), 0x1C000001);
  const auto& critical_chars = field(critical, "characteristics");
  CHECK(std::get<op::Array>(critical_chars.data).size() == 2);
  const auto& uid = typed(critical_chars, 0x44000000);
  CHECK(field(uid, "mutable") == Value{false});
  CHECK(camera.fixed.at(std::get<std::int64_t>(field(uid, "iid").data)) ==
        Value{std::string("camera")});
  CHECK(field(typed(critical_chars, 0x30000002), "iid") == Value{input_on});
  const auto& exterior = typed(accessories, 0x01300001);
  const auto& general = typed(field(exterior, "services"), 0x1F000001);
  const auto& general_chars = field(general, "characteristics");
  CHECK(std::get<op::Array>(general_chars.data).size() == 1);
  CHECK(field(typed(general_chars, 0x30000002), "iid") == Value{general_on});
  contains_type(camera.profile, 0x13000001, false);
  contains_type(camera.profile, 0x1F000002, false);
  contains_type(camera.profile, 0x36000063, false);
  std::set<std::int64_t> topology_ids;
  const auto check_ids = [&](const auto& self, const Value& value) -> void {
    if (const auto* entries = std::get_if<op::Dictionary>(&value.data)) {
      for (const auto& [key, member] : *entries) {
        if (key == Value{std::string("iid")})
          CHECK(topology_ids.insert(std::get<std::int64_t>(member.data)).second);
        self(self, member);
      }
    } else if (const auto* entries = std::get_if<op::Array>(&value.data))
      for (const auto& member : *entries)
        self(self, member);
  };
  check_ids(check_ids, camera.profile);
  auto camera_provider = std::make_shared<TestProvider>();
  camera_provider->samples["camera.visible"] = {Value{false}, 8000};
  v::ConfiguredModel camera_model(camera, camera_provider);
  camera_model.refresh(8000);
  snapshot(camera_model, input_on, Value{false}, 8000);
  snapshot(camera_model, general_on, Value{false}, 8000);
  const auto registration = camera_model.service().handle(
      v::encode_outgoing(
          {"registerRequest", 2, Value{op::Array{Value{input_on}, Value{general_on}}}, {}, {}}, 0),
      8000);
  CHECK(registration && camera_model.service().subscription_count() == 2);
  const auto registered = v::decode_incoming(*registration).command;
  CHECK(registered.name == "registerResponse" && registered.values);
  CHECK(!camera_model.service().updates(8000));
  {
    const auto response = write(camera_model, input_on, Value{true}, 8001);
    CHECK(response.errors || response.error);
  }
  {
    const auto response = write(camera_model, general_on, Value{true}, 8001);
    CHECK(response.errors || response.error);
  }
  camera_provider->samples["camera.visible"] = {Value{true}, 8100};
  camera_model.refresh(8100);
  snapshot(camera_model, input_on, Value{true}, 8100);
  snapshot(camera_model, general_on, Value{true}, 8100);
  const auto changed = camera_model.service().updates(8100);
  CHECK(changed);
  const auto notification = v::decode_incoming(*changed).command;
  CHECK(notification.name == "updateNotify" && notification.values);
  const auto& updates = std::get<op::Dictionary>(notification.values->data);
  CHECK(updates.size() == 2);
  for (const auto& [id, value] : updates) {
    CHECK((id == Value{input_on} || id == Value{general_on}) && value == Value{true});
  }
  camera_provider->samples["camera.visible"] = {Value{false}, 8200};
  camera_model.refresh(8200);
  snapshot(camera_model, input_on, Value{false}, 8200);
  snapshot(camera_model, general_on, Value{false}, 8200);
  CHECK(camera_model.service().updates(8200));
  constexpr auto valid_camera =
      R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":false}}})";
  for (
      const auto* invalid :
      {R"({"version":2,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":false}}})",
       R"({"version":1,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":false}}})",
       R"({"version":1,"experimental":false,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":false}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"bad space","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":false}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":true,"initial":false}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":true}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":99,"writable":false,"initial":false}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":1001,"writable":false,"initial":false}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","writable":false,"initial":false}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"max_age_ms":150,"writable":false,"initial":false}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":false,"selected":0}}})",
       R"({"version":1,"experimental":true,"stream_identifier":"camera","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":false}},"buttons":[]})"})
    rejects([&] { v::parse_configuration("", "", invalid); });
  const auto valid_parsed = v::parse_configuration("", "", valid_camera);
  CHECK(valid_parsed.camera && valid_parsed.camera->binding == "camera.visible");
  rejects([&] {
    v::parse_configuration(
        R"({"version":1,"zones":[{"id":"driver","layout":"seat_front_left","temperature":{"binding":"camera.visible"}}]})",
        "", valid_camera);
  });
  rejects([&] {
    v::parse_configuration(
        "",
        R"({"version":1,"sections":[{"id":"camera","label":"Camera"}],"settings":[{"id":"test","section":"camera","label":"Test","kind":"boolean","binding":"camera.visible","initial":false,"writable":false,"max_age_ms":150}]})",
        valid_camera);
  });
  rejects([&] {
    v::parse_configuration(
        "", "",
        std::string(R"({"version":1,"experimental":true,"stream_identifier":")") +
            std::string(65, 'c') +
            R"(","general":{"on":{"binding":"camera.visible","max_age_ms":150,"writable":false,"initial":false}}})");
  });
  std::cout << "Vehicle configuration: capabilities, zones, seat controls, settings, applied "
               "writes, bounds and provider loss passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
