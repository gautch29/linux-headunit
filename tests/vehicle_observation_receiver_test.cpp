// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_observation_receiver.hpp>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
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
  check(rejected, "invalid complete observation snapshot was accepted");
}
Clock::time_point at(std::int64_t ms) {
  return Clock::time_point{std::chrono::milliseconds(ms)};
}
VehicleObservationPacket packet(std::int64_t published = 1000, std::uint64_t sequence = 1,
                                std::uint64_t generation = 1, std::int64_t started = 1000) {
  VehicleObservationPacket result;
  result.sequence = sequence;
  result.sample.sampled_at = at(published);
  result.sample.observation_timing.emplace();
  auto& timing = *result.sample.observation_timing;
  timing.generation = generation;
  timing.started_at = at(started);
  return result;
}
VehicleSignalTiming& timing(VehicleSample& sample, std::size_t index) {
  return vehicle_signal_timing(*sample.observation_timing, index);
}
const VehicleSignalTiming& timing(const VehicleSample& sample, std::size_t index) {
  return vehicle_signal_timing(*sample.observation_timing, index);
}
void simulation_flag(VehicleSample& sample) {
  sample.simulated = false;
  for (std::size_t i = 0; i < vehicle_observation_capacity; ++i)
    sample.simulated |= timing(sample, i).acquired_at &&
                        timing(sample, i).origin == VehicleObservationOrigin::simulation;
}
void number(VehicleObservationPacket& packet, std::size_t index, std::optional<double> value,
            std::int64_t acquired,
            VehicleAvailability availability = VehicleAvailability::available,
            VehicleObservationOrigin origin = VehicleObservationOrigin::measured) {
  packet.sample.*reading_definitions[index].member = value;
  auto& signal = timing(packet.sample, index);
  signal.acquired_at = at(acquired);
  signal.availability = availability;
  signal.origin = origin;
  simulation_flag(packet.sample);
}
void lamp(VehicleObservationPacket& packet, Indicator id, std::optional<IndicatorState> value,
          std::int64_t acquired,
          VehicleAvailability availability = VehicleAvailability::available) {
  packet.sample.indicator(id) = value;
  auto& signal = timing(packet.sample, reading_definitions.size() + std::size_t(id));
  signal.acquired_at = at(acquired);
  signal.availability = availability;
}
VehicleObservationPacket complete() {
  auto result = packet();
  for (std::size_t i = 0; i < reading_definitions.size(); ++i)
    number(result, i, reading_definitions[i].minimum, 1000);
  for (const auto& definition : indicator_definitions)
    lamp(result, definition.id, IndicatorState::off, 1000);
  return result;
}
void unchanged(const VehicleSample& expected, const VehicleSample& actual) {
  check(actual.sampled_at == expected.sampled_at && actual.simulated == expected.simulated &&
            bool(actual.observation_timing) == bool(expected.observation_timing),
        "invalid packet changed source publication or metadata");
  for (const auto& definition : reading_definitions)
    check(actual.*definition.member == expected.*definition.member,
          "invalid packet partly changed a reading");
  check(actual.indicators == expected.indicators, "invalid packet partly changed an indicator");
  check(actual.transmission == expected.transmission, "invalid packet partly changed transmission");
  check(actual.drive_mode_catalog == expected.drive_mode_catalog &&
            actual.drive_mode == expected.drive_mode,
        "invalid packet partly changed mode catalog or active mode");
  check(actual.trip == expected.trip && actual.heading == expected.heading,
        "invalid packet partly changed trip or heading");
  if (!expected.observation_timing) return;
  const auto& a = *actual.observation_timing;
  const auto& e = *expected.observation_timing;
  check(a.generation == e.generation && a.started_at == e.started_at,
        "invalid packet changed source generation");
  for (std::size_t i = 0; i < vehicle_observation_capacity; ++i) {
    const auto& next = timing(actual, i);
    const auto& previous = timing(expected, i);
    check(next.acquired_at == previous.acquired_at && next.maximum_age == previous.maximum_age &&
              next.origin == previous.origin && next.availability == previous.availability,
          "invalid packet changed a signal watermark");
  }
}
void mixed_expiry() {
  VehicleObservationReceiver receiver;
  const auto empty = receiver.sample(at(900));
  check(!receiver.initialized() && !empty.observation_timing && !empty.speed_kmh &&
            empty.sampled_at == Clock::time_point{},
        "uninitialized receiver fabricated metadata or a publication date");
  auto value = packet();
  number(value, 0, 0, 1000);
  number(value, 1, 0, 1000);
  number(value, 2, 40, 1000);
  number(value, 3, 88, 1000, VehicleAvailability::available, VehicleObservationOrigin::simulation);
  number(value, 8, 400, 1000, VehicleAvailability::available, VehicleObservationOrigin::calculated);
  lamp(value, Indicator::low_beam, IndicatorState::off, 1000);
  timing(value.sample, 0).maximum_age = 100ms;
  timing(value.sample, 2).maximum_age = 1s;
  timing(value.sample, 3).maximum_age = 2s;
  timing(value.sample, 10 + std::size_t(Indicator::low_beam)).maximum_age = 50ms;
  receiver.accept(value, at(1040));
  auto current = receiver.sample(at(1049));
  check(receiver.initialized() && current.speed_kmh == 0 && current.engine_rpm == 0 &&
            current.simulated && current.sampled_at == at(1000) && !current.oil_c &&
            !timing(current, 4).acquired_at,
        "zero, provenance or source dates lost");
  const auto lights = vehicle_indicator_snapshot(current, at(1049));
  check(lights.known.test(std::size_t(Indicator::low_beam)) && lights.visible.none(),
        "known off became unknown or on");
  current = receiver.sample(at(1050));
  check(!current.indicator(Indicator::low_beam) && current.speed_kmh == 0 &&
            timing(current, 10 + std::size_t(Indicator::low_beam)).availability ==
                VehicleAvailability::stale,
        "indicator boundary failed");
  current = receiver.sample(at(1100));
  check(!current.speed_kmh && current.fuel_percent == 40 && current.coolant_c == 88 &&
            current.sampled_at == at(1000) && timing(current, 0).acquired_at == at(1000) &&
            timing(current, 0).availability == VehicleAvailability::stale,
        "poll renewed dates or mixed-rate field expiry failed");
  current = receiver.sample(at(2000));
  check(!current.fuel_percent && current.coolant_c == 88, "slow field expired with fast field");
  current = receiver.sample(at(3000));
  check(!fresh(current, at(3000), 500ms) && !current.coolant_c && current.simulated &&
            valid_vehicle_sample(current),
        "expiry fabricated freshness or erased historical provenance");
  rejects([&] { receiver.sample(at(3000) - 1ns); });
}
void semantic_atomicity() {
  VehicleObservationReceiver receiver;
  auto original = complete();
  receiver.accept(original, at(1000));
  const auto before = receiver.sample(at(1000));
  std::vector<std::function<void(VehicleObservationPacket&)>> invalid{
      [](auto& p) { p.sequence = 0; },
      [](auto& p) { p.sample.observation_timing.reset(); },
      [](auto& p) { p.sample.observation_timing->generation = 0; },
      [](auto& p) { p.sample.observation_timing->started_at = Clock::time_point{}; },
      [](auto& p) { p.sample.observation_timing->started_at = at(1011); },
      [](auto& p) { p.sample.sampled_at = at(1201); },
      [](auto& p) { timing(p.sample, 33).origin = static_cast<VehicleObservationOrigin>(99); },
      [](auto& p) { timing(p.sample, 33).availability = static_cast<VehicleAvailability>(99); },
      [](auto& p) { p.sample.indicators.back() = static_cast<IndicatorState>(99); },
      [](auto& p) { p.sample.odometer_km = std::numeric_limits<double>::quiet_NaN(); },
      [](auto& p) { p.sample.odometer_km = 10000000; },
      [](auto& p) { timing(p.sample, 33).maximum_age = 0ns; },
      [](auto& p) { timing(p.sample, 33).maximum_age = 24h + 1ns; },
      [](auto& p) { timing(p.sample, 33).acquired_at = at(999); },
      [](auto& p) { timing(p.sample, 33).acquired_at = at(1011); },
      [](auto& p) { timing(p.sample, 33).acquired_at.reset(); },
      [](auto& p) { p.sample.indicators.back().reset(); },
      [](auto& p) { timing(p.sample, 33).availability = VehicleAvailability::unavailable; },
      [](auto& p) { timing(p.sample, 33).origin = VehicleObservationOrigin::simulation; },
      [](auto& p) { p.sample.simulated = true; },
      [](auto& p) {
        p.sample.indicators.back().reset();
        timing(p.sample, 33).availability = VehicleAvailability::stale;
      }};
  for (const auto& mutate : invalid) {
    auto bad = original;
    bad.sequence = 2;
    bad.sample.sampled_at = at(1010);
    number(bad, 0, 12, 1010);
    mutate(bad);
    rejects([&] { receiver.accept(bad, at(1200)); });
    unchanged(before, receiver.sample(at(1000)));
  }
  auto valid = original;
  valid.sequence = 2;
  valid.sample.sampled_at = at(1010);
  number(valid, 0, 12, 1010);
  receiver.accept(valid, at(1010));
  const auto after = receiver.sample(at(1010));
  check(after.speed_kmh == 12 && after.indicators.back() == IndicatorState::off,
        "invalid later field consumed sequence, watermark or host clock");
  for (const auto& definition : reading_definitions)
    check(bool(after.*definition.member), "bounded complete snapshot lost reading");
  for (const auto& definition : indicator_definitions)
    check(after.indicator(definition.id) == IndicatorState::off, "bounded snapshot lost lamp");
}
void ordering_atomicity() {
  VehicleObservationReceiver receiver;
  auto original = complete();
  receiver.accept(original, at(1000));
  const auto before = receiver.sample(at(1000));
  auto next = original;
  next.sequence = 2;
  next.sample.sampled_at = at(1010);
  number(next, 0, 12, 1010);
  lamp(next, Indicator::glow_plug, IndicatorState::off, 1010);
  auto bad = next;
  timing(bad.sample, 33).acquired_at.reset();
  bad.sample.indicators.back().reset();
  timing(bad.sample, 33).availability = VehicleAvailability::disconnected;
  check(valid_vehicle_observation_packet(bad, at(1100)),
        "dropped-watermark fixture should be semantically valid");
  rejects([&] { receiver.accept(bad, at(1100)); });
  unchanged(before, receiver.sample(at(1000)));
  bad = next;
  lamp(bad, Indicator::glow_plug, IndicatorState::on, 1000);
  check(valid_vehicle_observation_packet(bad, at(1100)),
        "equal-time conflict fixture should be semantically valid");
  rejects([&] { receiver.accept(bad, at(1100)); });
  unchanged(before, receiver.sample(at(1000)));
  receiver.accept(next, at(1010));
  check(receiver.sample(at(1010)).speed_kmh == 12,
        "later ordering failure consumed an earlier acquisition or sequence");
}
void equal_date_and_loss() {
  VehicleObservationReceiver receiver;
  auto original = packet();
  number(original, 0, 10, 1000);
  number(original, 2, std::nullopt, 1000, VehicleAvailability::unavailable);
  lamp(original, Indicator::low_beam, IndicatorState::off, 1000);
  receiver.accept(original, at(1000));
  auto next = original;
  next.sequence = 2;
  next.sample.sampled_at = at(1010);
  auto bad = next;
  number(bad, 0, 11, 1000);
  rejects([&] { receiver.accept(bad, at(1010)); });
  bad = next;
  timing(bad.sample, 0).origin = VehicleObservationOrigin::calculated;
  rejects([&] { receiver.accept(bad, at(1010)); });
  bad = next;
  number(bad, 0, std::nullopt, 1000, VehicleAvailability::unavailable);
  rejects([&] { receiver.accept(bad, at(1010)); });
  bad = next;
  number(bad, 2, 20, 1000);
  rejects([&] { receiver.accept(bad, at(1010)); });
  bad = next;
  lamp(bad, Indicator::low_beam, IndicatorState::on, 1000);
  rejects([&] { receiver.accept(bad, at(1010)); });
  receiver.accept(next, at(1010));
  check(receiver.sample(at(1010)).speed_kmh == 10, "identical higher-sequence snapshot rejected");
  auto lost = next;
  lost.sequence = 3;
  lost.sample.sampled_at = at(1020);
  for (const auto& definition : reading_definitions)
    (lost.sample.*definition.member).reset();
  for (auto& indicator : lost.sample.indicators)
    indicator.reset();
  for (std::size_t i = 0; i < legacy_vehicle_observation_capacity; ++i)
    timing(lost.sample, i).availability = VehicleAvailability::disconnected;
  receiver.accept(lost, at(1020));
  const auto disconnected = receiver.sample(at(1020));
  check(!disconnected.speed_kmh && timing(disconnected, 0).acquired_at == at(1000) &&
            timing(disconnected, 0).availability == VehicleAvailability::disconnected,
        "source loss erased watermark or retained value");
  auto revival = next;
  revival.sequence = 4;
  revival.sample.sampled_at = at(1030);
  check(valid_vehicle_observation_packet(revival, at(1030)),
        "same-date revival fixture is not fresh at publication");
  rejects([&] { receiver.accept(revival, at(1030)); });
  bad = lost;
  bad.sequence = 4;
  bad.sample.sampled_at = at(1030);
  timing(bad.sample, 0).acquired_at.reset();
  rejects([&] { receiver.accept(bad, at(1030)); });
  bad = lost;
  bad.sequence = 4;
  bad.sample.sampled_at = at(1030);
  timing(bad.sample, 0).acquired_at = at(999);
  rejects([&] { receiver.accept(bad, at(1030)); });
  bad = lost;
  bad.sequence = 4;
  bad.sample.sampled_at = at(1030);
  number(bad, 0, std::nullopt, 1000, VehicleAvailability::unavailable);
  rejects([&] {
    receiver.accept(bad, at(1030));
  });  // Original available history survives disconnection.
  bad = lost;
  bad.sequence = 4;
  bad.sample.sampled_at = at(1030);
  number(bad, 2, 20, 1000);
  rejects([&] {
    receiver.accept(bad, at(1030));
  });  // Original explicit unavailable history also survives.
  auto recovery = lost;
  recovery.sequence = 4;
  recovery.sample.sampled_at = at(1040);
  number(recovery, 0, 14, 1025);
  receiver.accept(recovery, at(1100));
  auto current = receiver.sample(at(1100));
  check(current.speed_kmh == 14 && current.sampled_at == at(1040) &&
            timing(current, 0).acquired_at == at(1025),
        "delayed newer acquisition could not recover or was redated at receipt");
  current = receiver.sample(at(1525));
  check(!current.speed_kmh && timing(current, 0).availability == VehicleAvailability::stale,
        "delayed recovery lifetime began at receipt");
}
void stale_history() {
  VehicleObservationReceiver receiver;
  auto original = packet();
  number(original, 0, 10, 1000);
  timing(original.sample, 0).maximum_age = 100ms;
  receiver.accept(original, at(1000));
  receiver.sample(at(1100));  // Local filtering must retain original equal-date comparison data.
  auto bad = original;
  bad.sequence = 2;
  number(bad, 0, 11, 1000);
  rejects([&] { receiver.accept(bad, at(1100)); });
  auto identical = original;
  identical.sequence = 2;
  receiver.accept(identical, at(1100));
  check(!receiver.sample(at(1100)).speed_kmh,
        "same original publication revived a locally expired field");
  auto stale = original;
  stale.sequence = 3;
  stale.sample.sampled_at = at(1100);
  number(stale, 0, std::nullopt, 1000, VehicleAvailability::stale);
  receiver.accept(stale, at(1100));
  auto replay = original;
  replay.sequence = 4;
  rejects([&] { receiver.accept(replay, at(1100)); });
  bad = stale;
  bad.sequence = 4;
  number(bad, 0, std::nullopt, 1000, VehicleAvailability::unavailable);
  rejects([&] { receiver.accept(bad, at(1100)); });
  bad = stale;
  bad.sequence = 4;
  timing(bad.sample, 0).origin = VehicleObservationOrigin::calculated;
  rejects([&] { receiver.accept(bad, at(1100)); });
  // A newly observed value may already be stale at receipt, but its acquisition
  // watermark still advances and prevents a later packet from walking back.
  auto delayed = stale;
  delayed.sequence = 4;
  delayed.sample.sampled_at = at(1120);
  number(delayed, 0, 12, 1110);
  receiver.accept(delayed, at(1300));
  check(!receiver.sample(at(1300)).speed_kmh, "expired-at-receipt source value displayed");
  bad = delayed;
  bad.sequence = 5;
  number(bad, 0, 13, 1109);
  rejects([&] { receiver.accept(bad, at(1300)); });
  auto fresh = delayed;
  fresh.sequence = 5;
  fresh.sample.sampled_at = at(1300);
  number(fresh, 0, 14, 1290);
  receiver.accept(fresh, at(1300));
  check(receiver.sample(at(1300)).speed_kmh == 14, "fresh later acquisition failed to recover");
}
void unknown_history() {
  VehicleObservationReceiver receiver;
  auto lost = packet();
  number(lost, 0, std::nullopt, 1000, VehicleAvailability::disconnected);
  number(lost, 2, std::nullopt, 1000, VehicleAvailability::unavailable);
  receiver.accept(lost, at(1000));
  auto next = lost;
  next.sequence = 2;
  next.sample.sampled_at = at(1010);
  number(next, 0, 0, 1000);
  rejects([&] {
    receiver.accept(next, at(1010));
  });  // Unknown disconnected history cannot produce fresh data.
  next = lost;
  next.sequence = 2;
  next.sample.sampled_at = at(1010);
  number(next, 0, std::nullopt, 1000, VehicleAvailability::unavailable);
  receiver.accept(next, at(1010));
  auto bad = next;
  bad.sequence = 3;
  bad.sample.sampled_at = at(1500);
  number(bad, 0, std::nullopt, 1000, VehicleAvailability::stale);
  check(valid_vehicle_observation_packet(bad, at(1500)),
        "explicit unavailable-to-stale fixture is structurally invalid");
  rejects([&] {
    receiver.accept(bad, at(1500));
  });  // Now-known explicit unavailable history is retained.
  auto available = next;
  available.sequence = 3;
  available.sample.sampled_at = at(1020);
  number(available, 0, 0, 1020);
  number(available, 2, 40, 1020);
  receiver.accept(available, at(1020));
  check(receiver.sample(at(1020)).speed_kmh == 0 && receiver.sample(at(1020)).fuel_percent == 40,
        "newer acquisition could not replace explicit unavailable or disconnected history");
  VehicleObservationReceiver stale_first;
  auto first = packet(1500);
  number(first, 0, std::nullopt, 1000, VehicleAvailability::stale);
  stale_first.accept(first, at(1500));
  auto incompatible = first;
  incompatible.sequence = 2;
  number(incompatible, 0, std::nullopt, 1000, VehicleAvailability::unavailable);
  rejects([&] { stale_first.accept(incompatible, at(1500)); });
}
void generation_and_ages() {
  VehicleObservationReceiver receiver;
  auto original = packet();
  number(original, 0, 10, 1000);
  receiver.accept(original, at(1000));
  receiver.sample(at(5000));
  auto bad = original;
  bad.sequence = 2;
  bad.sample.observation_timing->started_at = at(999);
  rejects([&] { receiver.accept(bad, at(5010)); });
  bad = packet(1100, 1, 2, 999);
  rejects([&] { receiver.accept(bad, at(5010)); });
  bad = packet(1100, 1, 2, 1050);
  timing(bad.sample, 33).maximum_age = 501ms;
  rejects([&] { receiver.accept(bad, at(5010)); });
  // Restart follows last accepted publication (1000), even though the local
  // consumer has already polled at 5000. Policy remains fixed across restart.
  auto restart = packet(1100, 1, 2, 1050);
  number(restart, 0, 0, 1050);
  receiver.accept(restart, at(5010));
  auto current = receiver.sample(at(5010));
  check(current.observation_timing->generation == 2 && current.sampled_at == at(1100) &&
            !current.speed_kmh && timing(current, 0).acquired_at == at(1050) &&
            !timing(current, 2).acquired_at,
        "delayed generation rejected or carried old watermarks");
  rejects([&] { receiver.accept(original, at(5010)); });
  bad = restart;
  bad.sequence = 2;
  bad.sample.sampled_at = at(1101);
  timing(bad.sample, 33).maximum_age = 501ms;
  rejects([&] { receiver.accept(bad, at(5010)); });
  bad = restart;
  bad.sequence = 2;
  bad.sample.observation_timing->started_at = at(1051);
  rejects([&] { receiver.accept(bad, at(5010)); });
  auto valid = restart;
  valid.sequence = 2;
  valid.sample.sampled_at = at(1101);
  receiver.accept(valid, at(5010));
  bad = packet(1200, 1, 3, 1100);
  rejects([&] {
    receiver.accept(bad, at(5010));
  });  // Must follow latest accepted source publication 1101.
  auto third = packet(1200, 1, 3, 1101);
  receiver.accept(third, at(5010));
  check(receiver.sample(at(5010)).observation_timing->generation == 3,
        "restart at exact publication boundary failed");
}
void full_resolution_ordering() {
  VehicleObservationReceiver receiver;
  auto first = packet();
  first.sample.sampled_at += 600ns;
  number(first, 0, 10, 1000);
  timing(first.sample, 0).acquired_at = at(1000) + 400ns;
  receiver.accept(first, at(1001));
  const auto before = receiver.sample(at(1001));
  auto next = first;
  next.sequence = 2;
  next.sample.sampled_at = at(1000) + 500ns;
  rejects([&] { receiver.accept(next, at(1001)); });
  unchanged(before, receiver.sample(at(1001)));
  next = first;
  next.sequence = 2;
  timing(next.sample, 0).acquired_at = at(1000) + 399ns;
  rejects([&] { receiver.accept(next, at(1001)); });
  next = first;
  next.sequence = 2;
  receiver.accept(next, at(1001));  // Equal publication with higher sequence is valid.
  rejects([&] { receiver.sample(at(1001) - 1ns); });
  next.sequence = 3;
  rejects([&] { receiver.accept(next, at(1001) - 1ns); });
  receiver.accept(next, at(1001));
  auto restarted = packet(1001, 1, 2, 1000);
  restarted.sample.observation_timing->started_at = at(1000) + 599ns;
  rejects([&] { receiver.accept(restarted, at(1001)); });
  restarted.sample.observation_timing->started_at = at(1000) + 600ns;
  receiver.accept(restarted, at(1001));
}
void transmission_lifetime_and_atomicity() {
  auto original = complete();
  original.version = VehicleObservationWireVersion::hvo2;
  const auto forward = TransmissionState{GearboxType::automatic, TransmissionSelector::drive,
                                         EngagedGear{EngagedGearKind::forward, 3}};
  original.sample.transmission = forward;
  timing(original.sample, transmission_observation_index) = {
      at(1000) + 1ns, 100ms, VehicleObservationOrigin::simulation, VehicleAvailability::available};
  original.sample.sampled_at = at(1000) + 2ns;
  simulation_flag(original.sample);
  VehicleObservationReceiver receiver;
  receiver.accept(original, at(1001));
  const auto before = receiver.sample(at(1001));
  check(before.transmission == forward && before.simulated && before.sampled_at == at(1000) + 2ns,
        "HVO2 transmission receipt changed compound state, provenance or source date");
  auto next = original;
  next.sequence = 2;
  next.sample.sampled_at = at(1010);
  number(next, 0, 12, 1010);
  std::vector<std::function<void(VehicleObservationPacket&)>> invalid{
      [](auto& p) { p.sample.transmission->engaged_gear->forward_number = 4; },
      [](auto& p) {
        timing(p.sample, transmission_observation_index).origin =
            VehicleObservationOrigin::measured;
        simulation_flag(p.sample);
      },
      [](auto& p) { timing(p.sample, transmission_observation_index).maximum_age = 101ms; },
      [](auto& p) { timing(p.sample, transmission_observation_index).acquired_at = at(1000); },
      [](auto& p) {
        timing(p.sample, transmission_observation_index).acquired_at.reset();
        p.sample.transmission.reset();
        timing(p.sample, transmission_observation_index).availability =
            VehicleAvailability::disconnected;
        simulation_flag(p.sample);
      },
      [](auto& p) {
        p.sample.transmission->selector = TransmissionSelector::park;
      }};
  for (const auto& mutate : invalid) {
    auto bad = next;
    mutate(bad);
    rejects([&] { receiver.accept(bad, at(1050)); });
    unchanged(before, receiver.sample(at(1001)));
  }
  receiver.accept(next, at(1010));
  check(receiver.sample(at(1010)).speed_kmh == 12,
        "transmission failure consumed earlier speed or sequence");
  auto current = receiver.sample(at(1100));
  check(current.transmission.has_value(), "nanosecond side of transmission expiry lost");
  current = receiver.sample(at(1100) + 1ns);
  check(!current.transmission && current.speed_kmh == 12 && current.simulated &&
            timing(current, transmission_observation_index).acquired_at == at(1000) + 1ns &&
            timing(current, transmission_observation_index).availability ==
                VehicleAvailability::stale &&
            current.sampled_at == at(1010),
        "transmission expiry changed a peer, original date or historical simulation provenance");
  auto lost = next;
  lost.sequence = 3;
  lost.sample.sampled_at = at(1110);
  lost.sample.transmission.reset();
  timing(lost.sample, transmission_observation_index).availability =
      VehicleAvailability::disconnected;
  receiver.accept(lost, at(1110));
  auto recovery = lost;
  recovery.sequence = 4;
  recovery.sample.sampled_at = at(1120);
  recovery.sample.transmission = TransmissionState{
      GearboxType::manual, TransmissionSelector::manual, EngagedGear{EngagedGearKind::forward, 4}};
  timing(recovery.sample, transmission_observation_index).acquired_at = at(1115) + 7ns;
  timing(recovery.sample, transmission_observation_index).availability =
      VehicleAvailability::available;
  receiver.accept(recovery, at(1150));
  check(receiver.sample(at(1150)).transmission == recovery.sample.transmission &&
            timing(receiver.sample(at(1150)), transmission_observation_index).acquired_at ==
                at(1115) + 7ns,
        "delayed newer transmission could not recover without redating");
  auto bad = recovery;
  bad.sequence = 5;
  bad.sample.sampled_at = at(1151);
  timing(bad.sample, transmission_observation_index).acquired_at = at(1115) + 6ns;
  rejects([&] { receiver.accept(bad, at(1151)); });
  VehicleObservationReceiver sticky;
  auto live = packet();
  live.version = VehicleObservationWireVersion::hvo2;
  live.sample.transmission = forward;
  timing(live.sample, transmission_observation_index) = {
      at(1000), 500ms, VehicleObservationOrigin::measured, VehicleAvailability::available};
  sticky.accept(live, at(1000));
  auto disconnected = live;
  disconnected.sequence = 2;
  disconnected.sample.sampled_at = at(1010);
  disconnected.sample.transmission.reset();
  timing(disconnected.sample, transmission_observation_index).availability =
      VehicleAvailability::disconnected;
  sticky.accept(disconnected, at(1010));
  auto same = live;
  same.sequence = 3;
  same.sample.sampled_at = at(1020);
  check(valid_vehicle_observation_packet(same, at(1020)),
        "sticky transmission fixture is not valid at publication");
  rejects([&] { sticky.accept(same, at(1020)); });
  unchanged(sticky.sample(at(1010)), disconnected.sample);
}
void version_is_atomic_across_generation() {
  auto v1 = packet();
  VehicleObservationReceiver legacy;
  legacy.accept(v1, at(1000));
  const auto old = legacy.sample(at(1000));
  auto v2 = v1;
  v2.version = VehicleObservationWireVersion::hvo2;
  v2.sequence = 2;
  v2.sample.sampled_at = at(1010);
  number(v2, 0, 42, 1010);
  rejects([&] { legacy.accept(v2, at(1100)); });
  unchanged(old, legacy.sample(at(1000)));
  v2.sample.observation_timing->generation = 2;
  v2.sample.observation_timing->started_at = at(1010);
  v2.sequence = 1;
  rejects([&] { legacy.accept(v2, at(1100)); });
  unchanged(old, legacy.sample(at(1000)));
  auto valid = v1;
  valid.sequence = 2;
  valid.sample.sampled_at = at(1010);
  legacy.accept(valid, at(1010));
  const auto empty_transmission = legacy.sample(at(1010));
  check(!empty_transmission.transmission &&
            !timing(empty_transmission, transmission_observation_index).acquired_at &&
            timing(empty_transmission, transmission_observation_index).availability ==
                VehicleAvailability::unavailable,
        "HVO1 polling fabricated transmission freshness");
  VehicleObservationReceiver extended;
  auto first = packet();
  first.version = VehicleObservationWireVersion::hvo2;
  extended.accept(first, at(1000));
  const auto before = extended.sample(at(1000));
  auto downgrade = packet(1010, 2);
  number(downgrade, 0, 42, 1010);
  rejects([&] { extended.accept(downgrade, at(1100)); });
  unchanged(before, extended.sample(at(1000)));
  downgrade = packet(1010, 1, 2, 1010);
  rejects([&] { extended.accept(downgrade, at(1100)); });
  unchanged(before, extended.sample(at(1000)));
  // Neither rejected version consumes the new generation or local clock.
  auto restart = downgrade;
  restart.version = VehicleObservationWireVersion::hvo2;
  extended.accept(restart, at(1010));
  check(extended.sample(at(1010)).observation_timing->generation == 2,
        "wire-version failure consumed restart generation or receipt time");
  VehicleObservationReceiver initially_invalid;
  auto bad = first;
  bad.version = static_cast<VehicleObservationWireVersion>(99);
  rejects([&] { initially_invalid.accept(bad, at(1100)); });
  initially_invalid.accept(v1, at(1000));
  check(initially_invalid.initialized(), "invalid first version consumed the family or host clock");
}
DriveModeIdentifier mode(std::string_view text) {
  const auto value = parse_drive_mode_identifier(text);
  check(value.has_value(), "invalid test mode identifier");
  return *value;
}
DriveModeCatalog catalog(std::string_view text) {
  const auto value = parse_drive_mode_catalog_key(text);
  check(value.has_value(), "invalid test mode catalog");
  return *value;
}
void drive_mode_lifetime_history_and_atomicity() {
  auto original = complete();
  original.version = VehicleObservationWireVersion::hvo3;
  original.sample.drive_mode_catalog = catalog("comfort,sport,track-plus");
  original.sample.drive_mode = mode("comfort");
  timing(original.sample, drive_mode_observation_index) = {
      at(1000) + 1ns, 100ms, VehicleObservationOrigin::simulation, VehicleAvailability::available};
  original.sample.sampled_at = at(1000) + 2ns;
  simulation_flag(original.sample);
  VehicleObservationReceiver receiver;
  receiver.accept(original, at(1001));
  const auto before = receiver.sample(at(1001));
  check(before.drive_mode == mode("comfort") &&
            before.drive_mode_catalog == original.sample.drive_mode_catalog && before.simulated,
        "HVO3 lost active mode, static catalog or simulation provenance");
  auto next = original;
  next.sequence = 2;
  next.sample.sampled_at = at(1010);
  number(next, 0, 42, 1010);
  std::vector<std::function<void(VehicleObservationPacket&)>> invalid{
      [](auto& p) { p.sample.drive_mode = mode("sport"); },
      [](auto& p) {
        timing(p.sample, drive_mode_observation_index).origin =
            VehicleObservationOrigin::calculated;
        simulation_flag(p.sample);
      },
      [](auto& p) { timing(p.sample, drive_mode_observation_index).maximum_age = 101ms; },
      [](auto& p) { timing(p.sample, drive_mode_observation_index).acquired_at = at(1000); },
      [](auto& p) {
        p.sample.drive_mode.reset();
        timing(p.sample, drive_mode_observation_index).acquired_at.reset();
        timing(p.sample, drive_mode_observation_index).availability =
            VehicleAvailability::disconnected;
        simulation_flag(p.sample);
      },
      [](auto& p) { p.sample.drive_mode = mode("unsupported"); },
      [](auto& p) { p.sample.drive_mode_catalog = catalog("sport,comfort,track-plus"); },
      [](auto& p) {
        p.sample.drive_mode_catalog = catalog("comfort,sport");
      }};
  for (const auto& mutate : invalid) {
    auto bad = next;
    mutate(bad);
    rejects([&] { receiver.accept(bad, at(1050)); });
    unchanged(before, receiver.sample(at(1001)));
  }
  receiver.accept(next, at(1010));
  check(receiver.sample(at(1010)).speed_kmh == 42,
        "mode failure consumed peer acquisition or sequence");
  check(receiver.sample(at(1100)).drive_mode.has_value(), "nanosecond side of mode expiry failed");
  const auto stale = receiver.sample(at(1100) + 1ns);
  check(
      !stale.drive_mode && stale.drive_mode_catalog == original.sample.drive_mode_catalog &&
          stale.speed_kmh == 42 && stale.simulated &&
          timing(stale, drive_mode_observation_index).acquired_at == at(1000) + 1ns &&
          timing(stale, drive_mode_observation_index).availability == VehicleAvailability::stale &&
          stale.sampled_at == at(1010),
      "mode expiry erased source catalog/history or renewed publication");
  auto lost = next;
  lost.sequence = 3;
  lost.sample.sampled_at = at(1110);
  lost.sample.drive_mode.reset();
  timing(lost.sample, drive_mode_observation_index).availability =
      VehicleAvailability::disconnected;
  receiver.accept(lost, at(1110));
  auto recovery = lost;
  recovery.sequence = 4;
  recovery.sample.sampled_at = at(1120);
  recovery.sample.drive_mode = mode("sport");
  timing(recovery.sample, drive_mode_observation_index).acquired_at = at(1115) + 7ns;
  timing(recovery.sample, drive_mode_observation_index).availability =
      VehicleAvailability::available;
  receiver.accept(recovery, at(1150));
  const auto fresh = receiver.sample(at(1150));
  check(fresh.drive_mode == mode("sport") &&
            timing(fresh, drive_mode_observation_index).acquired_at == at(1115) + 7ns &&
            fresh.sampled_at == at(1120),
        "delayed newer mode could not recover or acquired receipt time");
  auto bad = recovery;
  bad.sequence = 5;
  bad.sample.sampled_at = at(1151);
  timing(bad.sample, drive_mode_observation_index).acquired_at = at(1115) + 6ns;
  rejects([&] { receiver.accept(bad, at(1151)); });
  unchanged(fresh, receiver.sample(at(1150)));
  VehicleObservationReceiver sticky;
  auto live = packet();
  live.version = VehicleObservationWireVersion::hvo3;
  live.sample.drive_mode_catalog = catalog("comfort,sport");
  live.sample.drive_mode = mode("comfort");
  timing(live.sample, drive_mode_observation_index) = {
      at(1000), 500ms, VehicleObservationOrigin::measured, VehicleAvailability::available};
  sticky.accept(live, at(1000));
  auto disconnected = live;
  disconnected.sequence = 2;
  disconnected.sample.sampled_at = at(1010);
  disconnected.sample.drive_mode.reset();
  timing(disconnected.sample, drive_mode_observation_index).availability =
      VehicleAvailability::disconnected;
  sticky.accept(disconnected, at(1010));
  auto same = live;
  same.sequence = 3;
  same.sample.sampled_at = at(1020);
  check(valid_vehicle_observation_packet(same, at(1020)),
        "sticky mode fixture must be structurally fresh");
  rejects([&] { sticky.accept(same, at(1020)); });
  unchanged(disconnected.sample, sticky.sample(at(1010)));
}
void catalog_and_version_are_immutable_across_restarts() {
  VehicleObservationReceiver receiver;
  auto first = packet();
  first.version = VehicleObservationWireVersion::hvo3;
  first.sample.drive_mode_catalog = catalog("comfort,sport,track-plus");
  receiver.accept(first, at(1000));
  const auto before = receiver.sample(at(1000));
  const std::array<std::optional<DriveModeCatalog>, 5> changed{
      std::nullopt, catalog("sport,comfort,track-plus"), catalog("comfort,sport"),
      catalog("comfort,sport,track-plus,eco"), catalog("comfort,sport,track")};
  for (const auto& catalog_value : changed) {
    for (const auto generation : {1u, 2u}) {
      auto bad = packet(1010, generation == 1 ? 2 : 1, generation, generation == 1 ? 1000 : 1010);
      bad.version = VehicleObservationWireVersion::hvo3;
      bad.sample.drive_mode_catalog = catalog_value;
      number(bad, 0, 42, 1010);
      check(valid_vehicle_observation_packet(bad, at(1100)),
            "catalog mutation fixture is structurally invalid");
      rejects([&] { receiver.accept(bad, at(1100)); });
      unchanged(before, receiver.sample(at(1000)));
    }
  }
  for (const auto version :
       {VehicleObservationWireVersion::hvo1, VehicleObservationWireVersion::hvo2}) {
    auto bad = packet(1010, 1, 2, 1010);
    bad.version = version;
    number(bad, 0, 42, 1010);
    rejects([&] { receiver.accept(bad, at(1100)); });
    unchanged(before, receiver.sample(at(1000)));
    VehicleObservationReceiver old;
    auto initial = packet();
    initial.version = version;
    old.accept(initial, at(1000));
    auto upgrade = first;
    upgrade.sequence = 2;
    upgrade.sample.sampled_at = at(1010);
    rejects([&] { old.accept(upgrade, at(1100)); });
    unchanged(initial.sample, old.sample(at(1000)));
  }
  auto valid = packet(1010, 1, 2, 1010);
  valid.version = VehicleObservationWireVersion::hvo3;
  valid.sample.drive_mode_catalog = first.sample.drive_mode_catalog;
  number(valid, 0, 42, 1010);
  receiver.accept(valid, at(1010));
  check(receiver.sample(at(1010)).observation_timing->generation == 2 &&
            receiver.sample(at(1010)).speed_kmh == 42,
        "rejected catalog or version consumed restart generation/sequence/receipt clock");
  VehicleObservationReceiver absent;
  auto no_catalog = packet();
  no_catalog.version = VehicleObservationWireVersion::hvo3;
  absent.accept(no_catalog, at(1000));
  auto appears = first;
  appears.sequence = 2;
  appears.sample.sampled_at = at(1010);
  rejects([&] { absent.accept(appears, at(1100)); });
  unchanged(no_catalog.sample, absent.sample(at(1000)));
  appears = first;
  appears.sample.observation_timing->generation = 2;
  appears.sample.observation_timing->started_at = at(1010);
  appears.sample.sampled_at = at(1010);
  rejects([&] { absent.accept(appears, at(1100)); });
  unchanged(no_catalog.sample, absent.sample(at(1000)));
  VehicleObservationReceiver invalid_first;
  auto bad = first;
  bad.sample.drive_mode_catalog->count = 33;
  rejects([&] { invalid_first.accept(bad, at(1100)); });
  invalid_first.accept(first, at(1000));
  check(invalid_first.sample(at(1000)).drive_mode_catalog == first.sample.drive_mode_catalog,
        "invalid first catalog consumed catalog ownership or host clock");
}
TripState trip_state(std::string_view text) {
  const auto value = parse_trip_state_key(text);
  check(value.has_value(), "invalid test trip state");
  return *value;
}
VehicleObservationPacket trip_packet(std::int64_t published = 1000, std::uint64_t sequence = 1,
                                     std::uint64_t generation = 1, std::int64_t started = 1000) {
  auto value = packet(published, sequence, generation, started);
  value.version = VehicleObservationWireVersion::hvo4;
  return value;
}
void trip(VehicleObservationPacket& packet, const TripState& state, Clock::time_point acquired) {
  packet.sample.trip = state;
  timing(packet.sample, trip_observation_index).acquired_at = acquired;
  timing(packet.sample, trip_observation_index).availability = VehicleAvailability::available;
}
void heading(VehicleObservationPacket& packet, const HeadingState& state,
             Clock::time_point acquired) {
  packet.sample.heading = state;
  timing(packet.sample, heading_observation_index).acquired_at = acquired;
  timing(packet.sample, heading_observation_index).availability = VehicleAvailability::available;
}
void trip_heading_expiry_and_atomic_progress() {
  auto original = trip_packet();
  trip(original, trip_state("current/1/100/1000/500/-/-/10"), at(1000) + 1ns);
  heading(
      original,
      HeadingState{359.999, HeadingReference::magnetic_north, HeadingSource::vehicle_orientation},
      at(1000) + 2ns);
  timing(original.sample, trip_observation_index).maximum_age = 1s;
  timing(original.sample, heading_observation_index).maximum_age = 100ms;
  timing(original.sample, trip_observation_index).origin = VehicleObservationOrigin::simulation;
  original.sample.sampled_at = at(1000) + 3ns;
  simulation_flag(original.sample);
  VehicleObservationReceiver receiver;
  receiver.accept(original, at(1001));
  const auto before = receiver.sample(at(1001));
  auto bad = original;
  bad.sequence = 2;
  bad.sample.sampled_at = at(1010);
  number(bad, 0, 42, 1010);
  trip(bad, trip_state("current/1/200/1100/600/-/-/10"), at(1010));
  bad.sample.heading->degrees =
      180;  // Coherent, but conflicts with its unchanged original acquisition.
  check(valid_vehicle_observation_packet(bad, at(1050)),
        "late heading-conflict fixture is not statelessly valid");
  rejects([&] { receiver.accept(bad, at(1050)); });
  unchanged(before, receiver.sample(at(1001)));
  auto good = original;
  good.sequence = 2;
  good.sample.sampled_at = at(1010);
  number(good, 0, 43, 1010);
  trip(good, trip_state("current/1/101/1010/510/-/-/9"), at(1010));
  receiver.accept(good, at(1010));
  check(receiver.sample(at(1010)).trip == good.sample.trip &&
            receiver.sample(at(1010)).speed_kmh == 43,
        "failed later field consumed trip progress, sequence or earlier numeric acquisition");
  check(receiver.sample(at(1100) + 1ns).heading.has_value(),
        "heading expired before its exact independent boundary");
  auto current = receiver.sample(at(1100) + 2ns);
  check(!current.heading && current.trip == good.sample.trip && current.simulated &&
            timing(current, heading_observation_index).acquired_at == at(1000) + 2ns &&
            timing(current, heading_observation_index).availability == VehicleAvailability::stale &&
            current.sampled_at == at(1010),
        "heading age was tied to trip/receipt or original qualification date changed");
  current = receiver.sample(at(2010));
  check(!current.trip && current.simulated &&
            timing(current, trip_observation_index).acquired_at == at(1010) &&
            timing(current, trip_observation_index).availability == VehicleAvailability::stale,
        "trip expiry lost historical provenance/date");
  // Local expiry never changes source publication ordering. A fresh later
  // current-trip acquisition retains identity and reset without replaying dates.
  auto recovery = good;
  recovery.sequence = 3;
  recovery.sample = current_vehicle_sample(recovery.sample, at(2020));
  recovery.sample.sampled_at = at(2020);
  trip(recovery, trip_state("current/1/102/2020/520/-/-/8"), at(2015) + 7ns);
  recovery.sample.heading.reset();
  timing(recovery.sample, heading_observation_index).availability = VehicleAvailability::stale;
  receiver.accept(recovery, at(2030));
  check(receiver.sample(at(2030)).trip == recovery.sample.trip &&
            timing(receiver.sample(at(2030)), trip_observation_index).acquired_at == at(2015) + 7ns,
        "newer delayed current-trip acquisition could not recover without redating");
}
void trip_history_survives_missing_loss_and_source_generation() {
  VehicleObservationReceiver receiver;
  auto first = trip_packet();
  trip(first, trip_state("current/2/100/1000/500/-/-/10"), at(1000));
  receiver.accept(first, at(1000));
  auto partial = first;
  partial.sequence = 2;
  partial.sample.sampled_at = at(1010);
  trip(partial, trip_state("current/2/-/1100/-/-/-/-"), at(1010));
  receiver.accept(partial, at(1010));
  auto loss = partial;
  loss.sequence = 3;
  loss.sample.sampled_at = at(1020);
  loss.sample.trip.reset();
  timing(loss.sample, trip_observation_index).availability = VehicleAvailability::disconnected;
  receiver.accept(loss, at(1020));
  const auto before = receiver.sample(at(1020));
  const std::array invalid{
      trip_state("current/1/150/1200/600/-/-/10"),  // Older reset is not a restart.
      trip_state("current/2/99/1200/600/-/-/10"),   // Distance retained through omission.
      trip_state("current/2/101/1099/600/-/-/10"),  // Elapsed retained through loss.
      trip_state("current/2/101/1200/499/-/-/10"),  // Moving retained through omission.
      trip_state("other/3/1/10/5/-/-/10")};         // Source lifetime identity remains fixed.
  for (const auto& state : invalid) {
    auto bad = trip_packet(1040, 1, 2, 1020);
    trip(bad, state, at(1030));
    number(bad, 0, 42, 1030);
    check(valid_vehicle_observation_packet(bad, at(1100)),
          "hidden trip-regression fixture must be structurally valid");
    rejects([&] { receiver.accept(bad, at(1100)); });
    unchanged(before, receiver.sample(at(1020)));
  }
  auto restarted = trip_packet(1040, 1, 2, 1020);
  trip(restarted, trip_state("current/2/101/1200/600/-/-/9"), at(1030));
  receiver.accept(restarted, at(1040));
  check(receiver.sample(at(1040)).trip == restarted.sample.trip,
        "invalid trip regression consumed source generation, sequence or receipt time");
  auto bad = restarted;
  bad.sequence = 2;
  bad.sample.sampled_at = at(1050);
  trip(bad, trip_state("current/3/0/0/0/-/-/-"), at(1030));
  check(valid_vehicle_observation_packet(bad, at(1050)),
        "equal-acquisition reset fixture is structurally invalid");
  rejects([&] { receiver.accept(bad, at(1100)); });
  auto reset = bad;
  timing(reset.sample, trip_observation_index).acquired_at = at(1045) + 1ns;
  receiver.accept(reset, at(1050));
  check(receiver.sample(at(1050)).trip->reset_generation == 3 &&
            receiver.sample(at(1050)).trip->distance_km == 0,
        "coherent new reset with a genuinely newer acquisition failed");
  auto unknown = reset;
  unknown.sequence = 3;
  unknown.sample.sampled_at = at(1060);
  unknown.sample.trip.reset();
  timing(unknown.sample, trip_observation_index).availability = VehicleAvailability::unavailable;
  timing(unknown.sample, trip_observation_index).acquired_at = at(1060);
  receiver.accept(unknown, at(1060));
  auto old_reset = trip_packet(1080, 1, 3, 1060);
  trip(old_reset, trip_state("current/2/200/2000/800/-/-/8"), at(1070));
  rejects([&] { receiver.accept(old_reset, at(1100)); });
  unchanged(unknown.sample, receiver.sample(at(1060)));
  auto valid = old_reset;
  trip(valid, trip_state("current/3/1/20/10/-/-/8"), at(1070));
  receiver.accept(valid, at(1080));
  check(receiver.sample(at(1080)).trip == valid.sample.trip,
        "explicit unavailability reset lifetime trip history or poisoned later recovery");
}
void trip_sticky_loss_and_retained_counter_coherence() {
  VehicleObservationReceiver receiver;
  auto first = trip_packet();
  trip(first, trip_state("current/1/100/1000/500/-/-/10"), at(1000));
  receiver.accept(first, at(1000));
  auto disconnected = first;
  disconnected.sequence = 2;
  disconnected.sample.sampled_at = at(1010);
  disconnected.sample.trip.reset();
  timing(disconnected.sample, trip_observation_index).availability =
      VehicleAvailability::disconnected;
  receiver.accept(disconnected, at(1010));
  auto replay = first;
  replay.sequence = 3;
  replay.sample.sampled_at = at(1020);
  check(valid_vehicle_observation_packet(replay, at(1020)),
        "sticky trip replay must still be fresh at publication");
  rejects([&] { receiver.accept(replay, at(1020)); });
  unchanged(disconnected.sample, receiver.sample(at(1010)));
  auto equal_reset = replay;
  trip(equal_reset, trip_state("current/2/0/0/0/-/-/-"), at(1000));
  rejects([&] { receiver.accept(equal_reset, at(1020)); });
  auto fresh = replay;
  trip(fresh, trip_state("current/1/101/1020/520/-/-/9"), at(1015));
  receiver.accept(fresh, at(1020));
  auto duplicate = fresh;
  duplicate.sequence = 4;
  receiver.accept(duplicate, at(1020));
  rejects([&] { receiver.accept(duplicate, at(1020)); });
  VehicleObservationReceiver partial;
  auto moving = trip_packet();
  trip(moving, trip_state("current/1/-/-/300/-/-/-"), at(1000));
  partial.accept(moving, at(1000));
  auto too_short = moving;
  too_short.sequence = 2;
  too_short.sample.sampled_at = at(1010);
  trip(too_short, trip_state("current/1/-/250/-/-/-/-"), at(1010));
  check(valid_vehicle_observation_packet(too_short, at(1010)),
        "partial retained-counter fixture is statelessly invalid");
  rejects([&] { partial.accept(too_short, at(1050)); });
  unchanged(moving.sample, partial.sample(at(1000)));
  auto coherent = too_short;
  trip(coherent, trip_state("current/1/-/350/-/-/-/-"), at(1010));
  partial.accept(coherent, at(1010));
  check(partial.sample(at(1010)).trip == coherent.sample.trip,
        "failed partial counter relation consumed history or sequence");
}
void trip_old_acquisition_cannot_return_through_generation() {
  const auto state = trip_state("current/1/100/1000/500/-/-/10");
  auto first = trip_packet();
  trip(first, state, at(1000));
  VehicleObservationReceiver receiver;
  receiver.accept(first, at(1000));
  auto lost = first;
  lost.sequence = 2;
  lost.sample.trip.reset();
  timing(lost.sample, trip_observation_index).availability = VehicleAvailability::disconnected;
  receiver.accept(lost, at(1000));
  const auto before = receiver.sample(at(1000));
  auto revived = trip_packet(1000, 1, 2, 1000);
  trip(revived, state, at(1000));
  check(valid_vehicle_observation_packet(revived, at(1001)),
        "restart same-date replay must be structurally valid");
  rejects([&] { receiver.accept(revived, at(1050)); });
  unchanged(before, receiver.sample(at(1000)));
  auto changed_origin = revived;
  timing(changed_origin.sample, trip_observation_index).origin =
      VehicleObservationOrigin::calculated;
  rejects([&] { receiver.accept(changed_origin, at(1050)); });
  unchanged(before, receiver.sample(at(1000)));
  // An honest new source can start with unknown trip and no old acquisition.
  // Current-trip lifetime history survives without injecting that old date
  // into a snapshot whose generation may have begun after it.
  auto empty = trip_packet(1000, 1, 2, 1000);
  receiver.accept(empty, at(1000));
  const auto unknown = receiver.sample(at(1000));
  check(
      !unknown.trip && !timing(unknown, trip_observation_index).acquired_at &&
          timing(unknown, trip_observation_index).availability == VehicleAvailability::unavailable,
      "generation restart fabricated an old trip date into an honest unknown snapshot");
  revived.sequence = 2;
  rejects([&] { receiver.accept(revived, at(1050)); });
  unchanged(unknown, receiver.sample(at(1000)));
  revived.sample.sampled_at = at(1000) + 2ns;
  timing(revived.sample, trip_observation_index).acquired_at = at(1000) + 1ns;
  receiver.accept(revived, at(1001));
  check(receiver.sample(at(1001)).trip == state &&
            timing(receiver.sample(at(1001)), trip_observation_index).acquired_at == at(1000) + 1ns,
        "genuinely newer trip acquisition could not clear withdrawal through a restart");
  // A generation change itself requires a new acquisition, even if no prior
  // withdrawal was sent and the old observation would still be fresh by age.
  VehicleObservationReceiver direct;
  direct.accept(first, at(1000));
  auto old = trip_packet(1000, 1, 2, 1000);
  trip(old, state, at(1000));
  rejects([&] { direct.accept(old, at(1050)); });
  unchanged(first.sample, direct.sample(at(1000)));
  old.sample.sampled_at = at(1000) + 2ns;
  timing(old.sample, trip_observation_index).acquired_at = at(1000) + 1ns;
  direct.accept(old, at(1001));
  check(direct.sample(at(1001)).trip == state,
        "rejected restart poisoned later fresh trip acquisition");
  // Empty first generations do not claim a trip identity or block its first
  // qualified observation. Only a known identity participates in sticky loss.
  VehicleObservationReceiver initially_unknown;
  auto no_trip = trip_packet();
  initially_unknown.accept(no_trip, at(1000));
  auto known = first;
  known.sequence = 2;
  initially_unknown.accept(known, at(1000));
  check(initially_unknown.sample(at(1000)).trip == state,
        "initial unknown snapshot bound or poisoned a current-trip identity");
}
void hvo4_version_and_catalog_pinning() {
  auto first = trip_packet();
  first.sample.drive_mode_catalog = catalog("comfort,sport");
  VehicleObservationReceiver receiver;
  receiver.accept(first, at(1000));
  const auto before = receiver.sample(at(1000));
  for (const auto version :
       {VehicleObservationWireVersion::hvo1, VehicleObservationWireVersion::hvo2,
        VehicleObservationWireVersion::hvo3}) {
    auto downgrade = packet(1010, 1, 2, 1010);
    downgrade.version = version;
    rejects([&] { receiver.accept(downgrade, at(1100)); });
    unchanged(before, receiver.sample(at(1000)));
    VehicleObservationReceiver old;
    auto original = packet();
    original.version = version;
    old.accept(original, at(1000));
    auto upgrade = first;
    upgrade.sequence = 2;
    upgrade.sample.sampled_at = at(1010);
    rejects([&] { old.accept(upgrade, at(1100)); });
    unchanged(original.sample, old.sample(at(1000)));
  }
  auto changed = trip_packet(1010, 1, 2, 1010);
  changed.sample.drive_mode_catalog = catalog("sport,comfort");
  rejects([&] { receiver.accept(changed, at(1100)); });
  unchanged(before, receiver.sample(at(1000)));
  changed.sample.drive_mode_catalog = first.sample.drive_mode_catalog;
  receiver.accept(changed, at(1010));
  check(receiver.sample(at(1010)).observation_timing->generation == 2,
        "catalog/version failure consumed valid HVO4 restart");
}
void sequence_and_owner() {
  VehicleObservationReceiver receiver;
  auto value = packet();
  receiver.accept(value, at(1000));
  rejects([&] { receiver.accept(value, at(1001)); });
  auto maximum = value;
  maximum.sequence = std::numeric_limits<std::uint64_t>::max();
  receiver.accept(maximum, at(1000));
  rejects([&] { receiver.accept(maximum, at(1000)); });
  auto wrapped = value;
  wrapped.sequence = 1;
  rejects([&] { receiver.accept(wrapped, at(1000)); });
  auto restart = packet(1001, 1, 2, 1000);
  bool accept_rejected = false, poll_rejected = false;
  std::thread other([&] {
    try {
      receiver.accept(restart, at(1001));
    } catch (const std::invalid_argument&) {
      accept_rejected = true;
    }
    try {
      receiver.sample(at(1001));
    } catch (const std::invalid_argument&) {
      poll_rejected = true;
    }
  });
  other.join();
  check(accept_rejected && poll_rejected &&
            receiver.sample(at(1000)).observation_timing->generation == 1,
        "wrong owner mutated source or host clock");
  receiver.accept(restart, at(1001));
  check(receiver.initialized(), "new generation could not reset exhausted sequence");
  VehicleObservationReceiver empty;
  rejects([&] { empty.sample(Clock::time_point{}); });
  auto future = packet(1002, 1, 3, 1002);
  rejects([&] { empty.accept(future, at(1001)); });
  check(!empty.initialized() && !empty.sample(at(1000)).observation_timing,
        "invalid first packet initialized receiver or consumed clock");
}
}  // namespace
int main() try {
  // Explicit function-pointer type also compiles with GCC's constrained pair
  // constructors; class template deduction must not retain a function type.
  const std::pair<const char*, void (*)()> tests[]{
      {"mixed_expiry", mixed_expiry},
      {"semantic_atomicity", semantic_atomicity},
      {"ordering_atomicity", ordering_atomicity},
      {"equal_date_and_loss", equal_date_and_loss},
      {"stale_history", stale_history},
      {"unknown_history", unknown_history},
      {"generation_and_ages", generation_and_ages},
      {"full_resolution_ordering", full_resolution_ordering},
      {"sequence_and_owner", sequence_and_owner},
      {"transmission_lifetime_and_atomicity", transmission_lifetime_and_atomicity},
      {"version_is_atomic_across_generation", version_is_atomic_across_generation},
      {"drive_mode_lifetime_history_and_atomicity", drive_mode_lifetime_history_and_atomicity},
      {"catalog_and_version_are_immutable_across_restarts",
       catalog_and_version_are_immutable_across_restarts},
      {"trip_heading_expiry_and_atomic_progress", trip_heading_expiry_and_atomic_progress},
      {"trip_history_survives_missing_loss_and_source_generation",
       trip_history_survives_missing_loss_and_source_generation},
      {"trip_sticky_loss_and_retained_counter_coherence",
       trip_sticky_loss_and_retained_counter_coherence},
      {"hvo4_version_and_catalog_pinning", hvo4_version_and_catalog_pinning},
      {"trip_old_acquisition_cannot_return_through_generation",
       trip_old_acquisition_cannot_return_through_generation},
  };
  for (const auto& [name, run] : tests) {
    try {
      run();
    } catch (const std::exception& error) {
      throw std::runtime_error(std::string(name) + ": " + error.what());
    }
  }
  std::cout << "complete HVO1/HVO2/HVO3/HVO4 receiver expiry, lifetime trip history, immutable "
               "catalogs and atomic ordering passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
