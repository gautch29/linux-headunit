// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_publication.hpp>
#include <headunit/dated_simulated_vehicle.hpp>
#include <headunit/vehicle_observation_receiver.hpp>
#include <headunit/vehicle_observation_wire.hpp>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool value) {
  if (!value) throw std::runtime_error("dated simulation contract failed");
}
void legacy() {
  using namespace headunit;
  const auto started = Clock::time_point{std::chrono::seconds(100)};
  SimulatedVehicle source(started);
  const auto sample = source.sample(started + std::chrono::seconds(7));
  require(sample.simulated && !sample.observation_timing);
  const auto model = prephone_vehicle_summary(sample, {1, false}, sample.sampled_at);
  require(model.state == VehicleSummaryState::undated_source && !model.source_generation &&
          !model.source_started_at && !model.source_sampled_at);
  for (const auto& row : model.rows)
    require(!row.value && !row.acquired_at && !row.origin && !row.maximum_age);
}
void dated() {
  using namespace headunit;
  using namespace std::chrono_literals;
  const auto start = Clock::time_point{100s};
  VehicleAgePolicy ages;
  DatedSimulatedVehicle source(start, 43, ages);
  ages.readings.fill(1ns);  // The real cache already owns its immutable copy.
  SimulatedVehicle original(start);
  VehicleSample held;
  for (const auto elapsed : {0ms, 1ms, 7000ms, 90000ms}) {
    const auto now = start + elapsed;
    const auto sample = source.sample(now);
    const auto legacy_sample = original.sample(now);
    require(valid_vehicle_sample(sample) && sample.simulated);
    require(sample.sampled_at == legacy_sample.sampled_at);
    require(sample.transmission == legacy_sample.transmission &&
            sample.drive_mode == legacy_sample.drive_mode &&
            sample.drive_mode_catalog == legacy_sample.drive_mode_catalog &&
            sample.indicators == legacy_sample.indicators);
    require(!sample.trip && !sample.heading && !sample.acceleration && !sample.gps_position);
    const auto& timing = *sample.observation_timing;
    require(timing.generation == 43 && timing.started_at == start);
    for (std::size_t i = 0; i < drive_mode_vehicle_observation_capacity; ++i) {
      const auto& signal = vehicle_signal_timing(timing, i);
      require(signal.acquired_at == now && signal.maximum_age == 500ms &&
              signal.origin == VehicleObservationOrigin::simulation &&
              signal.availability == VehicleAvailability::available);
      require(vehicle_observation_value(sample, i) == vehicle_observation_value(legacy_sample, i));
    }
    for (std::size_t i = trip_observation_index; i < vehicle_observation_capacity; ++i)
      require(!vehicle_signal_timing(timing, i).acquired_at);
    const auto repeated = source.sample(now);
    require(repeated.sampled_at == sample.sampled_at &&
            repeated.observation_timing->readings[2].acquired_at == now);
    const auto model = prephone_vehicle_summary(sample, {43}, now);
    require(model.state == VehicleSummaryState::admitted && model.source_generation == 43);
    for (const auto& row : model.rows)
      require(row.value && row.acquired_at == now && row.maximum_age == 500ms &&
              row.origin == VehicleObservationOrigin::simulation);
    held = sample;
  }
  const auto acquired = held.sampled_at;
  // Independently known initial curve, rather than only comparing two samplers.
  DatedSimulatedVehicle first(start, 1);
  const auto initial = first.sample(start);
  require(initial.speed_kmh == 42 && initial.engine_rpm == 2370 && initial.fuel_percent == 62 &&
          initial.ambient_c == 18 && initial.range_km == 420 && initial.odometer_km == 123456);
  require(prephone_vehicle_summary(held, {43}, acquired + 499ms).rows[0].value.has_value());
  const auto expired = prephone_vehicle_summary(held, {43}, acquired + 500ms);
  for (const auto& row : expired.rows)
    require(!row.value && row.availability == VehicleSummaryAvailability::stale &&
            row.acquired_at == acquired && row.origin == VehicleObservationOrigin::simulation);
  bool rejected = false;
  try {
    source.sample(acquired - 1ns);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected);
  require(source.sample(acquired).observation_timing->readings[2].acquired_at == acquired);
  rejected = false;
  std::thread other([&] {
    try {
      source.sample(acquired + 1ns);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
  });
  other.join();
  require(rejected);
  source.retire(acquired + 1ns);
  for (const auto now : {acquired + 1ns, acquired + 10s}) {
    const auto retired = source.sample(now);
    require(retired.simulated && !retired.speed_kmh && !retired.transmission &&
            !retired.drive_mode);
    for (const auto& row : prephone_vehicle_summary(retired, {43}, now).rows)
      require(!row.value && row.availability == VehicleSummaryAvailability::disconnected &&
              row.acquired_at == acquired && row.origin == VehicleObservationOrigin::simulation);
  }
  // Actual codec/receiver has independent ordering and terminal nonrevival rules.
  VehicleObservationReceiver receiver;
  receiver.accept({held, 1, VehicleObservationWireVersion::hvo4}, acquired);
  require(!receiver.sample(acquired + 500ms).fuel_percent);
  receiver.accept({held, 2, VehicleObservationWireVersion::hvo4}, acquired + 500ms);
  require(!receiver.sample(acquired + 500ms).fuel_percent);
  DatedSimulatedVehicle successor(acquired + 501ms, 44);
  const auto fresh_sample = successor.sample(acquired + 501ms);
  const auto bytes =
      encode_vehicle_observation_datagram({fresh_sample, 1, VehicleObservationWireVersion::hvo4});
  const auto decoded = parse_vehicle_observation_datagram(bytes, acquired + 501ms);
  require(decoded.has_value());
  receiver.accept(*decoded, acquired + 501ms);
  require(receiver.sample(acquired + 501ms).fuel_percent == 62);
  try {
    receiver.accept({held, 3, VehicleObservationWireVersion::hvo4}, acquired + 502ms);
  } catch (const std::invalid_argument&) {
  }
  require(receiver.sample(acquired + 502ms).observation_timing->generation == 44);
  for (const auto generation : {std::uint64_t{0}}) {
    rejected = false;
    try {
      DatedSimulatedVehicle invalid(start, generation);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected);
  }
}
}  // namespace
int main(int argc, char** argv) try {
  legacy();
  dated();
  if (argc == 2 && std::string_view(argv[1]) == "--legacy-packet") {
    using namespace headunit;
    const auto now = Clock::now();
    SimulatedVehicle source(now);
    const auto model = prephone_vehicle_summary(source.sample(now), {1, false}, now);
    const auto packet = prephone_packet(model, {}, 1, now);
    std::cout.write(reinterpret_cast<const char*>(packet.data()), packet.size());
  } else {
    require(argc == 1);
    std::cout << "Legacy undated and new dated synthetic source contracts passed\n";
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
