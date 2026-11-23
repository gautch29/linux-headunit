// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_observation.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace headunit;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F&& action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid vehicle observation was accepted");
}
Clock::time_point at(std::int64_t ms) {
  return Clock::time_point{std::chrono::milliseconds(ms)};
}
VehicleObservation number(std::string_view key, double value, std::int64_t time,
                          VehicleObservationOrigin origin = VehicleObservationOrigin::measured) {
  return {key, value, at(time), origin};
}
VehicleObservation lamp(std::string_view key, IndicatorState value, std::int64_t time) {
  return {key, value, at(time)};
}
void accept(VehicleObservationCache& cache, std::uint64_t generation, std::uint64_t sequence,
            std::int64_t now, std::initializer_list<VehicleObservation> values) {
  cache.accept(generation, sequence, {values.begin(), values.size()}, at(now));
}
const VehicleSignalTiming& timing(const VehicleSample& sample, std::string_view key) {
  if (key == "transmission")
    return vehicle_signal_timing(*sample.observation_timing, transmission_observation_index);
  if (key == "drive_mode")
    return vehicle_signal_timing(*sample.observation_timing, drive_mode_observation_index);
  if (key == "trip")
    return vehicle_signal_timing(*sample.observation_timing, trip_observation_index);
  if (key == "heading")
    return vehicle_signal_timing(*sample.observation_timing, heading_observation_index);
  if (const auto* reading = find_vehicle_reading(key))
    return sample.observation_timing->readings[std::size_t(reading - reading_definitions.data())];
  return sample.observation_timing->indicators[std::size_t(find_vehicle_indicator(key)->id)];
}
void legacy() {
  VehicleSample sample{0.0, 0.0, at(1000)};  // Original aggregate source remains valid.
  sample.indicator(Indicator::low_beam) = IndicatorState::off;
  check(!sample.observation_timing && valid_vehicle_sample(sample), "legacy aggregate changed");
  check(fresh(sample, at(1500), 500ms), "legacy inclusive expiry changed");
  auto current = current_vehicle_sample(sample, at(1500));
  check(current.speed_kmh == 0 && current.indicator(Indicator::low_beam) == IndicatorState::off,
        "legacy zero/off changed");
  current = current_vehicle_sample(sample, at(1500) + 1ns);
  check(!current.speed_kmh && !current.engine_rpm && !current.indicator(Indicator::low_beam),
        "legacy expiry failed");
  check(!fresh(sample, at(999), 500ms), "legacy future sample became fresh");
  sample.fuel_percent = 101;
  check(!valid_vehicle_sample(sample) && !current_vehicle_sample(sample, at(1000)).speed_kmh,
        "invalid legacy sample retained fields");
  SimulatedVehicle simulator(at(1000));
  const auto simulated = simulator.sample(at(2000));
  check(simulated.simulated && !simulated.observation_timing && valid_vehicle_sample(simulated),
        "simulation behaviour changed");
}
void mixed_rates() {
  VehicleAgePolicy policy;
  policy.readings[0] = 100ms;
  policy.readings[2] = 1s;
  policy.readings[3] = 2s;
  policy.indicators[std::size_t(Indicator::high_beam)] = 50ms;
  VehicleObservationCache cache(policy);
  policy.readings[0] = 1ms;  // Cache policy is copied and immutable.
  rejects([&] { cache.sample(at(1000)); });
  cache.begin_generation(1, at(1000), at(1000));
  auto sample = cache.sample(at(1000));
  check(sample.observation_timing && !fresh(sample, at(1000), 500ms),
        "unobserved generation fabricated freshness");
  check(!timing(sample, "speed_kmh").acquired_at && !sample.speed_kmh,
        "unobserved speed became zero");
  accept(cache, 1, 1, 1000,
         {number("speed_kmh", 0, 1000), number("engine_rpm", 0, 1000),
          number("fuel_percent", 40, 1000),
          number("coolant_c", 88, 1000, VehicleObservationOrigin::simulation),
          number("range_km", 400, 1000, VehicleObservationOrigin::calculated),
          lamp("high_beam", IndicatorState::off, 1000)});
  sample = cache.sample(at(1049));
  check(sample.speed_kmh == 0 && sample.engine_rpm == 0 && sample.simulated &&
            valid_vehicle_sample(sample),
        "zero/origins not retained");
  check(timing(sample, "range_km").origin == VehicleObservationOrigin::calculated &&
            timing(sample, "coolant_c").origin == VehicleObservationOrigin::simulation &&
            timing(sample, "speed_kmh").origin == VehicleObservationOrigin::measured,
        "per-signal provenance was lost");
  auto lights = vehicle_indicator_snapshot(sample, at(1049), 1ns);
  check(lights.known.test(std::size_t(Indicator::high_beam)) && lights.visible.none(),
        "known off inferred unknown/visible");
  sample = cache.sample(at(1050));
  check(!sample.indicator(Indicator::high_beam) &&
            timing(sample, "high_beam").availability == VehicleAvailability::stale,
        "indicator maximum-age boundary did not expire");
  check(sample.speed_kmh == 0 && timing(sample, "speed_kmh").acquired_at == at(1000) &&
            timing(sample, "speed_kmh").maximum_age == 100ms,
        "read poll renewed observation or policy mutated");
  auto filtered = current_vehicle_sample(sample, at(1100), Clock::duration::zero());
  check(!filtered.speed_kmh && filtered.fuel_percent == 40 &&
            timing(filtered, "speed_kmh").availability == VehicleAvailability::stale,
        "per-signal expiry was replaced by aggregate/legacy expiry");
  check(fresh(sample, at(1100), Clock::duration::zero()),
        "slow fresh field was lost with fast-field expiry");
  accept(cache, 1, 2, 1100, {});  // Empty heartbeat advances ordering, never acquisition time.
  sample = cache.sample(at(1100));
  check(!sample.speed_kmh && sample.fuel_percent == 40 &&
            timing(sample, "fuel_percent").acquired_at == at(1000),
        "empty batch renewed or removed an omitted signal");
  sample = cache.sample(at(2000));
  check(!sample.fuel_percent && sample.coolant_c == 88 &&
            timing(sample, "fuel_percent").acquired_at == at(1000),
        "mixed-rate expiry failed");
  sample = cache.sample(at(3000));
  check(!fresh(sample, at(3000), 500ms) && !sample.coolant_c && sample.simulated,
        "last field expiry or simulation provenance lost");
  rejects([&] { cache.sample(at(2999)); });
}
void atomic_batches() {
  const auto modes = *parse_drive_mode_catalog_key("comfort,sport");
  VehicleObservationCache cache({}, modes);
  cache.begin_generation(1, at(1000), at(1000));
  accept(cache, 1, 1, 1000, {number("speed_kmh", 10, 1000), number("fuel_percent", 20, 1000)});
  rejects([&] {
    accept(cache, 1, 2, 1050, {number("speed_kmh", 25, 1040), number("fuel_percent", 101, 1040)});
  });
  auto sample = cache.sample(at(1000));  // Invalid batch did not even move host time.
  check(sample.speed_kmh == 10 && sample.fuel_percent == 20 &&
            timing(sample, "speed_kmh").acquired_at == at(1000),
        "invalid batch partially applied");
  std::vector<VehicleObservation> invalid{
      number("unknown", 1, 1000),
      number("speed_kmh", 1, 1001),
      number("speed_kmh", 1, 999),
      number("speed_kmh", std::numeric_limits<double>::quiet_NaN(), 1000),
      number("speed_kmh", 301, 1000),
      lamp("speed_kmh", IndicatorState::off, 1000),
      number("high_beam", 1, 1000),
      lamp("high_beam", static_cast<IndicatorState>(100), 1000)};
  auto unavailable = number("speed_kmh", 0, 1000);
  unavailable.availability = VehicleAvailability::unavailable;
  invalid.push_back(unavailable);
  auto invalid_origin = number("speed_kmh", 10, 1000);
  invalid_origin.origin = static_cast<VehicleObservationOrigin>(90);
  invalid.push_back(invalid_origin);
  auto invalid_state = number("speed_kmh", 10, 1000);
  invalid_state.availability = VehicleAvailability::stale;
  invalid.push_back(invalid_state);
  auto changed_origin = number("speed_kmh", 10, 1000, VehicleObservationOrigin::calculated);
  invalid.push_back(changed_origin);
  invalid.push_back(number("speed_kmh", 11, 1000));
  for (const auto& value : invalid)
    rejects([&] { accept(cache, 1, 2, 1000, {value}); });
  rejects([&] {
    accept(cache, 1, 2, 1000, {number("speed_kmh", 10, 1000), number("speed_kmh", 10, 1000)});
  });
  std::array<VehicleObservation, vehicle_observation_capacity + 1> too_large;
  too_large.fill(number("speed_kmh", 10, 1000));
  rejects([&] { cache.accept(1, 2, too_large, at(1000)); });
  rejects([&] { accept(cache, 1, 0, 1000, {}); });
  rejects([&] { accept(cache, 1, 1, 1000, {number("speed_kmh", 10, 1000)}); });
  rejects([&] { accept(cache, 2, 2, 1000, {number("speed_kmh", 10, 1000)}); });
  accept(cache, 1, 2, 1010, {number("speed_kmh", 15, 1010)});
  sample = cache.sample(at(1010));
  check(sample.speed_kmh == 15 && sample.fuel_percent == 20 &&
            timing(sample, "fuel_percent").acquired_at == at(1000),
        "delta omission changed another signal or invalid batch consumed sequence");
  rejects([&] { accept(cache, 1, 3, 1020, {number("speed_kmh", 14, 1005)}); });
  accept(cache, 1, 3, 1020,
         {number("speed_kmh", 15, 1010)});  // Equal-time identical, newer sequence: harmless no-op.
  check(timing(cache.sample(at(1020)), "speed_kmh").acquired_at == at(1010),
        "idempotent observation renewed acquisition");
  VehicleObservation absent{"fuel_percent", std::monostate{}, at(1020),
                            VehicleObservationOrigin::measured, VehicleAvailability::unavailable};
  accept(cache, 1, 4, 1020, {absent});
  sample = cache.sample(at(1020));
  check(!sample.fuel_percent &&
            timing(sample, "fuel_percent").availability == VehicleAvailability::unavailable &&
            timing(sample, "fuel_percent").acquired_at == at(1020),
        "explicit unavailable report became zero or stale");
  // All 38 distinct semantic observations fit one atomic update.
  std::vector<VehicleObservation> full;
  for (const auto& reading : reading_definitions)
    full.push_back(number(reading.key, reading.minimum, 1030));
  for (const auto& indicator : indicator_definitions)
    full.push_back(lamp(indicator.key, IndicatorState::off, 1030));
  full.push_back({"transmission",
                  TransmissionState{GearboxType::manual, std::nullopt,
                                    EngagedGear{EngagedGearKind::neutral, 0}},
                  at(1030)});
  full.push_back({"drive_mode", *parse_drive_mode_identifier("comfort"), at(1030)});
  full.push_back({"trip", TripState{*parse_trip_identifier("current"), 1, 0.}, at(1030)});
  full.push_back({"heading",
                  HeadingState{0, HeadingReference::true_north, HeadingSource::vehicle_orientation},
                  at(1030)});
  cache.accept(1, 5, full, at(1030));
  sample = cache.sample(at(1030));
  check(valid_vehicle_sample(sample), "full bounded batch rejected");
  for (const auto& reading : reading_definitions)
    check(bool(sample.*reading.member), "full batch lost reading");
  for (const auto& indicator : indicator_definitions)
    check(sample.indicator(indicator.id) == IndicatorState::off, "full batch lost lamp");
  check(sample.transmission && vehicle_observation_value(sample, transmission_observation_index) ==
                                   VehicleObservationValue{*sample.transmission},
        "full batch lost coherent transmission");
  check(sample.drive_mode == parse_drive_mode_identifier("comfort") &&
            sample.drive_mode_catalog == modes,
        "full batch lost effective drive mode/static vehicle capability");
  check(sample.trip && sample.trip->distance_km == 0 && sample.heading &&
            sample.heading->degrees == 0,
        "full batch lost known zero trip counter or explicitly qualified north heading");
}
void drive_mode_observations() {
  auto configured = *parse_drive_mode_catalog_key("comfort,sport,track");
  const auto expected = configured;
  const auto comfort = *parse_drive_mode_identifier("comfort"),
             sport = *parse_drive_mode_identifier("sport"),
             track = *parse_drive_mode_identifier("track");
  VehicleAgePolicy ages;
  ages.drive_mode = 100ms;
  VehicleObservationCache cache(ages, configured);
  ages.drive_mode = 1ns;
  configured.count = 0;  // Configured catalog and ages are copied.
  cache.begin_generation(1, at(1000), at(1000));
  auto sample = cache.sample(at(1000));
  check(sample.drive_mode_catalog == expected && !sample.drive_mode &&
            !timing(sample, "drive_mode").acquired_at &&
            timing(sample, "drive_mode").maximum_age == 100ms && !fresh(sample, at(1000), 500ms),
        "unobserved mode fabricated Sport or accepted mutable catalog/age policy");
  accept(cache, 1, 1, 1000,
         {{"drive_mode", comfort, at(1000), VehicleObservationOrigin::simulation}});
  sample = cache.sample(at(1099));
  check(sample.drive_mode == comfort && sample.simulated && fresh(sample, at(1099), 1ns) &&
            timing(sample, "drive_mode").acquired_at == at(1000) && !sample.transmission,
        "mode-only sample lost freshness/provenance or fabricated a gearbox selector");
  sample = cache.sample(at(1100));
  check(!sample.drive_mode && sample.drive_mode_catalog == expected && sample.simulated &&
            !fresh(sample, at(1100), 500ms) &&
            timing(sample, "drive_mode").availability == VehicleAvailability::stale,
        "mode exact expiry retained active state or removed static catalog/provenance");
  accept(cache, 1, 2, 1101,
         {{"drive_mode", comfort, at(1000), VehicleObservationOrigin::simulation}});
  check(!cache.sample(at(1101)).drive_mode, "equal acquisition revived expired mode");
  rejects([&] {
    accept(cache, 1, 3, 1110,
           {number("speed_kmh", 20, 1110),
            {"drive_mode", *parse_drive_mode_identifier("unsupported"), at(1110)}});
  });
  sample = cache.sample(at(1101));
  check(!sample.speed_kmh && !sample.drive_mode && sample.drive_mode_catalog == expected,
        "unsupported mode partially committed another reading or moved host clock");
  accept(cache, 1, 3, 1110, {{"drive_mode", sport, at(1105)}});
  sample = cache.sample(at(1110));
  check(sample.drive_mode == sport && !sample.simulated &&
            timing(sample, "drive_mode").acquired_at == at(1105),
        "invalid mode consumed sequence or fresh delayed mode failed to recover");
  rejects([&] { accept(cache, 1, 4, 1111, {{"drive_mode", track, at(1105)}}); });
  rejects([&] {
    accept(cache, 1, 4, 1111,
           {{"drive_mode", sport, at(1105), VehicleObservationOrigin::calculated}});
  });
  rejects([&] { accept(cache, 1, 4, 1111, {{"drive_mode", sport, at(1104)}}); });
  rejects([&] { accept(cache, 1, 4, 1111, {number("drive_mode", 1, 1111)}); });
  rejects([&] { accept(cache, 1, 4, 1111, {{"speed_kmh", sport, at(1111)}}); });
  rejects([&] { accept(cache, 1, 4, 1111, {{"drive_mode", DriveModeIdentifier{}, at(1111)}}); });
  rejects([&] {
    accept(cache, 1, 4, 1111, {{"drive_mode", sport, at(1111)}, {"drive_mode", sport, at(1111)}});
  });
  cache.withdraw(at(1120));
  sample = cache.sample(at(1120));
  check(!sample.drive_mode && sample.drive_mode_catalog == expected &&
            timing(sample, "drive_mode").availability == VehicleAvailability::disconnected &&
            timing(sample, "drive_mode").acquired_at == at(1105),
        "mode loss retained active value or erased catalog/acquisition");
  accept(cache, 1, 4, 1121, {{"drive_mode", sport, at(1105)}});
  check(!cache.sample(at(1121)).drive_mode, "same-time acquisition revived disconnected mode");
  accept(cache, 1, 5, 1130,
         {{"drive_mode", track, at(1125), VehicleObservationOrigin::calculated}});
  sample = cache.sample(at(1130));
  check(sample.drive_mode == track && sample.drive_mode_catalog == expected &&
            timing(sample, "drive_mode").origin == VehicleObservationOrigin::calculated,
        "fresh delayed mode could not recover or lost original origin");
  accept(cache, 1, 6, 1131,
         {{"drive_mode", std::monostate{}, at(1131), VehicleObservationOrigin::measured,
           VehicleAvailability::unavailable}});
  sample = cache.sample(at(1131));
  check(!sample.drive_mode && sample.drive_mode_catalog == expected &&
            timing(sample, "drive_mode").availability == VehicleAvailability::unavailable,
        "explicit unknown mode became a default value or removed static catalog");
  rejects([&] { accept(cache, 1, 7, 1132, {{"drive_mode", track, at(1131)}}); });
  cache.begin_generation(2, at(1131), at(1140));
  sample = cache.sample(at(1140));
  check(!sample.drive_mode && sample.drive_mode_catalog == expected &&
            !timing(sample, "drive_mode").acquired_at &&
            timing(sample, "drive_mode").maximum_age == 100ms,
        "source restart retained mode or changed immutable capability/age");
  check(&vehicle_signal_timing(*sample.observation_timing, drive_mode_observation_index) ==
                &sample.observation_timing->drive_mode &&
            std::holds_alternative<std::monostate>(
                vehicle_observation_value(sample, drive_mode_observation_index)),
        "shared drive mode timing/value index incorrect");
  accept(cache, 2, 1, 1140, {{"drive_mode", comfort, at(1140)}});
  sample = cache.sample(at(1140));
  check(vehicle_observation_value(sample, drive_mode_observation_index) ==
            VehicleObservationValue{comfort},
        "shared drive mode value lost typed semantic identifier");
  check(!current_vehicle_sample(sample, at(1139)).drive_mode, "future mode retained active value");
  VehicleObservationCache unconfigured;
  unconfigured.begin_generation(1, at(1000), at(1000));
  rejects([&] { accept(unconfigured, 1, 1, 1000, {{"drive_mode", sport, at(1000)}}); });
  unconfigured.withdraw(at(1000));
  sample = unconfigured.sample(at(1000));
  check(!sample.drive_mode_catalog && !sample.drive_mode &&
            !timing(sample, "drive_mode").acquired_at &&
            timing(sample, "drive_mode").availability == VehicleAvailability::unavailable,
        "old source withdrawal manufactured an unrepresentable extension mode channel");
  accept(unconfigured, 1, 1, 1000,
         {{"drive_mode", std::monostate{}, at(1000), VehicleObservationOrigin::measured,
           VehicleAvailability::unavailable}});
  check(valid_vehicle_sample(unconfigured.sample(at(1000))) &&
            !unconfigured.sample(at(1000)).drive_mode,
        "explicit mode unavailable required a fabricated capability catalog");
}
void transmission_observations() {
  const auto neutral = EngagedGear{EngagedGearKind::neutral, 0};
  const auto reverse = EngagedGear{EngagedGearKind::reverse, 0};
  const auto forward = EngagedGear{EngagedGearKind::forward, 3};
  const TransmissionState parked{GearboxType::automatic, TransmissionSelector::park, neutral};
  const TransmissionState driving{GearboxType::automatic, TransmissionSelector::drive, forward};
  VehicleAgePolicy policy;
  policy.transmission = 100ms;
  VehicleObservationCache cache(policy);
  policy.transmission = 1ns;
  cache.begin_generation(1, at(1000), at(1000));
  const auto empty = cache.sample(at(1000));
  check(!empty.transmission && !timing(empty, "transmission").acquired_at &&
            timing(empty, "transmission").maximum_age == 100ms &&
            std::holds_alternative<std::monostate>(
                vehicle_observation_value(empty, transmission_observation_index)),
        "unobserved transmission fabricated gear or mutable age policy");
  accept(cache, 1, 1, 1000,
         {{"transmission", parked, at(1000), VehicleObservationOrigin::simulation}});
  auto sample = cache.sample(at(1099));
  check(sample.transmission == parked && sample.simulated && fresh(sample, at(1099), 1ns) &&
            timing(sample, "transmission").acquired_at == at(1000),
        "fresh actual transmission was lost, inferred or renewed");
  sample = cache.sample(at(1100));
  check(!sample.transmission && sample.simulated && !fresh(sample, at(1100), 500ms) &&
            timing(sample, "transmission").availability == VehicleAvailability::stale &&
            timing(sample, "transmission").acquired_at == at(1000),
        "actual transmission expiry or recorded simulation provenance lost");
  accept(cache, 1, 2, 1101,
         {{"transmission", parked, at(1000), VehicleObservationOrigin::simulation}});
  check(!cache.sample(at(1101)).transmission, "equal-time observation revived expired gear");
  accept(cache, 1, 3, 1110, {{"transmission", driving, at(1105)}});
  sample = cache.sample(at(1110));
  check(sample.transmission == driving && !sample.simulated &&
            transmission_display_label(sample.transmission) == "3",
        "coherent fresh gear did not replace expired simulated observation");
  rejects([&] {
    accept(cache, 1, 4, 1120,
           {{"transmission",
             TransmissionState{GearboxType::automatic, TransmissionSelector::reverse, forward},
             at(1120)},
            number("speed_kmh", 20, 1120)});
  });
  sample = cache.sample(at(1110));
  check(sample.transmission == driving && !sample.speed_kmh &&
            timing(sample, "transmission").acquired_at == at(1105),
        "incoherent transmission partially committed or moved the host clock");
  rejects([&] { accept(cache, 1, 4, 1110, {{"transmission", parked, at(1105)}}); });
  rejects([&] { accept(cache, 1, 4, 1110, {{"transmission", driving, at(1104)}}); });
  rejects([&] { accept(cache, 1, 4, 1110, {number("transmission", 3, 1110)}); });
  rejects([&] { accept(cache, 1, 4, 1110, {{"speed_kmh", driving, at(1110)}}); });
  rejects([&] { accept(cache, 1, 4, 1110, {{"transmission", TransmissionState{}, at(1110)}}); });
  cache.withdraw(at(1120));
  sample = cache.sample(at(1120));
  check(!sample.transmission &&
            timing(sample, "transmission").availability == VehicleAvailability::disconnected &&
            timing(sample, "transmission").acquired_at == at(1105),
        "withdrawal retained actual gear or fabricated acquisition");
  accept(cache, 1, 4, 1121, {{"transmission", driving, at(1105)}});
  check(!cache.sample(at(1121)).transmission,
        "same-time observation revived disconnected transmission");
  const TransmissionState reversing{GearboxType::automatic, TransmissionSelector::reverse, reverse};
  accept(cache, 1, 5, 1130, {{"transmission", reversing, at(1125)}});
  sample = cache.sample(at(1130));
  check(sample.transmission == reversing && timing(sample, "transmission").acquired_at == at(1125),
        "fresh delayed acquisition failed to recover transmission");
  accept(cache, 1, 6, 1131,
         {{"transmission", std::monostate{}, at(1131), VehicleObservationOrigin::measured,
           VehicleAvailability::unavailable}});
  sample = cache.sample(at(1131));
  check(!sample.transmission &&
            timing(sample, "transmission").availability == VehicleAvailability::unavailable &&
            timing(sample, "transmission").acquired_at == at(1131),
        "unknown transmission became a zero gear or stale value");
  cache.begin_generation(2, at(1131), at(1140));
  sample = cache.sample(at(1140));
  check(!sample.transmission && !timing(sample, "transmission").acquired_at &&
            timing(sample, "transmission").maximum_age == 100ms,
        "restart retained actual gear/acquisition or reset immutable age");
  const TransmissionState only_type{GearboxType::manual, std::nullopt, std::nullopt};
  accept(cache, 2, 1, 1140, {{"transmission", only_type, at(1140)}});
  sample = cache.sample(at(1140));
  check(sample.transmission == only_type && fresh(sample, at(1140), 1ns) &&
            transmission_display_label(sample.transmission) == "--",
        "known gearbox fabricated an effective selector or actual gear");
  const TransmissionState only_selector{std::nullopt, TransmissionSelector::drive, std::nullopt};
  accept(cache, 2, 2, 1141, {{"transmission", only_selector, at(1141)}});
  sample = cache.sample(at(1141));
  check(sample.transmission == only_selector && !sample.transmission->gearbox_type &&
            !sample.transmission->engaged_gear &&
            transmission_display_label(sample.transmission) == "D",
        "partial coherent observation retained previous unknown components");
  auto& mutable_timing =
      vehicle_signal_timing(*sample.observation_timing, transmission_observation_index);
  check(&mutable_timing == &sample.observation_timing->transmission,
        "shared mutable transmission timing index incorrect");
  bool bounded = false;
  try {
    (void)vehicle_signal_timing(*sample.observation_timing, vehicle_observation_capacity);
  } catch (const std::out_of_range&) {
    bounded = true;
  }
  check(bounded, "shared timing helper accepted out-of-bounds observation");
  bounded = false;
  try {
    (void)vehicle_observation_value(sample, vehicle_observation_capacity);
  } catch (const std::out_of_range&) {
    bounded = true;
  }
  check(bounded, "shared value helper accepted out-of-bounds observation");
}
TripState current_trip(std::uint64_t reset, double distance, double elapsed, double moving) {
  TripState result;
  result.identifier = *parse_trip_identifier("current");
  result.reset_generation = reset;
  result.distance_km = distance;
  result.elapsed_seconds = elapsed;
  result.moving_seconds = moving;
  return result;
}
void trip_heading_observations() {
  VehicleAgePolicy ages;
  ages.trip = 100ms;
  ages.heading = 50ms;
  VehicleObservationCache cache(ages);
  ages.trip = 1ns;
  ages.heading = 1ns;
  cache.begin_generation(1, at(1000), at(1000));
  auto sample = cache.sample(at(1000));
  check(!sample.trip && !sample.heading && !timing(sample, "trip").acquired_at &&
            !timing(sample, "heading").acquired_at && timing(sample, "trip").maximum_age == 100ms &&
            timing(sample, "heading").maximum_age == 50ms && !fresh(sample, at(1000), 500ms),
        "unobserved trip/heading fabricated zero/default values or mutable policy");
  const auto initial = current_trip(1, 10, 600, 500);
  const HeadingState bearing{359.9, HeadingReference::true_north,
                             HeadingSource::vehicle_orientation};
  accept(cache, 1, 1, 1000,
         {{"trip", initial, at(1000)},
          {"heading", bearing, at(1000), VehicleObservationOrigin::simulation}});
  sample = cache.sample(at(1049));
  check(sample.trip == initial && sample.heading == bearing && sample.simulated &&
            fresh(sample, at(1049), 1ns),
        "trip/heading-only freshness or explicitly qualified heading lost");
  sample = cache.sample(at(1050));
  check(sample.trip == initial && !sample.heading && sample.simulated &&
            timing(sample, "heading").availability == VehicleAvailability::stale &&
            timing(sample, "heading").acquired_at == at(1000),
        "heading exact expiry used aggregate trip date or erased simulated acquisition origin");
  sample = cache.sample(at(1100));
  check(!sample.trip && !sample.heading && !fresh(sample, at(1100), 500ms) && sample.simulated &&
            timing(sample, "trip").availability == VehicleAvailability::stale,
        "trip exact age boundary did not expire independently");
  accept(cache, 1, 2, 1101, {{"trip", initial, at(1000)}});
  check(!cache.sample(at(1101)).trip, "equal old acquisition revived expired trip");
  const auto advanced = current_trip(1, 11, 660, 550);
  accept(cache, 1, 3, 1105, {{"trip", advanced, at(1090)}});
  sample = cache.sample(at(1105));
  check(sample.trip == advanced && timing(sample, "trip").acquired_at == at(1090),
        "fresh delayed trip failed to recover with its actual acquisition");
  const auto reset = current_trip(2, 0, 0, 0);
  rejects([&] {
    accept(cache, 1, 4, 1110,
           {number("speed_kmh", 20, 1110),
            {"trip", reset, at(1110)},
            {"heading",
             HeadingState{360, HeadingReference::true_north, HeadingSource::vehicle_orientation},
             at(1110)}});
  });
  sample = cache.sample(at(1105));
  check(!sample.speed_kmh && sample.trip == advanced &&
            timing(sample, "trip").acquired_at == at(1090),
        "failed batch committed trip reset/scalar data or moved source clock");
  rejects([&] { accept(cache, 1, 4, 1110, {{"trip", initial, at(1110)}}); });
  accept(cache, 1, 4, 1110,
         {{"trip", reset, at(1110)}});  // Failed reset did not consume sequence/history.
  check(cache.sample(at(1110)).trip == reset,
        "new acquired reset did not allow known zero counters");
  cache.withdraw(at(1120));
  sample = cache.sample(at(1120));
  check(!sample.trip && !sample.heading &&
            timing(sample, "trip").availability == VehicleAvailability::disconnected &&
            timing(sample, "trip").acquired_at == at(1110),
        "trip withdrawal retained values or fabricated acquisition");
  accept(cache, 1, 5, 1121, {{"trip", reset, at(1110)}});
  check(!cache.sample(at(1121)).trip, "same old acquisition revived disconnected trip");
  accept(cache, 1, 6, 1122,
         {{"trip", std::monostate{}, at(1122), VehicleObservationOrigin::measured,
           VehicleAvailability::unavailable}});
  check(!cache.sample(at(1122)).trip &&
            timing(cache.sample(at(1122)), "trip").availability == VehicleAvailability::unavailable,
        "explicit unknown trip became a zero/reset action");
  const auto third = current_trip(3, 1, 60, 30);
  accept(cache, 1, 7, 1130, {{"trip", third, at(1130)}});
  check(cache.sample(at(1130)).trip == third, "new reset failed to recover after explicit unknown");
  cache.begin_generation(2, at(1130), at(1130));
  sample = cache.sample(at(1130));
  check(!sample.trip && !timing(sample, "trip").acquired_at &&
            timing(sample, "trip").availability == VehicleAvailability::unavailable,
        "source restart carried old raw acquisition into new-generation metadata");
  rejects([&] { accept(cache, 2, 1, 1130, {{"trip", third, at(1130)}}); });
  rejects([&] {
    accept(cache, 2, 1, 1130, {{"trip", third, at(1130), VehicleObservationOrigin::calculated}});
  });
  accept(cache, 2, 1, 1130, {});
  rejects([&] { accept(cache, 2, 2, 1131, {{"trip", third, at(1130)}}); });
  auto wrong = third;
  wrong.identifier = *parse_trip_identifier("other");
  rejects([&] { accept(cache, 2, 2, 1131, {{"trip", wrong, at(1131)}}); });
  const auto recovered = current_trip(3, 2, 120, 60);
  accept(cache, 2, 2, 1131, {{"trip", recovered, at(1131)}});
  sample = cache.sample(at(1131));
  check(sample.trip == recovered && timing(sample, "trip").acquired_at == at(1131),
        "strictly newer trip observation did not recover after empty new generation");
  rejects([&] { accept(cache, 2, 3, 1132, {{"trip", current_trip(2, 3, 180, 90), at(1132)}}); });
  rejects([&] { accept(cache, 2, 3, 1132, {{"trip", current_trip(3, 1, 240, 120), at(1132)}}); });
  const auto fourth = current_trip(4, 0, 0, 0);
  accept(cache, 2, 3, 1132, {{"trip", fourth, at(1132)}});
  rejects([&] { cache.begin_generation(3, at(1131), at(1133)); });
  sample = cache.sample(at(1132));
  check(sample.trip == fourth && sample.observation_timing->generation == 2,
        "failed source restart changed current trip/history");
  cache.begin_generation(3, at(1132), at(1133));
  rejects([&] { accept(cache, 3, 1, 1133, {{"trip", fourth, at(1132)}}); });
  const auto fifth = current_trip(5, 0, 0, 0);
  const HeadingState new_bearing{0, HeadingReference::magnetic_north,
                                 HeadingSource::course_over_ground};
  accept(cache, 3, 1, 1133, {{"trip", fifth, at(1133)}, {"heading", new_bearing, at(1133)}});
  sample = cache.sample(at(1133));
  check(sample.trip == fifth && sample.heading == new_bearing && !sample.simulated &&
            !sample.speed_kmh &&
            vehicle_observation_value(sample, trip_observation_index) ==
                VehicleObservationValue{fifth} &&
            vehicle_observation_value(sample, heading_observation_index) ==
                VehicleObservationValue{new_bearing},
        "typed trip/qualified heading indices, north zero or independent source recovery lost");
  check(&vehicle_signal_timing(*sample.observation_timing, trip_observation_index) ==
                &sample.observation_timing->trip &&
            &vehicle_signal_timing(*sample.observation_timing, heading_observation_index) ==
                &sample.observation_timing->heading,
        "shared new typed timing indices incorrect");
  VehicleObservationCache partial;
  partial.begin_generation(1, at(1000), at(1000));
  TripState moving;
  moving.identifier = *parse_trip_identifier("current");
  moving.reset_generation = 1;
  moving.moving_seconds = 300;
  accept(partial, 1, 1, 1000, {{"trip", moving, at(1000)}});
  TripState elapsed;
  elapsed.identifier = moving.identifier;
  elapsed.reset_generation = 1;
  elapsed.elapsed_seconds = 250;
  rejects([&] { accept(partial, 1, 2, 1100, {{"trip", elapsed, at(1100)}}); });
  elapsed.elapsed_seconds = 300;
  accept(partial, 1, 2, 1100, {{"trip", elapsed, at(1100)}});
  moving.moving_seconds = 400;
  accept(partial, 1, 3, 1200, {{"trip", moving, at(1200)}});
  elapsed.elapsed_seconds = 350;
  rejects([&] { accept(partial, 1, 4, 1300, {{"trip", elapsed, at(1300)}}); });
  elapsed.elapsed_seconds = 400;
  accept(partial, 1, 4, 1300, {{"trip", elapsed, at(1300)}});
  check(partial.sample(at(1300)).trip == elapsed,
        "chronological partial counters failed to recover after atomic rejection");
}
void withdrawal_recovery() {
  VehicleAgePolicy ages;
  ages.readings[0] = 100ms;
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, at(1000), at(1000));
  accept(cache, 7, 1, 1000, {number("speed_kmh", 12, 1000, VehicleObservationOrigin::calculated)});
  cache.withdraw(at(1030));
  auto sample = cache.sample(at(1030));
  check(!sample.speed_kmh &&
            timing(sample, "speed_kmh").availability == VehicleAvailability::disconnected &&
            timing(sample, "speed_kmh").acquired_at == at(1000),
        "withdrawal fabricated an acquisition or retained speed");
  accept(cache, 7, 2, 1040, {number("speed_kmh", 12, 1000, VehicleObservationOrigin::calculated)});
  sample = cache.sample(at(1040));
  check(!sample.speed_kmh &&
            timing(sample, "speed_kmh").availability == VehicleAvailability::disconnected,
        "identical replay revived disconnected signal");
  accept(cache, 7, 3, 1050, {number("speed_kmh", 14, 1020, VehicleObservationOrigin::calculated)});
  sample = cache.sample(at(1050));
  check(sample.speed_kmh == 14 && timing(sample, "speed_kmh").acquired_at == at(1020),
        "fresh delayed newer observation could not recover");
  sample = cache.sample(at(1120));
  check(!sample.speed_kmh, "boundary did not expire delayed observation");
  accept(cache, 7, 4, 1125, {number("speed_kmh", 14, 1020, VehicleObservationOrigin::calculated)});
  check(timing(cache.sample(at(1125)), "speed_kmh").availability == VehicleAvailability::stale,
        "same-time expired observation revived");
  accept(cache, 7, 5, 1140, {number("speed_kmh", 15, 1030)});
  sample = cache.sample(at(1140));
  check(!sample.speed_kmh && timing(sample, "speed_kmh").acquired_at == at(1030),
        "expired newer observation was displayed/dated at receipt");
  rejects([&] { accept(cache, 7, 6, 1140, {number("speed_kmh", 14, 1025)}); });
  accept(cache, 7, 6, 1140, {number("speed_kmh", 16, 1130)});
  sample = cache.sample(at(1160));
  check(sample.speed_kmh == 16 && timing(sample, "speed_kmh").acquired_at == at(1130),
        "fresh recovery failed or repeated sample renewed date");
  cache.withdraw(at(1160));
  rejects([&] { cache.begin_generation(8, at(1159), at(1161)); });
  rejects([&] { cache.begin_generation(7, at(1160), at(1161)); });
  rejects([&] { cache.begin_generation(0, at(1160), at(1161)); });
  rejects([&] { cache.begin_generation(8, at(1162), at(1161)); });
  cache.begin_generation(8, at(1160), at(1161));
  sample = cache.sample(at(1161));
  check(!sample.speed_kmh && !timing(sample, "speed_kmh").acquired_at &&
            sample.observation_timing->generation == 8,
        "new generation carried prior values/watermarks");
  rejects([&] { accept(cache, 7, 7, 1161, {number("speed_kmh", 0, 1160)}); });
  rejects([&] { accept(cache, 8, 1, 1161, {number("speed_kmh", 0, 1159)}); });
  accept(cache, 8, 1, 1161,
         {number("speed_kmh", 0, 1160), lamp("low_beam", IndicatorState::off, 1160)});
  sample = cache.sample(at(1161));
  check(sample.speed_kmh == 0, "fresh generation zero was unavailable");
  rejects([&] { cache.sample(at(1160)); });
  bool owner_rejected = false;
  std::thread other([&] {
    try {
      cache.sample(at(1162));
    } catch (const std::invalid_argument&) {
      owner_rejected = true;
    }
  });
  other.join();
  check(owner_rejected && cache.sample(at(1162)).speed_kmh == 0,
        "owner-thread policy damaged cache");
}
void metadata_validation() {
  VehicleObservationCache cache;
  cache.begin_generation(1, at(1000), at(1000));
  accept(cache, 1, 1, 1000, {number("speed_kmh", 20, 1000)});
  const auto good = cache.sample(at(1000));
  auto rejects_sample = [&](VehicleSample bad) {
    check(!valid_vehicle_sample(bad) && !fresh(bad, at(1000), 500ms),
          "malformed metadata accepted");
    const auto current = current_vehicle_sample(bad, at(1000));
    for (const auto& reading : reading_definitions)
      check(!(current.*reading.member), "invalid metadata retained values");
    check(!current.transmission, "invalid metadata retained transmission");
    check(!current.drive_mode, "invalid metadata retained effective drive mode");
    check(!current.trip && !current.heading, "invalid metadata retained current trip/heading");
    const auto indicators = vehicle_indicator_snapshot(bad, at(1000));
    check(!indicators.fresh_data && indicators.known.none() && indicators.visible.none(),
          "invalid metadata retained lamps");
  };
  auto bad = good;
  bad.observation_timing->generation = 0;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->started_at = Clock::time_point{};
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->started_at = at(1001);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].acquired_at = at(999);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].acquired_at = at(1001);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].maximum_age = 0ns;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].maximum_age = 24h + 1ns;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].origin = static_cast<VehicleObservationOrigin>(4);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].availability = static_cast<VehicleAvailability>(4);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].acquired_at.reset();
  rejects_sample(bad);
  bad = good;
  bad.speed_kmh.reset();
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].availability = VehicleAvailability::unavailable;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].origin = VehicleObservationOrigin::simulation;
  rejects_sample(bad);
  bad = good;
  bad.simulated = true;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->readings[0].availability = VehicleAvailability::stale;
  bad.speed_kmh.reset();
  bad.observation_timing->readings[0].acquired_at.reset();
  rejects_sample(bad);
  check(!fresh(good, at(999), 500ms) && !current_vehicle_sample(good, at(999)).speed_kmh,
        "future snapshot retained a current value");
  VehicleAgePolicy policy;
  policy.readings[0] = 0ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.indicators[0] = 24h + 1ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.transmission = 0ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.transmission = 24h + 1ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.drive_mode = 0ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.drive_mode = 24h + 1ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.trip = 0ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.trip = 24h + 1ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.heading = 0ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  policy = VehicleAgePolicy{};
  policy.heading = 24h + 1ns;
  rejects([&] { VehicleObservationCache invalid(policy); });
  rejects([&] { VehicleObservationCache invalid({}, DriveModeCatalog{}); });
  bad = good;
  bad.transmission = TransmissionState{GearboxType::manual, std::nullopt, std::nullopt};
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->transmission.maximum_age = 0ns;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->transmission.maximum_age = 24h + 1ns;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->transmission.acquired_at = at(999);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->transmission.acquired_at = at(1001);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->transmission.availability = VehicleAvailability::available;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->transmission.origin = static_cast<VehicleObservationOrigin>(99);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->transmission.availability = static_cast<VehicleAvailability>(99);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->transmission.acquired_at = at(1000);
  bad.observation_timing->transmission.origin = VehicleObservationOrigin::simulation;
  rejects_sample(bad);
  accept(cache, 1, 2, 1000,
         {{"transmission",
           TransmissionState{GearboxType::automatic, TransmissionSelector::drive,
                             EngagedGear{EngagedGearKind::forward, 2}},
           at(1000)}});
  const auto with_transmission = cache.sample(at(1000));
  check(valid_vehicle_sample(with_transmission) && with_transmission.transmission,
        "valid timed transmission shape rejected");
  bad = with_transmission;
  bad.transmission->selector = TransmissionSelector::reverse;
  rejects_sample(bad);
  bad = with_transmission;
  bad.transmission->gearbox_type = static_cast<GearboxType>(99);
  rejects_sample(bad);
  bad = with_transmission;
  bad.transmission = TransmissionState{};
  rejects_sample(bad);
  bad = with_transmission;
  bad.observation_timing->transmission.acquired_at.reset();
  rejects_sample(bad);
  bad = with_transmission;
  bad.observation_timing->transmission.availability = VehicleAvailability::disconnected;
  rejects_sample(bad);
  bad = with_transmission;
  bad.transmission.reset();
  rejects_sample(bad);
  bad = with_transmission;
  bad.transmission.reset();
  bad.observation_timing->transmission.availability = VehicleAvailability::stale;
  bad.observation_timing->transmission.acquired_at.reset();
  rejects_sample(bad);
  check(!current_vehicle_sample(with_transmission, at(999)).transmission,
        "future timed transmission retained actual gear");
  VehicleObservationCache unstarted;
  rejects([&] { unstarted.begin_generation(1, Clock::time_point{}, at(1000)); });
  rejects([&] { unstarted.withdraw(at(1000)); });
}
void flashing() {
  VehicleAgePolicy policy;
  policy.indicators[std::size_t(Indicator::left_turn)] = 2s;
  VehicleObservationCache cache(policy);
  cache.begin_generation(1, at(2000), at(2000));
  accept(cache, 1, 1, 2000, {lamp("left_turn", IndicatorState::flashing, 2000)});
  const auto sample = cache.sample(at(2000));
  const auto id = std::size_t(Indicator::left_turn);
  auto snapshot = vehicle_indicator_snapshot(sample, at(2000));
  check(snapshot.known.test(id) && snapshot.visible.test(id), "flash bright phase missing");
  snapshot = vehicle_indicator_snapshot(sample, at(2400));
  check(snapshot.known.test(id) && !snapshot.visible.test(id), "flash dark phase lost known state");
  snapshot = vehicle_indicator_snapshot(sample, at(4000));
  check(!snapshot.fresh_data && snapshot.known.none() && snapshot.visible.none(),
        "stale flash retained known/visible lamps");
}
void drive_mode_metadata_validation() {
  const auto catalog = *parse_drive_mode_catalog_key("comfort,sport");
  const auto comfort = *parse_drive_mode_identifier("comfort");
  VehicleObservationCache cache({}, catalog);
  cache.begin_generation(1, at(1000), at(1000));
  accept(cache, 1, 1, 1000, {{"drive_mode", comfort, at(1000)}, number("speed_kmh", 15, 1000)});
  const auto good = cache.sample(at(1000));
  auto rejects_sample = [&](VehicleSample bad) {
    check(!valid_vehicle_sample(bad) && !fresh(bad, at(1000), 500ms),
          "malformed timed mode/catalog accepted");
    const auto filtered = current_vehicle_sample(bad, at(1000));
    check(!filtered.drive_mode && !filtered.speed_kmh,
          "invalid mode/catalog retained active observations");
  };
  auto bad = good;
  bad.drive_mode_catalog.reset();
  rejects_sample(bad);
  bad = good;
  bad.drive_mode_catalog->count = 0;
  rejects_sample(bad);
  check(!valid_vehicle_sample(current_vehicle_sample(bad, at(1000))),
        "malformed timed static catalog became valid by filtering");
  bad = good;
  bad.drive_mode_catalog->count = 255;
  rejects_sample(bad);
  bad = good;
  bad.drive_mode_catalog->identifiers[1] = bad.drive_mode_catalog->identifiers[0];
  rejects_sample(bad);
  bad = good;
  bad.drive_mode_catalog->identifiers[2] = comfort;
  rejects_sample(bad);
  bad = good;
  bad.drive_mode = *parse_drive_mode_identifier("Comfort");
  rejects_sample(bad);
  bad = good;
  bad.drive_mode->length = 255;
  rejects_sample(bad);
  bad = good;
  bad.drive_mode->bytes[bad.drive_mode->length] = 'x';
  rejects_sample(bad);
  bad = good;
  bad.drive_mode.reset();
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.acquired_at.reset();
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.acquired_at = at(999);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.acquired_at = at(1001);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.maximum_age = 0ns;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.maximum_age = 24h + 1ns;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.origin = static_cast<VehicleObservationOrigin>(99);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.availability = static_cast<VehicleAvailability>(99);
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.availability = VehicleAvailability::disconnected;
  rejects_sample(bad);
  bad = good;
  bad.observation_timing->drive_mode.origin = VehicleObservationOrigin::simulation;
  rejects_sample(bad);
  bad = good;
  bad.drive_mode.reset();
  bad.observation_timing->drive_mode.availability = VehicleAvailability::stale;
  bad.observation_timing->drive_mode.acquired_at.reset();
  rejects_sample(bad);
}
void trip_heading_metadata_validation() {
  VehicleObservationCache cache;
  cache.begin_generation(1, at(1000), at(1000));
  accept(cache, 1, 1, 1000,
         {{"trip", current_trip(1, 1, 60, 30), at(1000)},
          {"heading",
           HeadingState{45, HeadingReference::true_north, HeadingSource::vehicle_orientation},
           at(1000)},
          number("speed_kmh", 15, 1000)});
  const auto good = cache.sample(at(1000));
  auto rejects_sample = [&](const VehicleSample& bad) {
    check(!valid_vehicle_sample(bad) && !fresh(bad, at(1000), 500ms),
          "malformed timed trip/heading accepted");
    const auto filtered = current_vehicle_sample(bad, at(1000));
    check(!filtered.trip && !filtered.heading && !filtered.speed_kmh,
          "invalid compound trip/heading retained readings");
  };
  for (const auto index : {trip_observation_index, heading_observation_index}) {
    auto bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).acquired_at.reset();
    rejects_sample(bad);
    bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).acquired_at = at(999);
    rejects_sample(bad);
    bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).acquired_at = at(1001);
    rejects_sample(bad);
    bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).maximum_age = 0ns;
    rejects_sample(bad);
    bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).maximum_age = 24h + 1ns;
    rejects_sample(bad);
    bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).origin =
        static_cast<VehicleObservationOrigin>(99);
    rejects_sample(bad);
    bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).availability =
        static_cast<VehicleAvailability>(99);
    rejects_sample(bad);
    bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).availability =
        VehicleAvailability::unavailable;
    rejects_sample(bad);
    bad = good;
    vehicle_signal_timing(*bad.observation_timing, index).origin =
        VehicleObservationOrigin::simulation;
    rejects_sample(bad);
  }
  auto bad = good;
  bad.trip->reset_generation = 0;
  rejects_sample(bad);
  bad = good;
  bad.trip->identifier.length = 255;
  rejects_sample(bad);
  bad = good;
  bad.trip->moving_seconds = 61;
  rejects_sample(bad);
  bad = good;
  bad.trip->average_speed_kmh = 30;
  bad.trip->average_speed_basis = TripAverageSpeedBasis::total;
  rejects_sample(bad);
  bad = good;
  bad.trip.reset();
  rejects_sample(bad);
  bad = good;
  bad.heading.reset();
  rejects_sample(bad);
  bad = good;
  bad.heading->degrees = 360;
  rejects_sample(bad);
  bad = good;
  bad.heading->reference = HeadingReference::unknown;
  rejects_sample(bad);
  bad = good;
  bad.heading->source = HeadingSource::unknown;
  rejects_sample(bad);
  const auto future = current_vehicle_sample(good, at(999));
  check(!future.trip && !future.heading, "future timed trip/heading remained available");
}
}  // namespace
int main() try {
  legacy();
  mixed_rates();
  atomic_batches();
  transmission_observations();
  drive_mode_observations();
  trip_heading_observations();
  withdrawal_recovery();
  metadata_validation();
  flashing();
  drive_mode_metadata_validation();
  trip_heading_metadata_validation();
  std::cout << "vehicle per-signal acquisition, typed trip/heading, expiry, provenance, atomic "
               "ordering and source-generation checks passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
