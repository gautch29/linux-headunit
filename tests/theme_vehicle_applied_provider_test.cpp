// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_applied_provider.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace headunit;
namespace control = headunit::vehicle_control;
namespace vehicle = headunit::theme::vehicle;
namespace op = headunit::theme::opack;
using op::Value;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)

namespace {
void rejects(auto f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid operation accepted");
}
Clock::time_point at(std::uint64_t ms, unsigned extra_us = 0) {
  return Clock::time_point{std::chrono::duration_cast<Clock::duration>(
      std::chrono::milliseconds{ms} + std::chrono::microseconds{extra_us})};
}
control::Key key(std::string_view value) {
  const auto result = control::parse_key(value);
  CHECK(result);
  return *result;
}
constexpr std::string_view temperature = "hvac.driver.temperature";
constexpr std::string_view fan = "hvac.driver.fan";
constexpr std::string_view power = "hvac.power";

vehicle::Configuration read_only_configuration() {
  // These are parsed capability aliases, not hand-authored wire IDs. The
  // initial fields are simulation defaults and must not become actual state.
  return vehicle::parse_configuration(R"({"version":1,"cabin":{"power":{
    "binding":"hvac.power","writable":false,"initial":false}},"zones":[{
    "id":"driver","layout":"seat_front_left","temperature":{
      "binding":"hvac.driver.temperature","writable":false,"initial":21,
      "minimum":16,"maximum":30,"step":0.5},"fan":{
      "binding":"hvac.driver.fan","writable":false,"initial":40}}]})",
                                      R"({"version":1,"experimental":true,"sections":[{
      "id":"hvac","label":"Confirmed climate state"}],"settings":[
      {"id":"hvac.temperature","section":"hvac","label":"Temperature",
        "kind":"float","binding":"hvac.driver.temperature","writable":false,
        "initial":21,"minimum":16,"maximum":30,"step":0.5},
      {"id":"hvac.fan","section":"hvac","label":"Fan","kind":"integer",
        "binding":"hvac.driver.fan","writable":false,"initial":40,
        "minimum":0,"maximum":100,"step":1},
      {"id":"hvac.power","section":"hvac","label":"Climate power",
        "kind":"boolean","binding":"hvac.power","writable":false,"initial":false}]})");
}
vehicle::Configuration writable_configuration() {
  return vehicle::parse_configuration(R"({"version":1,"zones":[{
    "id":"driver","layout":"seat_front_left","temperature":{
      "binding":"hvac.driver.temperature","writable":true}}]})",
                                      "");
}
std::shared_ptr<control::Coordinator> coordinator(
    Clock::duration maximum_age = std::chrono::seconds(2)) {
  const std::array<control::TargetPolicy, 3> policies{
      {{key(temperature), control::ValueKind::real, true, 16, 30, 0.5, maximum_age},
       {key(fan), control::ValueKind::integer, false, 0, 100, 1, maximum_age},
       {key(power), control::ValueKind::boolean, false, {}, {}, {}, maximum_age}}};
  auto result = std::make_shared<control::Coordinator>(policies);
  result->begin_generation(1, at(100), at(100));
  return result;
}
// The general fixtures deliberately use integer owner times. A separate test
// below exercises fine owner dates and service ticks with an explicit clock.
class TickClockProvider final : public vehicle::Provider {
  Clock::time_point now_{};
  std::shared_ptr<vehicle::Provider> bridge_;

 public:
  explicit TickClockProvider(std::shared_ptr<control::Coordinator> c)
      : bridge_(vehicle::confirmed_state_provider(std::move(c), [this] { return now_; })) {}
  std::optional<vehicle::Sample> read(std::string_view semantic_key, std::uint64_t ms) override {
    constexpr auto maximum =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count();
    if (ms <= static_cast<std::uint64_t>(maximum)) now_ = at(ms);
    return bridge_->read(semantic_key, ms);
  }
  Value apply(std::string_view semantic_key, const Value& value, std::uint64_t ms) override {
    return bridge_->apply(semantic_key, value, ms);
  }
};
std::shared_ptr<vehicle::Provider> test_provider(std::shared_ptr<control::Coordinator> c) {
  return std::make_shared<TickClockProvider>(std::move(c));
}
void observe(const std::shared_ptr<control::Coordinator>& c, std::string_view target,
             control::Value value, Clock::time_point confirmed, Clock::time_point now,
             std::uint64_t generation = 1) {
  c->observe({generation, key(target), std::move(value), confirmed, control::Origin::measured},
             now);
}
std::vector<std::int64_t> ids(const vehicle::Configuration& configuration,
                              std::string_view semantic_key) {
  std::vector<std::int64_t> result;
  for (const auto& signal : configuration.signals)
    if (signal.key == semantic_key) result.push_back(signal.policy.iid);
  CHECK(!result.empty());
  return result;
}
void readings(vehicle::ConfiguredModel& model, const vehicle::Configuration& configuration,
              std::string_view semantic_key, std::optional<Value> expected, std::uint64_t now) {
  const auto fields = ids(configuration, semantic_key);
  const auto snapshot = model.service().snapshot(fields, now);
  for (const auto id : fields)
    CHECK(snapshot.at(id).value == expected);
  // The actual phone-facing service returns the same values or per-field
  // unavailability. No numeric characteristic or resource IDs are guessed.
  op::Array requested;
  for (const auto id : fields)
    requested.push_back(Value{id});
  const auto response = model.service().handle(
      vehicle::encode_outgoing({"readRequest", 1, Value{std::move(requested)}, {}, {}}, 0), now);
  CHECK(response);
  const auto command = vehicle::decode_incoming(*response).command;
  CHECK(command.name == "readResponse" && !command.error);
  if (expected) {
    CHECK(command.values && !command.errors);
    const auto& values = std::get<op::Dictionary>(command.values->data);
    CHECK(values.size() == fields.size());
    for (const auto& [id, value] : values) {
      CHECK(snapshot.contains(std::get<std::int64_t>(id.data)));
      CHECK(value == *expected);
    }
  } else {
    CHECK(!command.values && command.errors);
    CHECK(std::get<op::Dictionary>(command.errors->data).size() == fields.size());
  }
}
vehicle::Command write(vehicle::ConfiguredModel& model, std::int64_t iid, Value value,
                       std::uint64_t now) {
  const auto response = model.service().handle(
      vehicle::encode_outgoing(
          {"writeRequest", 1, Value{op::Dictionary{{Value{iid}, std::move(value)}}}, {}, {}}, 0),
      now);
  CHECK(response);
  return vehicle::decode_incoming(*response).command;
}
void delayed_readings_and_expiry() {
  const auto configuration = read_only_configuration();
  CHECK(configuration.signals.size() == 7);
  for (const auto& signal : configuration.signals)
    CHECK(!signal.writable);
  auto c = coordinator();
  auto provider = test_provider(c);
  vehicle::ConfiguredModel model(configuration, provider);
  model.refresh(1000);
  for (const auto semantic_key : {temperature, fan, power})
    readings(model, configuration, semantic_key, {}, 1000);
  observe(c, temperature, 21.5, at(900, 750), at(1100));
  observe(c, fan, std::int64_t{43}, at(901, 250), at(1100));
  observe(c, power, true, at(903, 750), at(1100));
  model.refresh(1100);
  readings(model, configuration, temperature, Value{21.5}, 1100);
  readings(model, configuration, fan, Value{std::int64_t{43}}, 1100);
  readings(model, configuration, power, Value{true}, 1100);
  const auto original = provider->read(temperature, 1200);
  CHECK(original && original->sampled_ms == 900 && original->value == Value{21.5});
  const auto repeated = provider->read(temperature, 1800);
  CHECK(repeated && repeated->sampled_ms == 900 && repeated->value == original->value);
  model.refresh(1800);
  readings(model, configuration, temperature, Value{21.5}, 1800);
  model.refresh(1900);
  readings(model, configuration, temperature, {}, 1900);
  // Retained Coordinator state still has its precise original confirmation;
  // the shorter capability age, not the most recent poll, expired this alias.
  const auto precise = c->read(key(temperature), at(1900));
  CHECK(precise && precise->confirmed_at == at(900, 750));
  observe(c, temperature, 22.0, at(1850, 750), at(2000));
  model.refresh(2000);
  readings(model, configuration, temperature, Value{22.0}, 2000);
  CHECK(provider->read(temperature, 2000)->sampled_ms == 1850);
}
void exact_coordinator_age_and_loss() {
  const auto configuration = read_only_configuration();
  auto c = coordinator(std::chrono::milliseconds(500));
  auto provider = test_provider(c);
  observe(c, temperature, 21.5, at(1000, 750), at(1100));
  vehicle::ConfiguredModel model(configuration, provider);
  model.refresh(1100);
  CHECK(provider->read(temperature, 1500));   // Exactly 499.25 ms old.
  CHECK(!provider->read(temperature, 1501));  // Core age is 500.25 ms.
  model.refresh(1600);
  readings(model, configuration, temperature, {}, 1600);
  rejects([&] { observe(c, temperature, 21.5, at(1000, 750), at(1600)); });
  observe(c, temperature, 22.0, at(1550, 250), at(1700));
  model.refresh(1700);
  readings(model, configuration, temperature, Value{22.0}, 1700);
  CHECK(provider->read(temperature, 1700)->sampled_ms == 1550);
  c->disconnect(at(1750));
  model.refresh(1800);
  readings(model, configuration, temperature, {}, 1800);
  model.disconnect();
  model.refresh(1800);
  readings(model, configuration, temperature, {}, 1800);
  c->begin_generation(2, at(1800), at(1800));
  // A delayed actual reading can recover below the last poll's date. Loss is
  // not a fabricated new confirmation at 1800/1900 ms.
  observe(c, temperature, 22.5, at(1850, 250), at(2000), 2);
  model.refresh(2000);
  readings(model, configuration, temperature, Value{22.5}, 2000);
  CHECK(provider->read(temperature, 2000)->sampled_ms == 1850);
}
void commands_publish_only_confirmed_actual() {
  const auto configuration = read_only_configuration();
  auto c = coordinator();
  auto provider = test_provider(c);
  observe(c, temperature, 21.0, at(900), at(1000));
  vehicle::ConfiguredModel model(configuration, provider);
  model.refresh(1000);
  const control::Request request{1, 1, key(temperature), 23.0, at(1100), at(1500)};
  CHECK(c->submit(request, at(1100)).status == control::Status::accepted);
  model.refresh(1100);
  readings(model, configuration, temperature, Value{21.0}, 1100);
  CHECK(c->dispatch(1, at(1100), true) == request);
  CHECK(c->acknowledge({1, 1, 1, key(temperature), control::ReplyStatus::pending, at(1110), {}},
                       at(1110))
            .status == control::Status::pending);
  // Even an exact matching actual sensor reading leaves this command pending.
  observe(c, temperature, 23.0, at(1120), at(1125));
  model.refresh(1200);
  readings(model, configuration, temperature, Value{23.0}, 1200);
  CHECK(c->snapshot(1, at(1200))->status == control::Status::pending);
  const control::ConfirmedState applied{1, key(temperature), 22.0, at(1230, 250),
                                        control::Origin::measured};
  const auto result = c->acknowledge(
      {1, 1, 2, key(temperature), control::ReplyStatus::applied, at(1250), applied}, at(1250));
  CHECK(result.status == control::Status::applied && result.acknowledgement->confirmed == applied);
  model.refresh(1300);
  readings(model, configuration, temperature, Value{22.0}, 1300);
  const auto value = provider->read(temperature, 1400);
  CHECK(value && value->sampled_ms == 1230 && value->value == Value{22.0});
  model.refresh(2250);
  readings(model, configuration, temperature, {}, 2250);
}
void collisions_do_not_retain_or_revive() {
  const auto configuration = read_only_configuration();
  auto c = coordinator();
  auto provider = test_provider(c);
  observe(c, temperature, 21.0, at(1000, 100), at(1001));
  vehicle::ConfiguredModel model(configuration, provider);
  model.refresh(1001);
  readings(model, configuration, temperature, Value{21.0}, 1001);
  observe(c, temperature, 22.0, at(1000, 200), at(1001));
  CHECK(!provider->read(temperature, 1001));
  CHECK(!provider->read(temperature, 1101));
  model.refresh(1101);
  readings(model, configuration, temperature, {}, 1101);
  model.disconnect();
  model.refresh(1101);
  readings(model, configuration, temperature, {}, 1101);
  vehicle::ConfiguredModel new_connection(configuration, provider);
  new_connection.refresh(1101);
  readings(new_connection, configuration, temperature, {}, 1101);
  // More precise acquisitions within the collided millisecond cannot revive.
  observe(c, temperature, 22.5, at(1000, 300), at(1101));
  CHECK(!provider->read(temperature, 1101));
  observe(c, temperature, 23.0, at(1001, 50), at(1101));
  model.refresh(1201);
  readings(model, configuration, temperature, Value{23.0}, 1201);
  CHECK(provider->read(temperature, 1201)->sampled_ms == 1001);
}
void generation_collisions_and_scalar_precision() {
  const auto configuration = read_only_configuration();
  auto c = coordinator();
  auto provider = test_provider(c);
  observe(c, temperature, 21.0, at(1000), at(1000));
  vehicle::ConfiguredModel model(configuration, provider);
  model.refresh(1000);
  c->begin_generation(2, at(1000, 200), at(1000, 300));
  observe(c, temperature, 22.0, at(1000, 300), at(1000, 300), 2);
  // The generation is new, but the wire date is indistinguishable from the
  // old generation. Neither identical nor different values can revive it.
  CHECK(!provider->read(temperature, 1001));
  model.refresh(1100);
  readings(model, configuration, temperature, {}, 1100);
  observe(c, temperature, 22.0, at(1100, 750), at(1101), 2);
  model.refresh(1200);
  readings(model, configuration, temperature, Value{22.0}, 1200);
  // The bridge preserves actual precision without rounding to request steps;
  // stricter capability validation remains the ConfiguredModel's own policy.
  observe(c, temperature, 22.35, at(1250, 250), at(1300), 2);
  const auto precise = provider->read(temperature, 1300);
  CHECK(precise && precise->value == Value{22.35} && precise->sampled_ms == 1250);
  model.refresh(1300);
  readings(model, configuration, temperature, {}, 1300);
}
void all_phone_writes_reject_without_side_effects() {
  const auto configuration = writable_configuration();
  auto c = coordinator();
  auto provider = test_provider(c);
  observe(c, temperature, 21.0, at(900), at(1000));
  vehicle::ConfiguredModel model(configuration, provider);
  model.refresh(1000);
  const auto writable = configuration.signals.front().policy.iid;
  CHECK(configuration.signals.front().writable);
  CHECK(!c->snapshot(1, at(1000)));
  const auto response = write(model, writable, Value{23.0}, 1001);
  CHECK(response.name == "writeResponse" && response.errors && !response.values && !response.error);
  const op::Dictionary expected{{Value{writable}, Value{std::int64_t{-6705}}}};
  CHECK(*response.errors == Value{expected});
  readings(model, configuration, temperature, Value{21.0}, 1001);
  CHECK(!c->snapshot(1, at(1000)));
  CHECK(c->read(key(temperature), at(1000))->confirmed_at == at(900));
  rejects([&] {
    provider->apply(temperature, Value{23.0}, std::numeric_limits<std::uint64_t>::max());
  });
  rejects([&] { provider->apply("unknown", Value{std::string{"text"}}, 1001); });
  // No hidden command, identifier reservation or clock advancement occurred.
  const control::Request request{1, 1, key(temperature), 23.0, at(1000), at(1500)};
  CHECK(c->submit(request, at(1000)).status == control::Status::accepted);
  CHECK(c->dispatch(1, at(1000), true) == request);
}
void key_and_clock_bounds() {
  rejects([] { vehicle::confirmed_state_provider({}, [] { return at(100); }); });
  auto c = coordinator();
  rejects([&] { vehicle::confirmed_state_provider(c, {}); });
  auto provider = test_provider(c);
  CHECK(!provider->read("invalid key", 100));
  CHECK(!provider->read("unconfigured", 100));
  CHECK(!provider->read(std::string(129, 'x'), 100));
  rejects([&] { provider->read(temperature, std::numeric_limits<std::uint64_t>::max()); });
  constexpr auto maximum =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count();
  CHECK(!provider->read("unconfigured", static_cast<std::uint64_t>(maximum)));
  rejects([&] { provider->read("unconfigured", static_cast<std::uint64_t>(maximum) + 1); });
  observe(c, temperature, 21.0, at(900), at(1000));
  CHECK(provider->read(temperature, 1000));
  rejects([&] { provider->read(temperature, 999); });
  CHECK(provider->read(temperature, 1000));  // Invalid calls leave the bridge unchanged.
  observe(c, temperature, 21.5, at(1001), at(1100));
  CHECK(provider->read(temperature, 1100));
}
void fine_owner_clock_and_service_tick() {
  const auto configuration = read_only_configuration();
  auto c = coordinator();
  auto owner_now = at(1000, 500);
  bool fail_clock = false;
  auto provider = vehicle::confirmed_state_provider(c, [&] {
    if (fail_clock) throw std::runtime_error("owner clock unavailable");
    return owner_now;
  });
  observe(c, temperature, 21.5, at(1000, 500), owner_now);
  vehicle::ConfiguredModel model(configuration, provider);
  model.refresh(1000);
  readings(model, configuration, temperature, Value{21.5}, 1000);
  CHECK(provider->read(temperature, 1000)->sampled_ms == 1000);
  const control::Request request{1, 1, key(temperature), 23.0, at(1100, 100), at(1500)};
  owner_now = at(1100, 100);
  CHECK(c->submit(request, owner_now).status == control::Status::accepted);
  CHECK(c->dispatch(1, owner_now, true) == request);
  const control::ConfirmedState applied{1, key(temperature), 22.0, at(1100, 500),
                                        control::Origin::measured};
  owner_now = at(1100, 750);
  CHECK(
      c->acknowledge({1, 1, 1, key(temperature), control::ReplyStatus::applied, owner_now, applied},
                     owner_now)
          .status == control::Status::applied);
  // The same 1100-ms service tick can read a real 1100.5-ms confirmation using
  // the owner's precise operation clock, without rounding the read up by 1 ms.
  model.refresh(1100);
  readings(model, configuration, temperature, Value{22.0}, 1100);
  CHECK(provider->read(temperature, 1100)->sampled_ms == 1100);
  rejects([&] { provider->read(temperature, 1101); });  // Ahead of owner time.
  rejects([&] { provider->read(temperature, 1099); });  // Reversed service tick.
  CHECK(provider->read(temperature, 1100)->value == Value{22.0});
  owner_now = at(1100, 250);
  rejects([&] { provider->read(temperature, 1100); });  // Reversed fine owner time.
  owner_now = at(1100, 750);
  CHECK(provider->read(temperature, 1100)->value == Value{22.0});
  fail_clock = true;
  try {
    provider->read(temperature, 1100);
    throw std::logic_error("clock failure accepted");
  } catch (const std::runtime_error&) {
  }
  fail_clock = false;
  CHECK(provider->read(temperature, 1100)->value == Value{22.0});
}
void later_tick_confirmation_is_not_consumed() {
  const auto configuration = read_only_configuration();
  auto c = coordinator();
  auto owner_now = at(1000);
  auto provider = vehicle::confirmed_state_provider(c, [&] { return owner_now; });
  observe(c, temperature, 21.0, at(1000), owner_now);
  vehicle::ConfiguredModel model(configuration, provider);
  model.refresh(1000);
  owner_now = at(1101, 250);
  observe(c, temperature, 21.5, at(1101, 100), owner_now);
  // The caller's captured tick is older than the actual owner's operation.
  // This valid observation becomes eligible later without an invented date.
  model.refresh(1100);
  readings(model, configuration, temperature, {}, 1100);
  CHECK(!provider->read(temperature, 1100));
  const auto eligible = provider->read(temperature, 1101);
  CHECK(eligible && eligible->value == Value{21.5} && eligible->sampled_ms == 1101);
  owner_now = at(1200, 250);
  model.refresh(1200);
  readings(model, configuration, temperature, Value{21.5}, 1200);
  CHECK(provider->read(temperature, 1200)->sampled_ms == 1101);
}
}  // namespace

int main() try {
  delayed_readings_and_expiry();
  exact_coordinator_age_and_loss();
  commands_publish_only_confirmed_actual();
  collisions_do_not_retain_or_revive();
  generation_collisions_and_scalar_precision();
  all_phone_writes_reject_without_side_effects();
  key_and_clock_bounds();
  fine_owner_clock_and_service_tick();
  later_tick_confirmation_is_not_consumed();
  std::cout << "confirmed-state vehicle provider checks passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
