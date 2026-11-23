// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle.hpp>
#include <headunit/vehicle_observation.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace headunit {

SimulatedVehicle::SimulatedVehicle(Clock::time_point started_at) : started_at_(started_at) {}

VehicleSample SimulatedVehicle::sample(Clock::time_point now) {
  if (now < started_at_) {
    throw std::invalid_argument("simulation time moved backwards");
  }
  const double seconds = std::chrono::duration<double>(now - started_at_).count();
  const double speed = std::clamp(42.0 + 24.0 * std::sin(seconds * 0.12), 0.0, 130.0);
  const double rpm =
      std::clamp(900.0 + speed * 35.0 + 160.0 * std::sin(seconds * 0.55), 0.0, 6500.0);
  VehicleSample result{speed, rpm, now};
  result.simulated = true;
  result.fuel_percent = 62 + 8 * std::sin(seconds * 0.04);
  result.coolant_c = 88 + 3 * std::sin(seconds * 0.05);
  result.oil_c = 98 + 5 * std::sin(seconds * 0.05);
  result.oil_pressure_kpa = 280 + 40 * std::sin(seconds * 0.12);
  result.battery_voltage = 14.1 + 0.1 * std::sin(seconds);
  result.ambient_c = 18;
  result.range_km = 420;
  // The speed curve stays within 18..66 km/h, so its 0..130 clamp never engages.
  // Integrate that curve from start; elapsed time times current speed can fall.
  const double distance_km = (42.0 * seconds + 200.0 * (1.0 - std::cos(seconds * 0.12))) / 3600;
  result.odometer_km = 123456 + distance_km;
  // Bench automatic gearbox in Drive. The engaged gear follows simulated speed
  // only so gear-aware artwork has a coherent, explicitly simulated state.
  const auto gear = static_cast<std::uint8_t>(std::clamp(1.0 + speed / 18.0, 1.0, 8.0));
  result.transmission = TransmissionState{GearboxType::automatic, TransmissionSelector::drive,
                                          EngagedGear{EngagedGearKind::forward, gear}};
  // Bench drive-mode catalogue: comfort then sport, with sport selected.
  DriveModeCatalog modes;
  for (const auto key : {std::string_view{"comfort"}, std::string_view{"sport"}}) {
    modes.identifiers[modes.count] = *parse_drive_mode_identifier(key);
    ++modes.count;
  }
  result.drive_mode_catalog = modes;
  result.drive_mode = modes.identifiers[1];
  for (auto& value : result.indicators)
    value = IndicatorState::off;
  result.indicator(Indicator::low_beam) = IndicatorState::on;
  // Bench demo: cycle one indicator every three seconds, including flashing.
  const auto index = static_cast<std::size_t>(seconds / 3) % indicator_definitions.size();
  const auto id = indicator_definitions[index].id;
  result.indicator(id) =
      (id == Indicator::left_turn || id == Indicator::right_turn || id == Indicator::hazard)
          ? IndicatorState::flashing
          : IndicatorState::on;
  if (id == Indicator::hazard) {
    // The simulator supplies flashing summaries, not measured lamp phases.
    // Do not contradict hazard flashing with default explicit Off arrows.
    result.indicator(Indicator::left_turn) = IndicatorState::flashing;
    result.indicator(Indicator::right_turn) = IndicatorState::flashing;
  }
  return result;
}

const ReadingDefinition* find_vehicle_reading(std::string_view key) {
  for (const auto& item : reading_definitions)
    if (item.key == key) return &item;
  return nullptr;
}
const IndicatorDefinition* find_vehicle_indicator(std::string_view key) {
  for (const auto& item : indicator_definitions)
    if (item.key == key) return &item;
  return nullptr;
}
bool valid_vehicle_sample(const VehicleSample& sample) {
  for (const auto& item : reading_definitions) {
    const auto& value = sample.*item.member;
    if (value && (!std::isfinite(*value) || *value < item.minimum || *value > item.maximum))
      return false;
  }
  for (const auto& value : sample.indicators)
    if (value && *value != IndicatorState::off && *value != IndicatorState::on &&
        *value != IndicatorState::flashing)
      return false;
  if (sample.transmission && !valid_transmission_state(*sample.transmission)) return false;
  if (sample.drive_mode_catalog && !valid_drive_mode_catalog(*sample.drive_mode_catalog))
    return false;
  if (sample.drive_mode && (!sample.drive_mode_catalog ||
                            !contains_drive_mode(*sample.drive_mode_catalog, *sample.drive_mode)))
    return false;
  if (sample.trip && !valid_trip_state(*sample.trip)) return false;
  if (sample.heading && !valid_heading_state(*sample.heading)) return false;
  if (sample.acceleration && !valid_acceleration_state(*sample.acceleration)) return false;
  if (sample.gps_position && !valid_gps_position_state(*sample.gps_position)) return false;
  if (sample.observation_timing) {
    const auto& timing = *sample.observation_timing;
    if (!timing.generation || timing.started_at <= Clock::time_point{} ||
        sample.sampled_at < timing.started_at)
      return false;
    bool simulated = false;
    auto valid = [&](const VehicleSignalTiming& signal, bool has_value) {
      if (signal.maximum_age <= Clock::duration::zero() ||
          signal.maximum_age > maximum_vehicle_signal_age)
        return false;
      if (signal.origin != VehicleObservationOrigin::measured &&
          signal.origin != VehicleObservationOrigin::calculated &&
          signal.origin != VehicleObservationOrigin::simulation)
        return false;
      if (signal.availability != VehicleAvailability::available &&
          signal.availability != VehicleAvailability::unavailable &&
          signal.availability != VehicleAvailability::stale &&
          signal.availability != VehicleAvailability::disconnected)
        return false;
      if (signal.acquired_at) {
        if (*signal.acquired_at < timing.started_at || *signal.acquired_at > sample.sampled_at)
          return false;
        simulated |= signal.origin == VehicleObservationOrigin::simulation;
      }
      if (has_value != (signal.availability == VehicleAvailability::available)) return false;
      if ((signal.availability == VehicleAvailability::available ||
           signal.availability == VehicleAvailability::stale) &&
          !signal.acquired_at)
        return false;
      return true;
    };
    for (std::size_t i = 0; i < reading_definitions.size(); ++i)
      if (!valid(timing.readings[i], bool(sample.*reading_definitions[i].member))) return false;
    for (std::size_t i = 0; i < sample.indicators.size(); ++i)
      if (!valid(timing.indicators[i], bool(sample.indicators[i]))) return false;
    if (!valid(timing.transmission, bool(sample.transmission))) return false;
    if (!valid(timing.drive_mode, bool(sample.drive_mode))) return false;
    if (!valid(timing.trip, bool(sample.trip)) || !valid(timing.heading, bool(sample.heading)))
      return false;
    if (!valid(timing.acceleration, bool(sample.acceleration)) ||
        !valid(timing.gps_position, bool(sample.gps_position)))
      return false;
    if (sample.simulated != simulated) return false;
  }
  return true;
}

bool fresh(const VehicleSample& value, Clock::time_point now, Clock::duration maximum_age) {
  if (value.observation_timing) {
    if (!valid_vehicle_sample(value) || now < value.sampled_at) return false;
    auto current = [now](const VehicleSignalTiming& signal) {
      return signal.availability == VehicleAvailability::available && signal.acquired_at &&
             *signal.acquired_at <= now && now - *signal.acquired_at < signal.maximum_age;
    };
    const auto& timing = *value.observation_timing;
    return std::any_of(timing.readings.begin(), timing.readings.end(), current) ||
           std::any_of(timing.indicators.begin(), timing.indicators.end(), current) ||
           current(timing.transmission) || current(timing.drive_mode) || current(timing.trip) ||
           current(timing.heading) || current(timing.acceleration) || current(timing.gps_position);
  }
  return maximum_age > Clock::duration::zero() && value.sampled_at <= now &&
         now - value.sampled_at <= maximum_age;
}

VehicleSample current_vehicle_sample(const VehicleSample& value, Clock::time_point now,
                                     Clock::duration maximum_age) {
  auto result = value;
  auto clear = [&] {
    for (const auto& reading : reading_definitions)
      (result.*reading.member).reset();
    for (auto& indicator : result.indicators)
      indicator.reset();
    result.transmission.reset();
    result.drive_mode.reset();
    result.trip.reset();
    result.heading.reset();
    result.acceleration.reset();
    result.gps_position.reset();
  };
  if (!valid_vehicle_sample(value)) {
    clear();
    return result;
  }
  if (!result.observation_timing) {
    if (!fresh(value, now, maximum_age)) clear();
    return result;
  }
  auto filter = [&](VehicleSignalTiming& timing, auto& reading) {
    if (timing.availability != VehicleAvailability::available) return;
    if (now < value.sampled_at || now < *timing.acquired_at) {
      reading.reset();
      timing.availability = VehicleAvailability::unavailable;
    } else if (now - *timing.acquired_at >= timing.maximum_age) {
      reading.reset();
      timing.availability = VehicleAvailability::stale;
    }
  };
  auto& timing = *result.observation_timing;
  for (std::size_t i = 0; i < reading_definitions.size(); ++i)
    filter(timing.readings[i], result.*reading_definitions[i].member);
  for (std::size_t i = 0; i < result.indicators.size(); ++i)
    filter(timing.indicators[i], result.indicators[i]);
  filter(timing.transmission, result.transmission);
  filter(timing.drive_mode, result.drive_mode);
  filter(timing.trip, result.trip);
  filter(timing.heading, result.heading);
  filter(timing.acceleration, result.acceleration);
  filter(timing.gps_position, result.gps_position);
  return result;
}

}  // namespace headunit
