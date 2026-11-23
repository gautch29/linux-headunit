// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gauge_input.hpp>
#include <headunit/night_mode.hpp>
#include <headunit/vehicle_facts.hpp>
#include <headunit/theme/vehicle_data.hpp>
#include <headunit/binary_plist.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/vehicle_observation_wire.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
#ifdef HEADUNIT_TEST_GAUGE_SOCKET
#include <headunit/socket_vehicle.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <cstring>
#endif

using namespace headunit;
namespace v = headunit::theme::vehicle;
using P = PlistValue;
using V = headunit::theme::opack::Value;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid gauge input accepted");
}
Clock::time_point time_ms(std::uint64_t n) {
  return Clock::time_point{std::chrono::milliseconds(n)};
}
struct Input final : VehicleSource {
  VehicleSample value;
  VehicleSample sample(Clock::time_point) override { return value; }
};
v::LocalProfile profile(bool extended = false, bool transmission = false, bool drive_mode = false,
                        bool trip = false) {
  P::Array signals;
  const std::array names{"speed", "speedKMH", "speedMPH", "rotationalSpeed",
                         "rotationalSpeedState"};
  for (unsigned i = 0; i < 5; ++i) {
    signals.emplace_back(P::Dictionary{
        {"iid", P{std::int64_t(i + 3)}},
        {"kind", P{std::string(i == 4 ? "integer" : "real")}},
        {"maxAgeMs", P{std::int64_t(250)}},
        {"path", P{P::Array{P{std::string(i < 3 ? "vehicleMotion" : "internalCombustionEngine")},
                            P{std::string(i < 3 ? "displayedSpeed" : "engineRPM")},
                            P{std::string(names[i])}}}}});
  }
  if (extended) {
    const auto add = [&](std::int64_t id, const char* name, const char* kind, const char* field,
                         P::Dictionary extra) {
      P::Dictionary entry{
          {"iid", P{id}},
          {"kind", P{std::string(kind)}},
          {"maxAgeMs", P{std::int64_t(250)}},
          {"vehicleField", P{std::string(field)}},
          {"path", P{P::Array{P{std::string("localVehicle")}, P{std::string("instruments")},
                              P{std::string(name)}}}}};
      entry.insert(extra.begin(), extra.end());
      signals.emplace_back(std::move(entry));
    };
    add(20, "fuel", "real", "fuel_percent", {{"vehicleScale", P{0.01}}});
    add(21, "coolantF", "real", "coolant_c", {{"vehicleScale", P{1.8}}, {"vehicleOffset", P{32.}}});
    add(22, "engineLamp", "integer", "check_engine",
        {{"vehicleStates",
          P{P::Array{P{std::int64_t(0)}, P{std::int64_t(3)}, P{std::int64_t(5)}}}}});
    add(23, "highBeam", "boolean", "high_beam",
        {{"vehicleStates", P{P::Array{P{false}, P{true}, P{true}}}}});
    add(24, "rangeKM", "real", "range_km", {});
    add(25, "rangeMiles", "real", "range_km", {{"vehicleScale", P{1.0 / 1.609344}}});
    add(26, "odometerKM", "real", "odometer_km", {});
    add(27, "odometerMiles", "real", "odometer_km", {{"vehicleScale", P{1.0 / 1.609344}}});
    add(28, "outsideC", "real", "ambient_c", {});
  }
  if (transmission) {
    const auto add = [&](std::int64_t id, const char* field, const char* kind, P::Dictionary states,
                         const char* name) {
      signals.emplace_back(P::Dictionary{
          {"iid", P{id}},
          {"kind", P{std::string(kind)}},
          {"maxAgeMs", P{std::int64_t(250)}},
          {"vehicleField", P{std::string(field)}},
          {"vehicleStates", P{std::move(states)}},
          {"path", P{P::Array{P{std::string("driveState")}, P{std::string("transmissionStatus")},
                              P{std::string(name)}}}}});
    };
    add(30, "transmission.selector", "integer",
        {{"drive", P{std::int64_t(68)}},
         {"manual", P{std::int64_t(77)}},
         {"sport", P{std::int64_t(83)}},
         {"reverse", P{std::int64_t(82)}},
         {"neutral", P{std::int64_t(78)}}},
        "transmissionMode");
    // Asset mappings are explicit fixture data, not inferred model encodings.
    add(31, "transmission.gear", "integer",
        {{"4", P{std::int64_t(104)}},
         {"neutral", P{std::int64_t(201)}},
         {"reverse", P{std::int64_t(202)}}},
        "gearPosition");
    add(32, "transmission.gearbox_type", "string",
        {{"automatic", P{std::string("AUTO")}}, {"manual", P{std::string("MAN")}}}, "hardwareType");
  }
  if (drive_mode) {
    signals.emplace_back(
        P::Dictionary{{"iid", P{std::int64_t(40)}},
                      {"kind", P{std::string("array")}},
                      {"maxAgeMs", P{std::int64_t(250)}},
                      {"fixedValue", P{P::Array{P{std::string("sport")}, P{std::string("GT")}}}},
                      {"path", P{P::Array{P{std::string("driveState")}, P{std::string("driveMode")},
                                          P{std::string("identifiers")}}}}});
    signals.emplace_back(P::Dictionary{
        {"iid", P{std::int64_t(41)}},
        {"kind", P{std::string("integer")}},
        {"maxAgeMs", P{std::int64_t(250)}},
        {"vehicleField", P{std::string("drive_mode")}},
        {"vehicleStates",
         P{P::Dictionary{{"comfort", P{std::int64_t(1)}}, {"sport", P{std::int64_t(0)}}}}},
        {"path", P{P::Array{P{std::string("driveState")}, P{std::string("driveMode")},
                            P{std::string("currentIndex")}}}}});
    signals.emplace_back(P::Dictionary{
        {"iid", P{std::int64_t(42)}},
        {"kind", P{std::string("string")}},
        {"maxAgeMs", P{std::int64_t(250)}},
        {"vehicleField", P{std::string("drive_mode")}},
        {"vehicleStates", P{P::Dictionary{{"comfort", P{std::string("COMFORT")}},
                                          {"sport", P{std::string("SPORT")}}}}},
        {"path", P{P::Array{P{std::string("localVehicle")}, P{std::string("instruments")},
                            P{std::string("modeLabel")}}}}});
  }
  if (trip) {
    const auto add = [&](std::int64_t id, const char* name, const char* kind, P::Dictionary extra) {
      P::Dictionary entry{{"iid", P{id}},
                          {"kind", P{std::string(kind)}},
                          {"maxAgeMs", P{std::int64_t(250)}},
                          {"instance", P{std::string("currentTrip")}},
                          {"path", P{P::Array{P{std::string("tripComputer")},
                                              P{std::string("trip")}, P{std::string(name)}}}}};
      entry.insert(extra.begin(), extra.end());
      signals.emplace_back(std::move(entry));
    };
    add(50, "userVisibleLabel", "string", {{"fixedValue", P{std::string("Current Trip")}}});
    for (const auto& [id, name, kind] :
         std::array{std::tuple{51, "fuelEfficiency", "real"},
                    std::tuple{52, "localIntegerConsumption", "integer"}})
      add(id, name, kind,
          {{"vehicleField", P{std::string("trip.fuel_efficiency_l_per_100km")}},
           {"vehicleScale", P{1000.}},
           {"minimumValue", P{0.}},
           {"maximumValue", P{200000.}}});
  }
  return v::decode_local_profile(
      encode_binary_plist(P{P::Dictionary{{"schemaVersion", P{std::int64_t(1)}},
                                          {"mode", P{std::string("local-ui-bindings")}},
                                          {"signals", P{std::move(signals)}}}}));
}
TripState trip_state(std::uint64_t reset, double distance, double duration,
                     std::optional<double> consumption) {
  TripState value;
  value.identifier = *parse_trip_identifier("current");
  value.reset_generation = reset;
  value.distance_km = distance;
  value.elapsed_seconds = duration;
  value.moving_seconds = duration * 0.8;
  value.fuel_efficiency_l_per_100km = consumption;
  return value;
}
void night_mode_source_admission() {
  // Simulated acquisitions in the real cache, then a deliberately faulty
  // VehicleSource at the production GaugeInput admission boundary. Dates stay
  // in the past so the real receiver can consume its existing two-second lease.
  clear_vehicle_facts();
  const auto started = std::chrono::time_point_cast<std::chrono::milliseconds>(Clock::now()) -
                       std::chrono::seconds(1);
  const auto at = [&](unsigned ms) {
    return started + std::chrono::milliseconds(ms);
  };
  const auto headlights = *parse_night_mode_policy("headlights");
  const auto schedule = *parse_night_mode_policy("schedule");
  const auto lamp_index = static_cast<std::size_t>(Indicator::low_beam);
  VehicleObservationCache cache;
  cache.begin_generation(10, at(0), at(0));
  const auto acquire = [&](unsigned ms, std::uint64_t generation, std::uint64_t sequence,
                           std::optional<IndicatorState> lamp) {
    const VehicleObservation observation{
        "low_beam", lamp ? VehicleObservationValue{*lamp} : VehicleObservationValue{}, at(ms),
        VehicleObservationOrigin::simulation,
        lamp ? VehicleAvailability::available : VehicleAvailability::unavailable};
    cache.accept(generation, sequence, std::span{&observation, 1}, at(ms));
  };
  acquire(1, 10, 1, IndicatorState::on);
  auto configured = profile();
  auto source = std::make_unique<Input>();
  auto* data = source.get();
  data->value = cache.sample(at(1));
  v::GaugeInput bridge(configured, std::move(source));
  const auto decision = [&](std::optional<bool> expected, unsigned accepted_ms) {
    const auto facts = fresh_vehicle_facts(at(accepted_ms));
    check(
        facts && facts->low_beam == expected && facts->observed_at == at(accepted_ms) &&
            facts->speed_kmh == current_vehicle_sample(bridge.sample(), at(accepted_ms)).speed_kmh,
        "rejected low-beam snapshot changed admitted receiver facts");
    check(night_mode_at(headlights, 12 * 60, facts->low_beam) == expected.value_or(false) &&
              night_mode_at(headlights, 23 * 60, facts->low_beam) == expected.value_or(true),
          "missing low beam became measured false instead of schedule fallback");
    if (expected) {
      set_receiver_night_mode(headlights);
      check(receiver_night_mode_now() == expected,
            "real receiver did not consume the admitted headlight decision");
    } else {
      // Bracket genuine wall-clock policy evaluation; no car acquisition is
      // derived from wall time, including a legitimate schedule boundary.
      set_receiver_night_mode(schedule);
      const auto before = receiver_night_mode_now();
      set_receiver_night_mode(headlights);
      const auto fallback = receiver_night_mode_now();
      set_receiver_night_mode(schedule);
      const auto after = receiver_night_mode_now();
      check(fallback && (fallback == before || fallback == after),
            "real receiver ignored unknown-headlight schedule fallback");
    }
  };
  const auto pump = [&](unsigned ms) {
    data->value = cache.sample(at(ms));
    bridge.pump(at(ms));
  };
  bridge.pump(at(1));
  decision(true, 1);
  check(bridge.sample().observation_timing->indicators[lamp_index].acquired_at == at(1) &&
            bridge.sample().observation_timing->indicators[lamp_index].origin ==
                VehicleObservationOrigin::simulation,
        "headlight acquisition or simulated provenance changed");
  const auto original = data->value;
  data->value.indicator(Indicator::low_beam) = IndicatorState::off;
  data->value.sampled_at = at(2);
  data->value.speed_kmh = 40.;
  data->value.observation_timing->readings[0] = {at(2), std::chrono::milliseconds(500),
                                                 VehicleObservationOrigin::simulation,
                                                 VehicleAvailability::available};
  rejects([&] { bridge.pump(at(2)); });
  check(bridge.sample().indicator(Indicator::low_beam) == IndicatorState::on,
        "conflicting low beam changed the accepted sample");
  check(receiver_night_mode_now() == true,
        "rejected equal-date low beam changed real receiver night mode");
  decision(true, 1);
  data->value = original;
  rejects([&] { bridge.pump(at(0)); });
  decision(true, 1);
  data->value.indicator(Indicator::low_beam) = IndicatorState::off;
  data->value.sampled_at = at(3);
  data->value.observation_timing->indicators[lamp_index].acquired_at = at(3);
  data->value.observation_timing->readings[0].maximum_age += std::chrono::milliseconds(1);
  rejects([&] { bridge.pump(at(3)); });
  decision(true, 1);
  acquire(10, 10, 2, std::nullopt);
  pump(10);
  decision(std::nullopt, 10);
  acquire(11, 10, 3, IndicatorState::on);
  pump(11);
  decision(true, 11);
  acquire(12, 10, 4, IndicatorState::off);
  pump(12);
  decision(false, 12);
  pump(511);
  decision(false, 511);
  pump(512);  // Exactly acquisition + the unchanged 500 ms age policy.
  decision(std::nullopt, 512);
  check(bridge.sample().observation_timing->indicators[lamp_index].acquired_at == at(12),
        "poll refreshed the expired headlight acquisition");
  acquire(513, 10, 5, IndicatorState::on);
  pump(513);
  decision(true, 513);
  const auto before_withdrawal = data->value;
  cache.withdraw(at(520));
  pump(520);
  decision(std::nullopt, 520);
  data->value = before_withdrawal;
  data->value.sampled_at = at(521);
  rejects([&] { bridge.pump(at(521)); });
  decision(std::nullopt, 520);
  cache.begin_generation(11, at(530), at(530));
  pump(530);
  decision(std::nullopt, 530);
  data->value = before_withdrawal;
  data->value.sampled_at = at(531);
  rejects([&] { bridge.pump(at(531)); });
  decision(std::nullopt, 530);
  acquire(540, 11, 1, IndicatorState::off);
  pump(540);
  decision(false, 540);
  data->value = before_withdrawal;
  data->value.sampled_at = at(541);
  rejects([&] { bridge.pump(at(541)); });
  decision(false, 540);
  acquire(550, 11, 2, IndicatorState::on);
  pump(550);
  decision(true, 550);
  // Even a later service-publication rejection must leave facts atomic.
  configured.source->publish_batch(
      {},
      std::chrono::duration_cast<std::chrono::milliseconds>(at(560).time_since_epoch()).count());
  acquire(551, 11, 3, IndicatorState::off);
  data->value = cache.sample(at(551));
  rejects([&] { bridge.pump(at(551)); });
  decision(true, 550);
  check(bridge.sample().indicator(Indicator::low_beam) == IndicatorState::on,
        "failed service publication changed accepted low beam");
  // The legacy new-sample and repeated-sample returns share the same admission
  // rule. A service rejection cannot refresh even an unchanged facts lease.
  auto legacy_profile = profile();
  auto legacy_source = std::make_unique<Input>();
  auto* legacy = legacy_source.get();
  legacy->value.sampled_at = at(600);
  legacy->value.simulated = true;
  legacy->value.speed_kmh = 10.;
  legacy->value.indicator(Indicator::low_beam) = IndicatorState::on;
  v::GaugeInput legacy_bridge(legacy_profile, std::move(legacy_source));
  legacy_bridge.pump(at(600));
  legacy_profile.source->publish_batch(
      {},
      std::chrono::duration_cast<std::chrono::milliseconds>(at(620).time_since_epoch()).count());
  const auto legacy_facts = fresh_vehicle_facts(at(600));
  check(legacy_facts && legacy_facts->low_beam == true && legacy_facts->speed_kmh == 10.,
        "legacy source did not publish admitted facts");
  rejects([&] { legacy_bridge.pump(at(601)); });
  legacy->value.sampled_at = at(610);
  legacy->value.speed_kmh = 20.;
  legacy->value.indicator(Indicator::low_beam) = IndicatorState::off;
  rejects([&] { legacy_bridge.pump(at(610)); });
  const auto retained = fresh_vehicle_facts(at(620));
  check(retained && retained->observed_at == legacy_facts->observed_at &&
            retained->low_beam == legacy_facts->low_beam &&
            retained->speed_kmh == legacy_facts->speed_kmh,
        "rejected legacy publication changed facts or refreshed their lease");
  legacy_bridge.pump(at(620));
  const auto recovered = fresh_vehicle_facts(at(620));
  check(recovered && recovered->observed_at == at(620) && recovered->low_beam == false &&
            recovered->speed_kmh == 20.,
        "valid legacy publication did not recover after service rejection");
  clear_vehicle_facts();
  set_receiver_night_mode(std::nullopt);
}
int main() try {
  night_mode_source_admission();
  const std::array<std::int64_t, 5> ids{3, 4, 5, 6, 7};
  auto p = profile();
  auto source = std::make_unique<Input>();
  auto* input = source.get();
  input->value = {80.4672, 2400, time_ms(1000)};
  input->value.indicator(Indicator::high_beam) = IndicatorState::on;
  input->value.simulated = true;
  v::GaugeInput bridge(p, std::move(source));
  check(!bridge.sample().indicator(Indicator::high_beam), "unread source exposed a lamp");
  bridge.pump(time_ms(1000));
  check(bridge.sample().indicator(Indicator::high_beam) == IndicatorState::on &&
            bridge.sample().simulated,
        "shared sample lost indicators or simulation marker");
  auto readings = p.source->snapshot(ids, 1000);
  check(std::abs(std::get<double>(readings.at(3).value->data) - 80467.2) < 1e-7 &&
            readings.at(4).value == V{80.4672},
        "speed scaling");
  check(std::abs(std::get<double>(readings.at(5).value->data) - 50) < 1e-9, "mph scaling");
  check(readings.at(6).value == V{2400.} && readings.at(7).value == V{std::int64_t(0)},
        "RPM and state");
  bridge.pump(time_ms(1249));
  check(p.source->snapshot(ids, 1249).at(4).value == V{80.4672}, "premature expiry");
  bridge.pump(time_ms(1250));
  check(p.source->snapshot(ids, 1250).at(4).value == V{}, "polling refreshed stale sample");
  check(bridge.sample().sampled_at == time_ms(1000), "shared warning sample renewed freshness");
  input->value = {std::nullopt, 0, time_ms(1300)};
  bridge.pump(time_ms(1300));
  readings = p.source->snapshot(ids, 1300);
  check(readings.at(3).value == V{} && readings.at(4).value == V{} && readings.at(5).value == V{},
        "missing speed became zero");
  check(readings.at(6).value == V{0.} && readings.at(7).value == V{std::int64_t(0)},
        "valid zero RPM lost");
  input->value = {0, std::nullopt, time_ms(1400)};
  bridge.pump(time_ms(1400));
  readings = p.source->snapshot(ids, 1400);
  check(readings.at(4).value == V{0.} && readings.at(6).value == V{} && readings.at(7).value == V{},
        "missing RPM state retained");
  input->value = {12, 1200, time_ms(1399)};
  rejects([&] { bridge.pump(time_ms(1500)); });
  input->value.sampled_at = time_ms(1600);
  rejects([&] { bridge.pump(time_ms(1500)); });
  for (double bad : {-1., 301., std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
    input->value = {bad, 1200, time_ms(1500)};
    rejects([&] { bridge.pump(time_ms(1500)); });
  }
  input->value = {12, 9001, time_ms(1500)};
  rejects([&] { bridge.pump(time_ms(1500)); });
  check(p.source->snapshot(ids, 1500).at(4).value == V{0.}, "bad sample partially published");
  check(bridge.sample().sampled_at == time_ms(1400) &&
            !bridge.sample().indicator(Indicator::high_beam),
        "invalid or cleared sample left a lamp latched");
  auto missing = profile();
  missing.bindings.pop_back();
  rejects([&] { v::GaugeInput bad(missing, std::make_unique<Input>()); });
  auto ambiguous = profile();
  ambiguous.bindings.push_back(ambiguous.bindings.front());
  rejects([&] { v::GaugeInput bad(ambiguous, std::make_unique<Input>()); });
  auto configured = profile();
  rejects([&] { v::GaugeInput bad(configured, nullptr); });
  auto type_as_mode = profile(false, true);
  type_as_mode.vehicle_inputs[0].field = "transmission.gearbox_type";
  type_as_mode.vehicle_inputs[0].semantic_states = {{"manual", V{std::int64_t(77)}}};
  rejects([&] { v::GaugeInput bad(type_as_mode, std::make_unique<Input>()); });
  auto sim = profile();
  v::GaugeInput simulation(sim, std::make_unique<SimulatedVehicle>(time_ms(1000)));
  simulation.pump(time_ms(1000));
  const auto first = sim.source->snapshot(ids, 1000);
  simulation.pump(time_ms(2000));
  const auto next = sim.source->snapshot(ids, 2000);
  check(first.at(4).value != next.at(4).value && first.at(6).value != next.at(6).value,
        "simulation did not reach gauges");
  {
    // The bench simulation drives gear- and mode-aware artwork through the same
    // typed bindings as a real source: automatic Drive in 4th, Sport selected.
    auto typed = profile(false, true, true);
    v::GaugeInput typed_simulation(typed, std::make_unique<SimulatedVehicle>(time_ms(1000)));
    typed_simulation.pump(time_ms(11000));
    const std::array<std::int64_t, 3> typed_ids{30, 31, 41};
    const auto values = typed.source->snapshot(typed_ids, 11000);
    check(values.at(30).value == V{std::int64_t(68)} && values.at(31).value == V{std::int64_t(104)},
          "simulated automatic Drive gear did not reach gauges");
    check(values.at(41).value == V{std::int64_t(0)}, "simulated Sport mode did not reach gauges");
  }
  {
    // The signed transmission updater rebuilds its state from one update batch.
    // A gear change alone must still carry the unchanged selector, or the
    // cluster redacts the gear as if the selector were unavailable.
    auto grouped = profile(false, true);
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value.sampled_at = time_ms(1000);
    data->value.transmission =
        TransmissionState{GearboxType::automatic, TransmissionSelector::drive,
                          EngagedGear{EngagedGearKind::forward, 4}};
    v::GaugeInput bridge(grouped, std::move(source));
    bridge.pump(time_ms(1000));
    // CP.getVehicleDataUpdates uses an independent ScriptView, not the
    // phone-facing service's notification queue. Verify that path too.
    v::ScriptView script_view(grouped.source, grouped.bindings);
    const std::array<std::int64_t, 2> subscribed{30, 31};
    script_view.subscribe(subscribed);
    check(script_view.updates(1000).values.size() == 2, "script view omitted initial transmission");
    const auto registration =
        v::encode_outgoing({"registerRequest", std::int64_t(1), V{std::string("*")}, {}, {}}, 0);
    check(bool(grouped.source->handle(registration, 1000)), "registration had no response");
    data->value.sampled_at = time_ms(1010);
    data->value.transmission =
        TransmissionState{GearboxType::automatic, TransmissionSelector::drive,
                          EngagedGear{EngagedGearKind::neutral, 0}};
    bridge.pump(time_ms(1010));
    const auto script_update = script_view.updates(1010);
    check(script_update.values.size() == 2 && script_update.values.at(30) == V{std::int64_t(68)} &&
              script_update.values.at(31) == V{std::int64_t(201)},
          "signed-scene script update omitted its unchanged transmission selector");
    check(script_view.updates(1010).values.empty(), "script view repeated an unchanged batch");
    const auto update = grouped.source->updates(1010);
    check(bool(update), "gear change produced no update");
    const auto command = v::decode_incoming(*update).command;
    bool selector = false;
    bool gear = false;
    for (const auto& [key, value] : std::get<theme::opack::Dictionary>(command.values->data)) {
      const auto id = std::get<std::int64_t>(key.data);
      if (id == 30) selector = value == V{std::int64_t(68)};
      if (id == 31) gear = value == V{std::int64_t(201)};
    }
    check(gear && selector, "gear change update omitted its unchanged selector");
    script_view.subscribe(std::array<std::int64_t, 1>{31});
    check(script_view.updates(1010).values.size() == 1, "partial transmission subscription failed");
    grouped.source->publish(30, V{std::int64_t(77)}, 1020, 1020);
    check(script_view.updates(1020).values.empty(),
          "unsubscribed selector triggered script update");
    grouped.source->publish(31, V{std::int64_t(104)}, 1030, 1030);
    const auto partial = script_view.updates(1030);
    check(partial.values.size() == 1 && partial.values.at(31) == V{std::int64_t(104)},
          "transmission group leaked an unsubscribed selector");
    script_view.subscribe(subscribed);
    check(script_view.updates(1030).values.size() == 2, "transmission resubscription failed");
    check(script_view.updates(1269).values.empty(), "transmission selector expired early");
    const auto expired = script_view.updates(1270);
    check(expired.values.size() == 2 && expired.values.at(30) == V{} &&
              expired.values.at(31) == V{std::int64_t(104)},
          "script group resend renewed the selector's acquisition date");
    grouped.source->withdraw(31, 1272);
    const auto withdrawn = script_view.updates(1272);
    check(withdrawn.values.size() == 2 && withdrawn.values.at(30) == V{} &&
              withdrawn.values.at(31) == V{} && script_view.updates(1273).values.empty(),
          "script transmission withdrawal retained or repeatedly delivered an old value");
  }
  {
    auto extra = profile(true);
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = {40, 1800, time_ms(1000)};
    data->value.fuel_percent = 67;
    data->value.coolant_c = 90;
    data->value.range_km = 160.9344;
    data->value.odometer_km = 1609.344;
    data->value.ambient_c = -8;
    data->value.indicator(Indicator::check_engine) = IndicatorState::flashing;
    data->value.indicator(Indicator::high_beam) = IndicatorState::off;
    v::GaugeInput extended(extra, std::move(source));
    extended.pump(time_ms(1000));
    const std::array<std::int64_t, 9> extra_ids{20, 21, 22, 23, 24, 25, 26, 27, 28};
    auto values = extra.source->snapshot(extra_ids, 1000);
    check(std::abs(std::get<double>(values.at(20).value->data) - 0.67) < 1e-9,
          "fuel conversion did not reach script input");
    check(values.at(21).value == V{194.} && values.at(22).value == V{std::int64_t(5)} &&
              values.at(23).value == V{false},
          "temperature or explicit indicator mapping failed");
    check(values.at(24).value == V{160.9344} && values.at(26).value == V{1609.344} &&
              values.at(28).value == V{-8.},
          "range, odometer or negative ambient reading lost");
    check(std::abs(std::get<double>(values.at(25).value->data) - 100) < 1e-9 &&
              std::abs(std::get<double>(values.at(27).value->data) - 1000) < 1e-9,
          "kilometre/mile inputs disagreed");
    extended.pump(time_ms(1250));
    values = extra.source->snapshot(extra_ids, 1250);
    for (const auto id : extra_ids)
      check(values.at(id).value == V{}, "extended script value did not expire");
    data->value = {40, 1800, time_ms(1300)};
    extended.pump(time_ms(1300));
    values = extra.source->snapshot(extra_ids, 1300);
    for (const auto id : extra_ids)
      check(values.at(id).value == V{}, "unavailable vehicle data became valid");
    data->value.sampled_at = time_ms(1400);
    data->value.fuel_percent = 101;
    rejects([&] { extended.pump(time_ms(1400)); });
  }
  {
    auto configured = profile(false, true);
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value.sampled_at = time_ms(1000);
    data->value.transmission =
        TransmissionState{GearboxType::automatic, TransmissionSelector::drive,
                          EngagedGear{EngagedGearKind::forward, 4}};
    v::GaugeInput bridge(configured, std::move(source));
    bridge.pump(time_ms(1000));
    const std::array<std::int64_t, 3> transmission_ids{30, 31, 32};
    auto values = configured.source->snapshot(transmission_ids, 1000);
    check(values.at(30).value == V{std::int64_t(68)} &&
              values.at(31).value == V{std::int64_t(104)} &&
              values.at(32).value == V{std::string("AUTO")},
          "explicit transmission scalars not mapped independently");
    data->value.sampled_at = time_ms(1010);
    data->value.transmission = TransmissionState{GearboxType::manual, {}, {}};
    bridge.pump(time_ms(1010));
    values = configured.source->snapshot(transmission_ids, 1010);
    check(values.at(30).value == V{} && values.at(31).value == V{} &&
              values.at(32).value == V{std::string("MAN")},
          "gearbox type inferred a selector or engaged gear");
    data->value.sampled_at = time_ms(1020);
    data->value.transmission = TransmissionState{
        {}, TransmissionSelector::drive, EngagedGear{EngagedGearKind::forward, 5}};
    bridge.pump(time_ms(1020));
    values = configured.source->snapshot(transmission_ids, 1020);
    check(values.at(30).value == V{std::int64_t(68)} && values.at(31).value == V{} &&
              values.at(32).value == V{},
          "unmapped or unknown transmission component gained a default value");
    data->value.sampled_at = time_ms(1030);
    data->value.transmission.reset();
    bridge.pump(time_ms(1030));
    for (const auto& [id, value] : configured.source->snapshot(transmission_ids, 1030))
      check(value.value == V{}, "missing transmission retained a script scalar");
  }
  {
    auto configured = profile(false, true);
    VehicleAgePolicy ages;
    ages.transmission = std::chrono::milliseconds(100);
    VehicleObservationCache cache(ages);
    cache.begin_generation(1, time_ms(1000), time_ms(1000));
    const TransmissionState initial{GearboxType::automatic, TransmissionSelector::drive,
                                    EngagedGear{EngagedGearKind::forward, 4}};
    const std::array values{VehicleObservation{"speed_kmh", 40., time_ms(1000)},
                            VehicleObservation{"transmission", initial, time_ms(1000)}};
    cache.accept(1, 1, values, time_ms(1000));
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = cache.sample(time_ms(1000));
    v::GaugeInput bridge(configured, std::move(source));
    bridge.pump(time_ms(1001));
    const std::array<std::int64_t, 4> transmission_ids{4, 30, 31, 32};
    for (unsigned component = 0; component < 3; ++component) {
      data->value = cache.sample(time_ms(1001));
      if (component == 0)
        data->value.transmission->selector = TransmissionSelector::reverse;
      else if (component == 1)
        data->value.transmission->engaged_gear = EngagedGear{EngagedGearKind::forward, 5};
      else
        data->value.transmission->gearbox_type = GearboxType::manual;
      rejects([&] { bridge.pump(time_ms(1002)); });
      const auto snapshot = configured.source->snapshot(transmission_ids, 1002);
      check(snapshot.at(30).value == V{std::int64_t(68)} &&
                snapshot.at(31).value == V{std::int64_t(104)} &&
                bridge.sample().transmission == initial,
            "same-acquisition transmission conflict partially published");
    }
    const TransmissionState partial{GearboxType::manual, TransmissionSelector::manual, {}};
    const std::array update{VehicleObservation{"transmission", partial, time_ms(1050)}};
    cache.accept(1, 2, update, time_ms(1050));
    data->value = cache.sample(time_ms(1050));
    bridge.pump(time_ms(1051));
    auto snapshot = configured.source->snapshot(transmission_ids, 1051);
    check(snapshot.at(30).value == V{std::int64_t(77)} && snapshot.at(31).value == V{} &&
              snapshot.at(32).value == V{std::string("MAN")},
          "partial coherent transmission did not withdraw unknown gear");
    data->value = cache.sample(time_ms(1051));
    data->value.observation_timing->transmission.maximum_age = std::chrono::milliseconds(101);
    rejects([&] { bridge.pump(time_ms(1052)); });
    data->value = cache.sample(time_ms(1052));
    data->value.observation_timing->transmission.acquired_at.reset();
    rejects([&] { bridge.pump(time_ms(1052)); });
    data->value = cache.sample(time_ms(1149));
    bridge.pump(time_ms(1149));
    check(configured.source->snapshot(transmission_ids, 1149).at(30).value == V{std::int64_t(77)},
          "transmission expired before own age policy");
    data->value = cache.sample(time_ms(1150));
    bridge.pump(time_ms(1150));
    snapshot = configured.source->snapshot(transmission_ids, 1150);
    check(snapshot.at(4).value == V{40.} && snapshot.at(30).value == V{} &&
              snapshot.at(31).value == V{} && snapshot.at(32).value == V{},
          "transmission expiry renewed components or expired unrelated speed");
    check(bridge.sample().observation_timing->transmission.acquired_at == time_ms(1050),
          "transmission expiry lost its acquisition watermark");
  }
  {
    auto configured = profile(false, false, true);
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value.sampled_at = time_ms(1000);
    data->value.drive_mode_catalog = parse_drive_mode_catalog_key("comfort,sport,track");
    v::GaugeInput bridge(configured, std::move(source));
    const std::array<std::int64_t, 2> mode_ids{41, 42};
    bridge.pump(time_ms(1000));
    check(configured.source->snapshot(mode_ids, 1000).at(41).value == V{},
          "configured mode catalogue selected zero before observation");
    data->value.sampled_at = time_ms(1010);
    data->value.drive_mode = parse_drive_mode_identifier("comfort");
    bridge.pump(time_ms(1010));
    auto values = configured.source->snapshot(mode_ids, 1010);
    check(values.at(41).value == V{std::int64_t(1)} &&
              values.at(42).value == V{std::string("COMFORT")},
          "source catalogue order was treated as signed artwork order");
    data->value.sampled_at = time_ms(1020);
    data->value.drive_mode = parse_drive_mode_identifier("sport");
    bridge.pump(time_ms(1020));
    check(configured.source->snapshot(mode_ids, 1020).at(41).value == V{std::int64_t(0)},
          "explicit real zero mode mapping lost");
    data->value.sampled_at = time_ms(1030);
    data->value.drive_mode = parse_drive_mode_identifier("track");
    bridge.pump(time_ms(1030));
    values = configured.source->snapshot(mode_ids, 1030);
    check(values.at(41).value == V{} && values.at(42).value == V{},
          "valid unmapped source mode inherited zero/Sport");
    data->value.sampled_at = time_ms(1040);
    data->value.drive_mode = parse_drive_mode_identifier("wet");
    rejects([&] { bridge.pump(time_ms(1040)); });
    check(bridge.sample().drive_mode == parse_drive_mode_identifier("track"),
          "invalid mode membership mutated accepted source");
    data->value.drive_mode.reset();
    data->value.drive_mode_catalog = parse_drive_mode_catalog_key("sport,comfort,track");
    rejects([&] { bridge.pump(time_ms(1040)); });
    data->value.drive_mode_catalog.reset();
    rejects([&] { bridge.pump(time_ms(1040)); });
  }
  {
    auto configured = profile(false, false, true);
    VehicleAgePolicy ages;
    ages.drive_mode = std::chrono::milliseconds(100);
    VehicleObservationCache cache(ages, parse_drive_mode_catalog_key("comfort,sport,track"));
    cache.begin_generation(1, time_ms(1000), time_ms(1000));
    const std::array initial{
        VehicleObservation{"speed_kmh", 40., time_ms(1000)},
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("comfort"), time_ms(1000)}};
    cache.accept(1, 1, initial, time_ms(1000));
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = cache.sample(time_ms(1000));
    v::GaugeInput bridge(configured, std::move(source));
    bridge.pump(time_ms(1001));
    const std::array<std::int64_t, 3> mode_ids{4, 41, 42};
    auto conflict = cache.sample(time_ms(1001));
    conflict.drive_mode = parse_drive_mode_identifier("sport");
    conflict.speed_kmh = 50.;
    conflict.observation_timing->readings[0].acquired_at = time_ms(1001);
    data->value = conflict;
    rejects([&] { bridge.pump(time_ms(1002)); });
    auto values = configured.source->snapshot(mode_ids, 1002);
    check(values.at(4).value == V{40.} && values.at(41).value == V{std::int64_t(1)} &&
              bridge.sample().drive_mode == parse_drive_mode_identifier("comfort"),
          "same-date mode conflict partially published speed or mode");
    data->value = cache.sample(time_ms(1002));
    data->value.observation_timing->drive_mode.maximum_age = std::chrono::milliseconds(101);
    rejects([&] { bridge.pump(time_ms(1003)); });
    data->value = cache.sample(time_ms(1099));
    bridge.pump(time_ms(1099));
    check(configured.source->snapshot(mode_ids, 1099).at(41).value == V{std::int64_t(1)},
          "mode expired before original acquisition age");
    data->value = cache.sample(time_ms(1100));
    bridge.pump(time_ms(1100));
    values = configured.source->snapshot(mode_ids, 1100);
    check(values.at(4).value == V{40.} && values.at(41).value == V{} && values.at(42).value == V{},
          "mode polling renewed expiry or expired unrelated speed");
    check(bridge.sample().observation_timing->drive_mode.acquired_at == time_ms(1000),
          "mode expiry replaced acquisition time");
    const std::array sport{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("sport"), time_ms(1110)}};
    cache.accept(1, 2, sport, time_ms(1110));
    data->value = cache.sample(time_ms(1110));
    bridge.pump(time_ms(1111));
    check(configured.source->snapshot(mode_ids, 1111).at(41).value == V{std::int64_t(0)},
          "fresh measured sport could not restore explicit zero");
    const std::array unknown{VehicleObservation{"drive_mode",
                                                {},
                                                time_ms(1120),
                                                VehicleObservationOrigin::measured,
                                                VehicleAvailability::unavailable}};
    cache.accept(1, 3, unknown, time_ms(1120));
    data->value = cache.sample(time_ms(1120));
    bridge.pump(time_ms(1121));
    check(configured.source->snapshot(mode_ids, 1121).at(41).value == V{},
          "explicit unavailable mode retained Sport");
    const std::array unmapped{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("track"), time_ms(1130)}};
    cache.accept(1, 4, unmapped, time_ms(1130));
    data->value = cache.sample(time_ms(1130));
    bridge.pump(time_ms(1131));
    check(configured.source->snapshot(mode_ids, 1131).at(41).value == V{},
          "unmapped source catalogue member became a fallback mode");
    // Generation reset must not permit a new catalogue, including its order.
    cache.begin_generation(2, time_ms(1140), time_ms(1140));
    data->value = cache.sample(time_ms(1140));
    data->value.drive_mode_catalog = parse_drive_mode_catalog_key("sport,comfort,track");
    rejects([&] { bridge.pump(time_ms(1141)); });
    data->value = cache.sample(time_ms(1141));
    data->value.drive_mode_catalog.reset();
    rejects([&] { bridge.pump(time_ms(1141)); });
    data->value = cache.sample(time_ms(1141));
    bridge.pump(time_ms(1141));
    const std::array comfort{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("comfort"), time_ms(1142)}};
    cache.accept(2, 1, comfort, time_ms(1142));
    data->value = cache.sample(time_ms(1142));
    bridge.pump(time_ms(1143));
    check(configured.source->snapshot(mode_ids, 1143).at(41).value == V{std::int64_t(1)},
          "same-catalogue generation did not recover after rejected changes");
  }
  {
    auto configured = profile(false, false, true);
    auto source = std::make_unique<VehicleObservationCache>(
        VehicleAgePolicy{}, parse_drive_mode_catalog_key("comfort,sport"));
    auto* data = source.get();
    data->begin_generation(1, time_ms(1000), time_ms(1000));
    v::GaugeInput bridge(configured, std::move(source));
    const auto first = time_ms(1000) + std::chrono::microseconds(100);
    const std::array initial{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("comfort"), first}};
    data->accept(1, 1, initial, first);
    bridge.pump(first);
    const auto second = time_ms(1000) + std::chrono::microseconds(200);
    const std::array next{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("sport"), second}};
    data->accept(1, 2, next, second);
    bridge.pump(second);
    const std::array<std::int64_t, 2> mode_ids{41, 42};
    auto values = configured.source->snapshot(mode_ids, 1000);
    check(values.at(41).value == V{} && values.at(42).value == V{},
          "same-millisecond mode acquisitions fabricated ordering");
    bridge.pump(time_ms(1000) + std::chrono::microseconds(900));
    check(configured.source->snapshot(mode_ids, 1000).at(41).value == V{},
          "poll revived coarsened mode observation");
    const std::array recovery{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("sport"), time_ms(1001)}};
    data->accept(1, 3, recovery, time_ms(1001));
    bridge.pump(time_ms(1001));
    check(configured.source->snapshot(mode_ids, 1001).at(41).value == V{std::int64_t(0)},
          "next distinct millisecond did not recover mode");
  }
  {
    auto configured = profile(false, false, true);
    VehicleObservationCache cache({}, parse_drive_mode_catalog_key("comfort,sport"));
    cache.begin_generation(1, time_ms(1000), time_ms(1000));
    const std::array initial{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("comfort"), time_ms(1000)}};
    cache.accept(1, 1, initial, time_ms(1000));
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = cache.sample(time_ms(1000));
    const auto original = data->value;
    v::GaugeInput bridge(configured, std::move(source));
    bridge.pump(time_ms(1000));
    cache.withdraw(time_ms(1010));
    data->value = cache.sample(time_ms(1010));
    bridge.pump(time_ms(1010));
    data->value = original;
    data->value.sampled_at = time_ms(1011);
    rejects([&] { bridge.pump(time_ms(1011)); });
    const std::array<std::int64_t, 1> mode_ids{41};
    check(configured.source->snapshot(mode_ids, 1011).at(41).value == V{} &&
              !bridge.sample().drive_mode,
          "same-date lost mode revived");
  }
  {
    auto configured = profile(false, false, true);
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value.sampled_at = time_ms(1000);
    v::GaugeInput bridge(configured, std::move(source));
    bridge.pump(time_ms(1000));
    data->value.sampled_at = time_ms(1001);
    data->value.drive_mode_catalog = parse_drive_mode_catalog_key("comfort,sport");
    rejects([&] { bridge.pump(time_ms(1001)); });
    check(!bridge.sample().drive_mode_catalog,
          "catalogue absence was not pinned after a real legacy source publication");
    auto direct = profile(false, false, true);
    direct.vehicle_inputs[0].semantic_states["comfort"] = V{std::int64_t(2)};
    rejects([&] { v::GaugeInput bad(direct, std::make_unique<Input>()); });
  }
  {
    auto configured = profile(false, false, false, true);
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value.sampled_at = time_ms(1000);
    v::GaugeInput bridge(configured, std::move(source));
    bridge.pump(time_ms(1000));
    const std::array<std::int64_t, 3> trip_ids{50, 51, 52};
    auto values = configured.source->snapshot(trip_ids, 1000);
    check(values.at(50).value == V{std::string("Current Trip")} && values.at(51).value == V{} &&
              values.at(52).value == V{},
          "immutable trip label fabricated a mean consumption");
    data->value.sampled_at = time_ms(1010);
    data->value.trip = trip_state(1, 10, 1000, 6.5);
    bridge.pump(time_ms(1010));
    values = configured.source->snapshot(trip_ids, 1010);
    check(values.at(51).value == V{6500.} && values.at(52).value == V{std::int64_t(6500)},
          "canonical trip mean did not use explicit mL/100km scale");
    bridge.pump(time_ms(1260));
    check(configured.source->snapshot(trip_ids, 1260).at(51).value == V{},
          "trip polling renewed a historical mean");
    data->value.sampled_at = time_ms(1270);
    data->value.trip = trip_state(1, 9, 1100, 6.0);
    rejects([&] { bridge.pump(time_ms(1270)); });
    check(bridge.sample().trip->distance_km == 10, "trip counter decrease changed accepted sample");
    // A partial observation and then complete absence must retain hidden counter history.
    data->value.trip = trip_state(1, 10, 1100, {});
    data->value.trip->distance_km.reset();
    bridge.pump(time_ms(1270));
    check(configured.source->snapshot(trip_ids, 1270).at(51).value == V{},
          "undefined trip mean retained old consumption");
    data->value.sampled_at = time_ms(1280);
    data->value.trip.reset();
    bridge.pump(time_ms(1280));
    data->value.sampled_at = time_ms(1290);
    data->value.trip = trip_state(1, 9, 1200, 6.0);
    rejects([&] { bridge.pump(time_ms(1290)); });
    data->value.trip = trip_state(2, 0, 0, 0.);
    rejects([&] { bridge.pump(time_ms(1290)); });  // Undefined zero denominator.
    data->value.trip = trip_state(2, 0, 0, {});
    bridge.pump(time_ms(1290));
    check(configured.source->snapshot(trip_ids, 1290).at(51).value == V{},
          "genuine reset inferred a zero mean");
    data->value.sampled_at = time_ms(1300);
    data->value.trip = trip_state(2, 1, 100, 0.);
    bridge.pump(time_ms(1300));
    check(configured.source->snapshot(trip_ids, 1300).at(51).value == V{0.},
          "known zero mean over positive distance lost");
    data->value.trip = trip_state(3, 0, 0, {});
    rejects([&] { bridge.pump(time_ms(1300)); });
    data->value.sampled_at = time_ms(1310);
    data->value.trip = trip_state(2, 2, 200, 1.23456);
    bridge.pump(time_ms(1310));
    values = configured.source->snapshot(trip_ids, 1310);
    check(values.at(51).value &&
              std::abs(std::get<double>(values.at(51).value->data) - 1234.56) < 1e-9 &&
              values.at(52).value == V{},
          "integer trip conversion silently truncated its mean");
    data->value.sampled_at = time_ms(1320);
    data->value.trip->identifier = *parse_trip_identifier("other");
    rejects([&] { bridge.pump(time_ms(1320)); });
    auto direct = profile(false, false, false, true);
    direct.vehicle_inputs[0].scale = 0;
    rejects([&] { v::GaugeInput bad(direct, std::make_unique<Input>()); });
  }
  {
    auto configured = profile(false, false, false, true);
    VehicleObservationCache cache;
    cache.begin_generation(1, time_ms(1000), time_ms(1000));
    const std::array initial{VehicleObservation{"speed_kmh", 40., time_ms(1000)},
                             VehicleObservation{"trip", trip_state(7, 10, 1000, 6.5), time_ms(1000),
                                                VehicleObservationOrigin::calculated}};
    cache.accept(1, 1, initial, time_ms(1000));
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = cache.sample(time_ms(1000));
    const auto original = data->value;
    v::GaugeInput bridge(configured, std::move(source));
    bridge.pump(time_ms(1000));
    const std::array<std::int64_t, 2> trip_ids{4, 51};
    data->value = original;
    data->value.sampled_at = time_ms(1010);
    data->value.speed_kmh = 50;
    data->value.observation_timing->readings[0].acquired_at = time_ms(1010);
    data->value.trip->fuel_efficiency_l_per_100km = 6.0;
    rejects([&] { bridge.pump(time_ms(1010)); });
    auto values = configured.source->snapshot(trip_ids, 1010);
    check(values.at(4).value == V{40.} && values.at(51).value == V{6500.},
          "same-acquisition trip conflict partially changed another reading");
    cache.withdraw(time_ms(1020));
    data->value = cache.sample(time_ms(1020));
    bridge.pump(time_ms(1020));
    data->value = original;
    data->value.sampled_at = time_ms(1021);
    rejects([&] { bridge.pump(time_ms(1021)); });
    check(configured.source->snapshot(trip_ids, 1021).at(51).value == V{},
          "lost trip revived at the same acquisition");
    cache.begin_generation(2, time_ms(1030), time_ms(1030));
    data->value = cache.sample(time_ms(1030));
    bridge.pump(time_ms(1030));
    const auto candidate = [&](TripState trip, Clock::time_point acquired) {
      data->value = cache.sample(time_ms(1040));
      data->value.trip = std::move(trip);
      data->value.sampled_at = time_ms(1040);
      data->value.observation_timing->trip.acquired_at = acquired;
      data->value.observation_timing->trip.availability = VehicleAvailability::available;
      data->value.observation_timing->trip.origin = VehicleObservationOrigin::calculated;
    };
    candidate(trip_state(7, 9, 1200, 6.0), time_ms(1040));
    rejects([&] { bridge.pump(time_ms(1040)); });
    candidate(trip_state(6, 0, 0, {}), time_ms(1040));
    rejects([&] { bridge.pump(time_ms(1040)); });
    candidate(trip_state(8, 0, 0, {}), time_ms(1040));
    bridge.pump(time_ms(1040));
    check(bridge.sample().trip->reset_generation == 8 &&
              configured.source->snapshot(trip_ids, 1040).at(51).value == V{},
          "fresh reset after source generation could not decrease counters without inventing "
          "consumption");
    candidate(trip_state(9, 0, 0, {}), time_ms(1040));
    rejects([&] { bridge.pump(time_ms(1040)); });
    data->value = bridge.sample();
    data->value.sampled_at = time_ms(1041);
    data->value.trip = trip_state(9, 0, 0, {});
    data->value.observation_timing->trip.acquired_at = time_ms(1039);
    rejects([&] { bridge.pump(time_ms(1041)); });
    check(bridge.sample().trip->reset_generation == 8,
          "conflicting/older trip reset mutated lifetime history");
  }
  {
    auto configured = profile(false, false, false, true);
    VehicleObservationCache cache;
    cache.begin_generation(1, time_ms(1000), time_ms(1000));
    const auto known = trip_state(1, 10, 1000, 6.5);
    const std::array initial{
        VehicleObservation{"trip", known, time_ms(1000), VehicleObservationOrigin::calculated}};
    cache.accept(1, 1, initial, time_ms(1000));
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = cache.sample(time_ms(1000));
    v::GaugeInput bridge(configured, std::move(source));
    bridge.pump(time_ms(1000));
    // Restart at the original publication boundary, so a replay is statelessly
    // fresh/coherent; lifetime loss history must still forbid its revival.
    cache.begin_generation(2, time_ms(1000), time_ms(1001));
    data->value = cache.sample(time_ms(1001));
    bridge.pump(time_ms(1001));
    const auto observation = [&](TripState trip, Clock::time_point acquired,
                                 Clock::time_point published) {
      data->value = cache.sample(published);
      data->value.trip = std::move(trip);
      data->value.observation_timing->trip.acquired_at = acquired;
      data->value.observation_timing->trip.availability = VehicleAvailability::available;
      data->value.observation_timing->trip.origin = VehicleObservationOrigin::calculated;
    };
    observation(known, time_ms(1000), time_ms(1002));
    rejects([&] { bridge.pump(time_ms(1002)); });
    const std::array<std::int64_t, 1> trip_ids{51};
    check(configured.source->snapshot(trip_ids, 1002).at(51).value == V{} && !bridge.sample().trip,
          "new-generation empty trip allowed an identical old acquired value to revive");
    observation(known, time_ms(1003), time_ms(1003));
    bridge.pump(time_ms(1003));
    check(configured.source->snapshot(trip_ids, 1003).at(51).value == V{6500.},
          "strictly newer acquisition did not recover lost trip");
    bridge.pump(time_ms(1503));
    bridge.pump(time_ms(1504));
    check(configured.source->snapshot(trip_ids, 1504).at(51).value == V{},
          "unchanged expired raw snapshot revived or threw while polling");
    data->value.sampled_at = time_ms(1004);
    data->value.trip = trip_state(1, 11, 1100, 6.0);
    data->value.observation_timing->trip.acquired_at = time_ms(1004);
    bridge.pump(time_ms(1505));
    check(configured.source->snapshot(trip_ids, 1505).at(51).value == V{},
          "new but expired historical trip became fresh display data");
    observation(known, time_ms(1506), time_ms(1506));
    rejects([&] { bridge.pump(time_ms(1506)); });
    observation(trip_state(1, 12, 1200, 6.0), time_ms(1507), time_ms(1507));
    bridge.pump(time_ms(1507));
    check(configured.source->snapshot(trip_ids, 1507).at(51).value == V{6000.},
          "valid recovery lost hidden historical trip counters");
  }
  {
    // Different sensor rates must stay different through the real script
    // service, including expiry without a new socket packet or source read.
    auto configured = profile(true);
    VehicleAgePolicy ages;
    ages.readings[3] = std::chrono::milliseconds(100);
    ages.indicators[static_cast<std::size_t>(Indicator::check_engine)] =
        std::chrono::milliseconds(100);
    auto source = std::make_unique<VehicleObservationCache>(ages);
    auto* data = source.get();
    data->begin_generation(1, time_ms(1000), time_ms(1000));
    const std::array initial{VehicleObservation{"speed_kmh", 40., time_ms(1000)},
                             VehicleObservation{"coolant_c", 90., time_ms(1000)},
                             VehicleObservation{"check_engine", IndicatorState::on, time_ms(1000)}};
    data->accept(1, 1, initial, time_ms(1000));
    v::GaugeInput observed(configured, std::move(source));
    observed.pump(time_ms(1000));
    const std::array<std::int64_t, 3> read_ids{4, 21, 22};
    const std::array faster{VehicleObservation{"speed_kmh", 50., time_ms(1099)}};
    data->accept(1, 2, faster, time_ms(1099));
    observed.pump(time_ms(1099));
    auto values = configured.source->snapshot(read_ids, 1099);
    check(values.at(21).value == V{194.} && values.at(22).value == V{std::int64_t(3)},
          "observation expired early");
    observed.pump(time_ms(1100));
    values = configured.source->snapshot(read_ids, 1100);
    check(values.at(4).value == V{50.} && values.at(21).value == V{} && values.at(22).value == V{},
          "fresh speed renewed old temperature or lamp");
    check(observed.sample().observation_timing->readings[3].acquired_at == time_ms(1000),
          "script bridge renewed acquisition time");
    data->withdraw(time_ms(1120));
    observed.pump(time_ms(1120));
    check(configured.source->snapshot(read_ids, 1120).at(4).value == V{},
          "provider loss did not withdraw speed");
    data->accept(1, 3, faster, time_ms(1121));
    observed.pump(time_ms(1121));
    check(configured.source->snapshot(read_ids, 1121).at(4).value == V{},
          "same acquisition revived disconnected speed");
    const std::array recovery{VehicleObservation{"speed_kmh", 55., time_ms(1110)}};
    data->accept(1, 4, recovery, time_ms(1122));
    observed.pump(time_ms(1122));
    check(configured.source->snapshot(read_ids, 1122).at(4).value == V{55.},
          "fresh delayed acquisition could not recover");
    observed.pump(time_ms(1360));
    check(configured.source->snapshot(read_ids, 1360).at(4).value == V{},
          "delayed acquisition was dated at delivery");
    data->begin_generation(2, time_ms(1400), time_ms(1400));
    observed.pump(time_ms(1400));
    const std::array zero{VehicleObservation{"speed_kmh", 0., time_ms(1401)}};
    data->accept(2, 1, zero, time_ms(1401));
    observed.pump(time_ms(1401));
    check(configured.source->snapshot(read_ids, 1401).at(4).value == V{0.},
          "new generation lost real zero speed");
  }
  {
    // The bridge also checks snapshots from arbitrary VehicleSource adapters;
    // malformed/replayed metadata cannot partially overwrite a healthy scene.
    VehicleObservationCache cache;
    cache.begin_generation(1, time_ms(1000), time_ms(1000));
    const std::array values{VehicleObservation{"speed_kmh", 40., time_ms(1000)},
                            VehicleObservation{"engine_rpm", 1200., time_ms(1000)}};
    cache.accept(1, 1, values, time_ms(1000));
    auto configured = profile();
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = cache.sample(time_ms(1000));
    const auto original = data->value;
    v::GaugeInput observed(configured, std::move(source));
    observed.pump(time_ms(1000));
    data->value.speed_kmh = 41;
    rejects([&] { observed.pump(time_ms(1001)); });
    check(configured.source->snapshot(ids, 1001).at(4).value == V{40.},
          "conflicting equal acquisition changed speed");
    data->value = original;
    data->value.sampled_at = time_ms(1100);
    data->value.speed_kmh = 50;
    data->value.observation_timing->readings[0].acquired_at = time_ms(1100);
    data->value.observation_timing->readings[1].maximum_age = std::chrono::milliseconds(501);
    rejects([&] { observed.pump(time_ms(1100)); });
    check(configured.source->snapshot(ids, 1100).at(4).value == V{40.},
          "invalid later metadata partially published speed");
    data->value = original;
    data->value.sampled_at = time_ms(1100);
    data->value.observation_timing.reset();
    rejects([&] { observed.pump(time_ms(1100)); });
    data->value = original;
    observed.pump(time_ms(1100));
    data->value.observation_timing->generation = 2;
    data->value.observation_timing->started_at = time_ms(999);
    data->value.sampled_at = time_ms(1101);
    data->value.observation_timing->readings[0].acquired_at = time_ms(1101);
    data->value.observation_timing->readings[1].acquired_at = time_ms(1101);
    rejects([&] { observed.pump(time_ms(1101)); });
  }
  {
    VehicleObservationCache cache;
    cache.begin_generation(1, time_ms(1000), time_ms(1000));
    const std::array values{VehicleObservation{"speed_kmh", 40., time_ms(1000)}};
    cache.accept(1, 1, values, time_ms(1000));
    auto configured = profile();
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = cache.sample(time_ms(1000));
    const auto original = data->value;
    v::GaugeInput observed(configured, std::move(source));
    observed.pump(time_ms(1000));
    data->value.sampled_at = time_ms(1100);
    data->value.speed_kmh.reset();
    data->value.observation_timing->readings[0].availability = VehicleAvailability::disconnected;
    observed.pump(time_ms(1100));
    data->value = original;
    data->value.sampled_at = time_ms(1101);
    rejects([&] { observed.pump(time_ms(1101)); });
    check(!observed.sample().speed_kmh && configured.source->snapshot(ids, 1101).at(4).value == V{},
          "equal-date source revival escaped into shared native sample");
  }
  {
    auto configured = profile();
    auto source = std::make_unique<VehicleObservationCache>();
    auto* data = source.get();
    data->begin_generation(1, time_ms(1000), time_ms(1000));
    v::GaugeInput observed(configured, std::move(source));
    const auto first = time_ms(1000) + std::chrono::microseconds(100);
    const std::array initial{VehicleObservation{"speed_kmh", 40., first}};
    data->accept(1, 1, initial, first);
    observed.pump(first);
    const auto second = time_ms(1000) + std::chrono::microseconds(200);
    const std::array next{VehicleObservation{"speed_kmh", 41., second}};
    data->accept(1, 2, next, second);
    observed.pump(second);
    check(configured.source->snapshot(ids, 1000).at(4).value == V{},
          "sub-millisecond update fabricated a service timestamp");
    observed.pump(time_ms(1000) + std::chrono::microseconds(900));
    check(configured.source->snapshot(ids, 1000).at(4).value == V{},
          "poll revived a coarsened observation");
    const std::array recovery{VehicleObservation{"speed_kmh", 42., time_ms(1001)}};
    data->accept(1, 3, recovery, time_ms(1001));
    observed.pump(time_ms(1001));
    check(configured.source->snapshot(ids, 1001).at(4).value == V{42.},
          "distinct millisecond could not recover");
  }
  {
    VehicleObservationCache cache;
    cache.begin_generation(1, time_ms(1000), time_ms(1000));
    const std::array values{VehicleObservation{"speed_kmh", 40., time_ms(1000)}};
    cache.accept(1, 1, values, time_ms(1000));
    auto configured = profile();
    auto source = std::make_unique<Input>();
    auto* data = source.get();
    data->value = cache.sample(time_ms(1000));
    data->value.sampled_at = time_ms(1000) + std::chrono::microseconds(200);
    v::GaugeInput observed(configured, std::move(source));
    observed.pump(data->value.sampled_at);
    data->value.sampled_at = time_ms(1000) + std::chrono::microseconds(100);
    rejects([&] { observed.pump(time_ms(1001)); });
    check(observed.sample().sampled_at == time_ms(1000) + std::chrono::microseconds(200),
          "sub-ms reversed publication changed shared sample");
  }
#ifdef HEADUNIT_TEST_GAUGE_SOCKET
  char directory[] = "/tmp/headunit-gauge-XXXXXX";
  check(mkdtemp(directory), "socket directory");
  const std::string path = std::filesystem::canonical(directory).string() + "/vehicle.sock";
  {
    auto local = profile();
    v::GaugeInput socket_input(local, std::make_unique<SocketVehicle>(path));
    const int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    check(sender >= 0 && fcntl(sender, F_SETFD, FD_CLOEXEC) == 0, "socket sender");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const char packet[] = "HUV1 57.4 2400";
    check(sendto(sender, packet, sizeof(packet) - 1, 0, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == sizeof(packet) - 1,
          "socket send");
    socket_input.pump(time_ms(1000));
    auto state = local.source->snapshot(ids, 1000);
    check(state.at(4).value == V{57.4} && state.at(6).value == V{2400.},
          "socket did not reach theme values");
    socket_input.pump(time_ms(1250));
    check(local.source->snapshot(ids, 1250).at(4).value == V{}, "socket polling renewed old data");
    close(sender);
  }
  check(access(path.c_str(), F_OK) != 0, "gauge source retained its bound endpoint");
  // The stable advisory-lock inode remains for subsequent process restarts.
  // Remove it only when dismantling this private test directory.
  check(unlink((path + ".lock").c_str()) == 0, "gauge lock fixture cleanup");
  check(rmdir(directory) == 0, "socket cleanup");
  char observations_directory[] = "/tmp/headunit-gauge-observations-XXXXXX";
  check(mkdtemp(observations_directory), "observation socket directory");
  const std::string observed_path =
      std::filesystem::canonical(observations_directory).string() + "/vehicle.sock";
  {
    auto local = profile(true);
    v::GaugeInput socket_input(local, std::make_unique<SocketVehicle>(observed_path));
    socket_input.pump(time_ms(1000));  // No packet yet: must not select legacy mode.
    const int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    check(sender >= 0 && fcntl(sender, F_SETFD, FD_CLOEXEC) == 0, "observation sender");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, observed_path.c_str(), observed_path.size() + 1);
    const auto send = [&](const VehicleObservationPacket& packet) {
      const auto bytes = encode_vehicle_observation_datagram(packet);
      check(sendto(sender, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == ssize_t(bytes.size()),
            "observation send");
    };
    VehicleAgePolicy ages;
    ages.readings[3] = std::chrono::milliseconds(100);
    VehicleObservationCache producer(ages);
    producer.begin_generation(1, time_ms(1001), time_ms(1001));
    const std::array initial{VehicleObservation{"speed_kmh", 40., time_ms(1001)},
                             VehicleObservation{"coolant_c", 90., time_ms(1001)}};
    producer.accept(1, 1, initial, time_ms(1001));
    send({producer.sample(time_ms(1001)), 1});
    socket_input.pump(time_ms(1002));
    const std::array<std::int64_t, 2> observed_ids{4, 21};
    auto state = local.source->snapshot(observed_ids, 1002);
    check(state.at(4).value == V{40.} && state.at(21).value == V{194.},
          "observation wire did not reach actual script service");
    const std::array faster{VehicleObservation{"speed_kmh", 50., time_ms(1099)}};
    producer.accept(1, 2, faster, time_ms(1099));
    send({producer.sample(time_ms(1099)), 2});
    socket_input.pump(time_ms(1101));
    state = local.source->snapshot(observed_ids, 1101);
    check(state.at(4).value == V{50.} && state.at(21).value == V{},
          "socket-to-script bridge renewed old temperature");
    socket_input.pump(time_ms(1200));  // Poll after the next generation has begun.
    producer.begin_generation(2, time_ms(1150), time_ms(1151));
    const std::array zero{VehicleObservation{"speed_kmh", 0., time_ms(1151)}};
    producer.accept(2, 1, zero, time_ms(1151));
    send({producer.sample(time_ms(1151)), 1});
    socket_input.pump(time_ms(1201));
    state = local.source->snapshot(observed_ids, 1201);
    check(state.at(4).value == V{0.} && socket_input.sample().observation_timing->generation == 2,
          "delayed restart was compared against local poll rather than source publication");
    check(socket_input.sample().observation_timing->readings[0].acquired_at == time_ms(1151),
          "script source lost transmitted acquisition date");
    close(sender);
  }
  check(unlink((observed_path + ".lock").c_str()) == 0 && rmdir(observations_directory) == 0,
        "observation socket cleanup");
  char transmission_directory[] = "/tmp/headunit-gauge-transmission-XXXXXX";
  check(mkdtemp(transmission_directory), "transmission socket directory");
  const std::string transmission_path =
      std::filesystem::canonical(transmission_directory).string() + "/vehicle.sock";
  {
    auto local = profile(false, true);
    v::GaugeInput socket_input(local, std::make_unique<SocketVehicle>(transmission_path));
    socket_input.pump(time_ms(1000));
    const int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    check(sender >= 0 && fcntl(sender, F_SETFD, FD_CLOEXEC) == 0, "transmission socket sender");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, transmission_path.c_str(), transmission_path.size() + 1);
    const auto send = [&](VehicleSample value, std::uint64_t sequence) {
      const auto bytes = encode_vehicle_observation_datagram(
          {std::move(value), sequence, VehicleObservationWireVersion::hvo2});
      check(bytes.starts_with("HVO2 "),
            "transmission test silently encoded the legacy observation version");
      check(sendto(sender, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == ssize_t(bytes.size()),
            "transmission socket send");
    };
    VehicleAgePolicy ages;
    ages.transmission = std::chrono::milliseconds(100);
    VehicleObservationCache producer(ages);
    producer.begin_generation(1, time_ms(1001), time_ms(1001));
    const TransmissionState initial{GearboxType::automatic, TransmissionSelector::drive,
                                    EngagedGear{EngagedGearKind::forward, 4}};
    const std::array values{VehicleObservation{"speed_kmh", 40., time_ms(1001)},
                            VehicleObservation{"transmission", initial, time_ms(1001)}};
    producer.accept(1, 1, values, time_ms(1001));
    send(producer.sample(time_ms(1020)), 1);
    socket_input.pump(time_ms(1021));
    const std::array<std::int64_t, 4> transmission_ids{4, 30, 31, 32};
    auto state = local.source->snapshot(transmission_ids, 1021);
    check(state.at(30).value == V{std::int64_t(68)} && state.at(31).value == V{std::int64_t(104)} &&
              state.at(32).value == V{std::string("AUTO")},
          "HVO2 transmission did not reach the real script service");
    check(socket_input.sample().sampled_at == time_ms(1020) &&
              socket_input.sample().observation_timing->transmission.acquired_at == time_ms(1001),
          "HVO2 source acquisition was replaced by publication/receipt time");
    const TransmissionState partial{GearboxType::manual, TransmissionSelector::manual, {}};
    const std::array update{VehicleObservation{"transmission", partial, time_ms(1050)}};
    producer.accept(1, 2, update, time_ms(1050));
    send(producer.sample(time_ms(1050)), 2);
    socket_input.pump(time_ms(1051));
    state = local.source->snapshot(transmission_ids, 1051);
    check(state.at(30).value == V{std::int64_t(77)} && state.at(31).value == V{} &&
              state.at(32).value == V{std::string("MAN")},
          "HVO2 unknown gear inherited a previous value");
    auto conflict = producer.sample(time_ms(1052));
    conflict.transmission->selector = TransmissionSelector::reverse;
    send(conflict, 3);
    socket_input.pump(time_ms(1053));
    state = local.source->snapshot(transmission_ids, 1053);
    check(
        state.at(30).value == V{std::int64_t(77)} && socket_input.sample().transmission == partial,
        "HVO2 equal-acquisition transmission conflict escaped into the script/native sample");
    const TransmissionState neutral{GearboxType::automatic, TransmissionSelector::neutral,
                                    EngagedGear{EngagedGearKind::neutral, 0}};
    const std::array recovery{VehicleObservation{"transmission", neutral, time_ms(1060)}};
    producer.accept(1, 3, recovery, time_ms(1060));
    send(producer.sample(time_ms(1060)), 3);
    socket_input.pump(time_ms(1061));
    state = local.source->snapshot(transmission_ids, 1061);
    check(
        state.at(30).value == V{std::int64_t(78)} && state.at(31).value == V{std::int64_t(201)},
        "HVO2 rejected packet consumed the valid recovery sequence or inferred a neutral encoding");
    socket_input.pump(time_ms(1160));
    state = local.source->snapshot(transmission_ids, 1160);
    check(state.at(4).value == V{40.} && state.at(30).value == V{} && state.at(31).value == V{} &&
              state.at(32).value == V{},
          "HVO2 socket polling refreshed expired transmission components");
    check(socket_input.sample().observation_timing->transmission.acquired_at == time_ms(1060),
          "HVO2 expiration changed original transmission time");
    close(sender);
  }
  check(unlink((transmission_path + ".lock").c_str()) == 0 && rmdir(transmission_directory) == 0,
        "transmission socket cleanup");
  char mode_directory[] = "/tmp/headunit-gauge-drive-mode-XXXXXX";
  check(mkdtemp(mode_directory), "mode socket directory");
  const std::string mode_path =
      std::filesystem::canonical(mode_directory).string() + "/vehicle.sock";
  {
    auto local = profile(false, false, true);
    v::GaugeInput socket_input(local, std::make_unique<SocketVehicle>(mode_path));
    socket_input.pump(time_ms(1000));  // Empty startup must not pin catalogue absence.
    const int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    check(sender >= 0 && fcntl(sender, F_SETFD, FD_CLOEXEC) == 0, "mode socket sender");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, mode_path.c_str(), mode_path.size() + 1);
    const auto send = [&](VehicleSample value, std::uint64_t sequence) {
      const auto bytes = encode_vehicle_observation_datagram(
          {std::move(value), sequence, VehicleObservationWireVersion::hvo3});
      check(bytes.starts_with("HVO3 ") &&
                bytes.find("drive_modes=comfort,sport,track") != std::string::npos,
            "mode fixture did not explicitly encode its immutable HVO3 catalogue");
      check(sendto(sender, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == ssize_t(bytes.size()),
            "mode socket send");
    };
    VehicleAgePolicy ages;
    ages.drive_mode = std::chrono::milliseconds(100);
    VehicleObservationCache producer(ages, parse_drive_mode_catalog_key("comfort,sport,track"));
    producer.begin_generation(1, time_ms(1001), time_ms(1001));
    const std::array initial{
        VehicleObservation{"speed_kmh", 40., time_ms(1001)},
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("comfort"), time_ms(1001)}};
    producer.accept(1, 1, initial, time_ms(1001));
    send(producer.sample(time_ms(1020)), 1);
    socket_input.pump(time_ms(1021));
    const std::array<std::int64_t, 3> mode_ids{4, 41, 42};
    auto state = local.source->snapshot(mode_ids, 1021);
    check(
        state.at(41).value == V{std::int64_t(1)} && state.at(42).value == V{std::string("COMFORT")},
        "HVO3 semantic mode did not reach the real script service in authored order");
    check(socket_input.sample().sampled_at == time_ms(1020) &&
              socket_input.sample().observation_timing->drive_mode.acquired_at == time_ms(1001) &&
              drive_mode_catalog_key(*socket_input.sample().drive_mode_catalog) ==
                  "comfort,sport,track",
          "HVO3 receipt changed original dates or inferred the authored source catalogue");
    const std::array sport{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("sport"), time_ms(1050)}};
    producer.accept(1, 2, sport, time_ms(1050));
    send(producer.sample(time_ms(1050)), 2);
    socket_input.pump(time_ms(1051));
    check(local.source->snapshot(mode_ids, 1051).at(41).value == V{std::int64_t(0)},
          "HVO3 explicit measured zero index lost");
    auto conflict = producer.sample(time_ms(1052));
    conflict.drive_mode = parse_drive_mode_identifier("comfort");
    send(conflict, 3);
    socket_input.pump(time_ms(1053));
    check(local.source->snapshot(mode_ids, 1053).at(41).value == V{std::int64_t(0)} &&
              socket_input.sample().drive_mode == parse_drive_mode_identifier("sport"),
          "HVO3 equal-acquisition mode conflict escaped into script/native state");
    const std::array track{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("track"), time_ms(1060)}};
    producer.accept(1, 3, track, time_ms(1060));
    send(producer.sample(time_ms(1060)), 3);
    socket_input.pump(time_ms(1061));
    state = local.source->snapshot(mode_ids, 1061);
    check(state.at(41).value == V{} && state.at(42).value == V{} &&
              socket_input.sample().drive_mode == parse_drive_mode_identifier("track"),
          "HVO3 invalid packet consumed recovery sequence or unmapped mode inherited Sport");
    const std::array comfort{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("comfort"), time_ms(1070)}};
    producer.accept(1, 4, comfort, time_ms(1070));
    send(producer.sample(time_ms(1070)), 4);
    socket_input.pump(time_ms(1071));
    socket_input.pump(time_ms(1169));
    check(local.source->snapshot(mode_ids, 1169).at(41).value == V{std::int64_t(1)},
          "HVO3 original mode expired early");
    socket_input.pump(time_ms(1170));
    state = local.source->snapshot(mode_ids, 1170);
    check(state.at(4).value == V{40.} && state.at(41).value == V{} && state.at(42).value == V{} &&
              socket_input.sample().observation_timing->drive_mode.acquired_at == time_ms(1070),
          "HVO3 poll renewed mode expiry or acquisition");
    socket_input.pump(time_ms(1200));
    // Source restarts before a later host poll, preserving its own generation date.
    producer.begin_generation(2, time_ms(1150), time_ms(1151));
    const std::array recovery{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("sport"), time_ms(1151)}};
    producer.accept(2, 1, recovery, time_ms(1151));
    send(producer.sample(time_ms(1151)), 1);
    socket_input.pump(time_ms(1201));
    check(local.source->snapshot(mode_ids, 1201).at(41).value == V{std::int64_t(0)} &&
              socket_input.sample().observation_timing->generation == 2 &&
              socket_input.sample().observation_timing->drive_mode.acquired_at == time_ms(1151),
          "HVO3 delayed restart used consumer poll date or altered source catalogue");
    const std::array unknown{VehicleObservation{"drive_mode",
                                                {},
                                                time_ms(1202),
                                                VehicleObservationOrigin::measured,
                                                VehicleAvailability::unavailable}};
    producer.accept(2, 2, unknown, time_ms(1202));
    send(producer.sample(time_ms(1202)), 2);
    socket_input.pump(time_ms(1203));
    check(local.source->snapshot(mode_ids, 1203).at(41).value == V{} &&
              !socket_input.sample().drive_mode,
          "HVO3 unavailable mode retained its previous active selection");
    close(sender);
  }
  check(unlink((mode_path + ".lock").c_str()) == 0 && rmdir(mode_directory) == 0,
        "mode socket cleanup");
  char trip_directory[] = "/tmp/headunit-gauge-trip-XXXXXX";
  check(mkdtemp(trip_directory), "trip socket directory");
  const std::string trip_path =
      std::filesystem::canonical(trip_directory).string() + "/vehicle.sock";
  {
    auto local = profile(false, false, false, true);
    v::GaugeInput socket_input(local, std::make_unique<SocketVehicle>(trip_path));
    socket_input.pump(time_ms(1000));
    const int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    check(sender >= 0 && fcntl(sender, F_SETFD, FD_CLOEXEC) == 0, "trip socket sender");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, trip_path.c_str(), trip_path.size() + 1);
    const auto send = [&](VehicleSample value, std::uint64_t sequence) {
      const auto bytes = encode_vehicle_observation_datagram(
          {std::move(value), sequence, VehicleObservationWireVersion::hvo4});
      check(bytes.starts_with("HVO4 "), "trip fixture silently encoded a historical transport");
      check(sendto(sender, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == ssize_t(bytes.size()),
            "trip socket send");
    };
    VehicleAgePolicy ages;
    ages.trip = std::chrono::milliseconds(100);
    ages.heading = std::chrono::milliseconds(40);
    VehicleObservationCache producer(ages);
    producer.begin_generation(1, time_ms(1001), time_ms(1001));
    const std::array initial{VehicleObservation{"speed_kmh", 40., time_ms(1001)},
                             VehicleObservation{"trip", trip_state(1, 10, 1000, 6.5), time_ms(1001),
                                                VehicleObservationOrigin::calculated},
                             VehicleObservation{"heading",
                                                HeadingState{90., HeadingReference::true_north,
                                                             HeadingSource::vehicle_orientation},
                                                time_ms(1001)}};
    producer.accept(1, 1, initial, time_ms(1001));
    send(producer.sample(time_ms(1020)), 1);
    socket_input.pump(time_ms(1021));
    const std::array<std::int64_t, 3> trip_ids{4, 50, 51};
    auto state = local.source->snapshot(trip_ids, 1021);
    check(state.at(51).value == V{6500.} && socket_input.sample().sampled_at == time_ms(1020) &&
              socket_input.sample().observation_timing->trip.acquired_at == time_ms(1001) &&
              socket_input.sample().observation_timing->heading.acquired_at == time_ms(1001),
          "HVO4 changed original trip/heading dates or lost explicit unit conversion");
    socket_input.pump(time_ms(1040));
    check(socket_input.sample().heading.has_value(), "HVO4 heading expired early");
    socket_input.pump(time_ms(1041));
    check(!socket_input.sample().heading && socket_input.sample().trip &&
              local.source->snapshot(trip_ids, 1041).at(51).value == V{6500.},
          "HVO4 heading expiration cleared independent trip consumption");
    const std::array unknown{VehicleObservation{"trip",
                                                {},
                                                time_ms(1050),
                                                VehicleObservationOrigin::calculated,
                                                VehicleAvailability::unavailable}};
    producer.accept(1, 2, unknown, time_ms(1050));
    send(producer.sample(time_ms(1050)), 2);
    socket_input.pump(time_ms(1051));
    check(local.source->snapshot(trip_ids, 1051).at(51).value == V{} && !socket_input.sample().trip,
          "HVO4 unavailable trip retained mean consumption");
    auto decrease = producer.sample(time_ms(1052));
    decrease.trip = trip_state(1, 9, 1200, 6.0);
    decrease.observation_timing->trip.acquired_at = time_ms(1052);
    decrease.observation_timing->trip.availability = VehicleAvailability::available;
    send(decrease, 3);
    socket_input.pump(time_ms(1053));
    check(!socket_input.sample().trip && local.source->snapshot(trip_ids, 1053).at(51).value == V{},
          "HVO4 missing observation erased prior counter history");
    const std::array reset{VehicleObservation{"trip", trip_state(2, 0, 0, {}), time_ms(1060),
                                              VehicleObservationOrigin::calculated}};
    producer.accept(1, 3, reset, time_ms(1060));
    send(producer.sample(time_ms(1060)), 3);
    socket_input.pump(time_ms(1061));
    check(socket_input.sample().trip->reset_generation == 2 &&
              local.source->snapshot(trip_ids, 1061).at(51).value == V{},
          "HVO4 fresh reset was rejected or fabricated a zero mean");
    auto conflict = producer.sample(time_ms(1062));
    conflict.trip = trip_state(3, 0, 0, {});
    send(conflict, 4);
    socket_input.pump(time_ms(1063));
    check(socket_input.sample().trip->reset_generation == 2,
          "HVO4 conflicting reset advanced lifetime trip history");
    const std::array zero{VehicleObservation{"trip", trip_state(2, 1, 100, 0.), time_ms(1070),
                                             VehicleObservationOrigin::calculated}};
    producer.accept(1, 4, zero, time_ms(1070));
    send(producer.sample(time_ms(1070)), 4);
    socket_input.pump(time_ms(1071));
    check(local.source->snapshot(trip_ids, 1071).at(51).value == V{0.},
          "HVO4 rejected packet consumed sequence or discarded a known zero mean");
    const std::array moving{VehicleObservation{"trip", trip_state(2, 2, 200, 6.0), time_ms(1080),
                                               VehicleObservationOrigin::calculated}};
    producer.accept(1, 5, moving, time_ms(1080));
    send(producer.sample(time_ms(1080)), 5);
    socket_input.pump(time_ms(1081));
    socket_input.pump(time_ms(1179));
    check(local.source->snapshot(trip_ids, 1179).at(51).value == V{6000.},
          "HVO4 trip expired before original compound acquisition age");
    socket_input.pump(time_ms(1180));
    state = local.source->snapshot(trip_ids, 1180);
    check(state.at(4).value == V{40.} && state.at(51).value == V{} && !socket_input.sample().trip &&
              socket_input.sample().observation_timing->trip.acquired_at == time_ms(1080),
          "HVO4 polling renewed trip mean or lost its acquisition watermark");
    socket_input.pump(time_ms(1200));
    producer.begin_generation(2, time_ms(1190), time_ms(1191));
    const std::array recovery{VehicleObservation{"trip", trip_state(2, 3, 300, 6.1), time_ms(1191),
                                                 VehicleObservationOrigin::calculated}};
    producer.accept(2, 1, recovery, time_ms(1191));
    auto restarted = producer.sample(time_ms(1191));
    auto replay = restarted;
    replay.trip = trip_state(1, 0, 0, {});
    send(replay, 1);
    socket_input.pump(time_ms(1201));
    check(!socket_input.sample().trip, "HVO4 source generation forgot older trip reset rejection");
    send(restarted, 1);
    socket_input.pump(time_ms(1202));
    state = local.source->snapshot(trip_ids, 1202);
    check(state.at(51).value == V{6100.} &&
              socket_input.sample().observation_timing->generation == 2 &&
              socket_input.sample().observation_timing->trip.acquired_at == time_ms(1191),
          "HVO4 delayed restart used poll dates or consumed rejected sequence");
    producer.begin_generation(3, time_ms(1191), time_ms(1203));
    send(producer.sample(time_ms(1203)), 1);
    socket_input.pump(time_ms(1204));
    auto replay_after_empty = producer.sample(time_ms(1205));
    replay_after_empty.trip = restarted.trip;
    replay_after_empty.observation_timing->trip.acquired_at = time_ms(1191);
    replay_after_empty.observation_timing->trip.availability = VehicleAvailability::available;
    replay_after_empty.observation_timing->trip.origin = VehicleObservationOrigin::calculated;
    send(replay_after_empty, 2);
    socket_input.pump(time_ms(1206));
    check(!socket_input.sample().trip && local.source->snapshot(trip_ids, 1206).at(51).value == V{},
          "HVO4 new-generation empty snapshot allowed fresh-looking old acquired trip revival");
    const std::array fresh_after_loss{VehicleObservation{
        "trip", trip_state(2, 4, 400, 6.0), time_ms(1207), VehicleObservationOrigin::calculated}};
    producer.accept(3, 1, fresh_after_loss, time_ms(1207));
    send(producer.sample(time_ms(1207)), 2);
    socket_input.pump(time_ms(1208));
    check(local.source->snapshot(trip_ids, 1208).at(51).value == V{6000.},
          "HVO4 genuinely newer trip did not recover after sticky generation loss");
    close(sender);
  }
  check(unlink((trip_path + ".lock").c_str()) == 0 && rmdir(trip_directory) == 0,
        "trip socket cleanup");
#endif
  std::cout << "Gauge source mapping, timestamps, expiry, missing values and simulation passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
