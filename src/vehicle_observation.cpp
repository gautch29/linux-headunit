// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_observation.hpp>
#include <algorithm>
#include <bitset>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace headunit {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
bool valid_origin(VehicleObservationOrigin origin) {
  return origin == VehicleObservationOrigin::measured ||
         origin == VehicleObservationOrigin::calculated ||
         origin == VehicleObservationOrigin::simulation;
}
void simulation_flag(VehicleSample& sample) {
  const auto& timing = *sample.observation_timing;
  auto simulated = [](const VehicleSignalTiming& signal) {
    return signal.acquired_at && signal.origin == VehicleObservationOrigin::simulation;
  };
  sample.simulated = std::any_of(timing.readings.begin(), timing.readings.end(), simulated) ||
                     std::any_of(timing.indicators.begin(), timing.indicators.end(), simulated) ||
                     simulated(timing.transmission) || simulated(timing.drive_mode) ||
                     simulated(timing.trip) || simulated(timing.heading) ||
                     simulated(timing.acceleration) || simulated(timing.gps_position);
}
}  // namespace
VehicleSignalTiming& vehicle_signal_timing(VehicleObservationTiming& timing, std::size_t index) {
  if (index < reading_definitions.size()) return timing.readings.at(index);
  if (index < legacy_vehicle_observation_capacity)
    return timing.indicators.at(index - reading_definitions.size());
  if (index == transmission_observation_index) return timing.transmission;
  if (index == drive_mode_observation_index) return timing.drive_mode;
  if (index == trip_observation_index) return timing.trip;
  if (index == heading_observation_index) return timing.heading;
  if (index == acceleration_observation_index) return timing.acceleration;
  if (index == gps_position_observation_index) return timing.gps_position;
  throw std::out_of_range("vehicle observation index exceeds capacity");
}
const VehicleSignalTiming& vehicle_signal_timing(const VehicleObservationTiming& timing,
                                                 std::size_t index) {
  if (index < reading_definitions.size()) return timing.readings.at(index);
  if (index < legacy_vehicle_observation_capacity)
    return timing.indicators.at(index - reading_definitions.size());
  if (index == transmission_observation_index) return timing.transmission;
  if (index == drive_mode_observation_index) return timing.drive_mode;
  if (index == trip_observation_index) return timing.trip;
  if (index == heading_observation_index) return timing.heading;
  if (index == acceleration_observation_index) return timing.acceleration;
  if (index == gps_position_observation_index) return timing.gps_position;
  throw std::out_of_range("vehicle observation index exceeds capacity");
}
VehicleObservationValue vehicle_observation_value(const VehicleSample& sample, std::size_t index) {
  if (index < reading_definitions.size()) {
    if (const auto& value = sample.*reading_definitions[index].member; value) return *value;
  } else if (index < legacy_vehicle_observation_capacity) {
    if (const auto& value = sample.indicators.at(index - reading_definitions.size()); value)
      return *value;
  } else if (index == transmission_observation_index) {
    if (sample.transmission) return *sample.transmission;
  } else if (index == drive_mode_observation_index) {
    if (sample.drive_mode) return *sample.drive_mode;
  } else if (index == trip_observation_index) {
    if (sample.trip) return *sample.trip;
  } else if (index == heading_observation_index) {
    if (sample.heading) return *sample.heading;
  } else if (index == acceleration_observation_index) {
    if (sample.acceleration) return *sample.acceleration;
  } else if (index == gps_position_observation_index) {
    if (sample.gps_position) return *sample.gps_position;
  } else
    throw std::out_of_range("vehicle observation index exceeds capacity");
  return std::monostate{};
}
VehicleAgePolicy::VehicleAgePolicy() {
  readings.fill(std::chrono::milliseconds(500));
  indicators.fill(std::chrono::milliseconds(500));
}
VehicleObservationCache::VehicleObservationCache(VehicleAgePolicy ages,
                                                 std::optional<DriveModeCatalog> modes)
    : ages_(std::move(ages)),
      drive_mode_catalog_(std::move(modes)),
      owner_(std::this_thread::get_id()) {
  static_assert(reading_definitions.size() + indicator_definitions.size() ==
                legacy_vehicle_observation_capacity);
  static_assert(transmission_observation_index + 1 == transmission_vehicle_observation_capacity);
  static_assert(drive_mode_observation_index + 1 == drive_mode_vehicle_observation_capacity);
  static_assert(trip_observation_index == drive_mode_vehicle_observation_capacity);
  static_assert(heading_observation_index + 1 == trip_heading_vehicle_observation_capacity);
  static_assert(std::is_nothrow_copy_assignable_v<VehicleSample>);
  static_assert(std::is_nothrow_move_assignable_v<VehicleSample>);
  static_assert(std::is_nothrow_move_assignable_v<decltype(watermarks_)>);
  check(!drive_mode_catalog_ || valid_drive_mode_catalog(*drive_mode_catalog_),
        "invalid configured drive mode catalog");
  for (const auto age : ages_.readings)
    check(age > Clock::duration::zero() && age <= maximum_vehicle_signal_age,
          "invalid vehicle reading age policy");
  for (const auto age : ages_.indicators)
    check(age > Clock::duration::zero() && age <= maximum_vehicle_signal_age,
          "invalid vehicle indicator age policy");
  check(ages_.transmission > Clock::duration::zero() &&
            ages_.transmission <= maximum_vehicle_signal_age,
        "invalid transmission age policy");
  check(
      ages_.drive_mode > Clock::duration::zero() && ages_.drive_mode <= maximum_vehicle_signal_age,
      "invalid drive mode age policy");
  check(ages_.trip > Clock::duration::zero() && ages_.trip <= maximum_vehicle_signal_age,
        "invalid trip age policy");
  check(ages_.heading > Clock::duration::zero() && ages_.heading <= maximum_vehicle_signal_age,
        "invalid heading age policy");
  for (const auto age : {ages_.acceleration, ages_.gps_position})
    check(age > Clock::duration::zero() && age <= maximum_vehicle_signal_age,
          "invalid sport age policy");
}
void VehicleObservationCache::check_owner_and_time(Clock::time_point now) const {
  check(owner_ == std::this_thread::get_id(),
        "vehicle observation cache used by another owner thread");
  check(now > Clock::time_point{} && (!last_host_time_ || now >= *last_host_time_),
        "vehicle host observation time reversed or is not positive");
}
void VehicleObservationCache::begin_generation(std::uint64_t generation, Clock::time_point started,
                                               Clock::time_point now) {
  check_owner_and_time(now);
  check(generation &&
            (!sample_.observation_timing || generation > sample_.observation_timing->generation),
        "vehicle source generation did not advance");
  check(started > Clock::time_point{} && started <= now &&
            (!last_host_time_ || started >= *last_host_time_),
        "vehicle generation start is future, nonpositive or older than the host watermark");
  VehicleSample replacement;
  replacement.sampled_at = now;
  replacement.observation_timing.emplace();
  replacement.drive_mode_catalog = drive_mode_catalog_;
  auto& timing = *replacement.observation_timing;
  timing.generation = generation;
  timing.started_at = started;
  for (std::size_t i = 0; i < timing.readings.size(); ++i)
    timing.readings[i].maximum_age = ages_.readings[i];
  for (std::size_t i = 0; i < timing.indicators.size(); ++i)
    timing.indicators[i].maximum_age = ages_.indicators[i];
  timing.transmission.maximum_age = ages_.transmission;
  timing.drive_mode.maximum_age = ages_.drive_mode;
  timing.trip.maximum_age = ages_.trip;
  timing.heading.maximum_age = ages_.heading;
  timing.acceleration.maximum_age = ages_.acceleration;
  timing.gps_position.maximum_age = ages_.gps_position;
  auto trip_progress = trip_progress_;
  check(withdraw_trip_progress(trip_progress),
        "invalid retained trip history during source restart");
  sample_ = std::move(replacement);
  watermarks_ = {};
  trip_progress_ = trip_progress;
  sequence_ = 0;
  last_host_time_ = now;
}
void VehicleObservationCache::expire(Clock::time_point now) {
  auto current = current_vehicle_sample(sample_, now);
  auto trip_progress = trip_progress_;
  if (!current.trip)
    check(withdraw_trip_progress(trip_progress), "invalid retained trip history during expiry");
  sample_ = std::move(current);
  sample_.sampled_at = now;
  trip_progress_ = trip_progress;
  last_host_time_ = now;
}
void VehicleObservationCache::accept(std::uint64_t generation, std::uint64_t sequence,
                                     std::span<const VehicleObservation> observations,
                                     Clock::time_point now) {
  check_owner_and_time(now);
  check(sample_.observation_timing && generation == sample_.observation_timing->generation,
        "vehicle observation source generation mismatch");
  check(sequence && sequence > sequence_, "vehicle observation sequence replay");
  check(observations.size() <= vehicle_observation_capacity,
        "vehicle observation batch exceeds 40 signals");
  auto candidate = sample_;
  auto watermarks = watermarks_;
  std::bitset<vehicle_observation_capacity> seen;
  auto trip_progress = trip_progress_;
  candidate.sampled_at = now;
  for (const auto& observation : observations) {
    check(observation.key.size() <= 64, "vehicle observation key exceeds bound");
    const auto* reading = find_vehicle_reading(observation.key);
    const auto* indicator = find_vehicle_indicator(observation.key);
    const bool transmission = observation.key == "transmission";
    const bool drive_mode = observation.key == "drive_mode";
    const bool trip = observation.key == "trip", heading = observation.key == "heading";
    const bool acceleration = observation.key == "acceleration";
    const bool gps_position = observation.key == "gps_position";
    check(reading || indicator || transmission || drive_mode || trip || heading || acceleration ||
              gps_position,
          "unknown semantic vehicle observation");
    const auto index = reading        ? std::size_t(reading - reading_definitions.data())
                       : transmission ? transmission_observation_index
                       : drive_mode   ? drive_mode_observation_index
                       : trip         ? trip_observation_index
                       : heading      ? heading_observation_index
                       : acceleration ? acceleration_observation_index
                       : gps_position ? gps_position_observation_index
                                      : reading_definitions.size() + std::size_t(indicator->id);
    check(!seen.test(index), "duplicate signal in vehicle observation batch");
    seen.set(index);
    check(valid_origin(observation.origin), "invalid vehicle observation origin");
    check(observation.availability == VehicleAvailability::available ||
              observation.availability == VehicleAvailability::unavailable,
          "producer availability must be available or unavailable");
    const bool available = observation.availability == VehicleAvailability::available;
    if (!available)
      check(std::holds_alternative<std::monostate>(observation.value),
            "unavailable observation carries a value");
    else if (reading) {
      const auto* value = std::get_if<double>(&observation.value);
      check(value && std::isfinite(*value) && *value >= reading->minimum &&
                *value <= reading->maximum,
            "invalid numeric vehicle observation");
    } else if (transmission) {
      const auto* value = std::get_if<TransmissionState>(&observation.value);
      check(value && valid_transmission_state(*value), "invalid coherent transmission observation");
    } else if (drive_mode) {
      const auto* value = std::get_if<DriveModeIdentifier>(&observation.value);
      check(value && drive_mode_catalog_ && contains_drive_mode(*drive_mode_catalog_, *value),
            "unsupported or invalid effective drive mode observation");
    } else if (trip) {
      const auto* value = std::get_if<TripState>(&observation.value);
      check(value && valid_trip_state(*value), "invalid current trip observation");
    } else if (heading) {
      const auto* value = std::get_if<HeadingState>(&observation.value);
      check(value && valid_heading_state(*value), "invalid qualified heading observation");
    } else if (acceleration) {
      const auto* value = std::get_if<AccelerationState>(&observation.value);
      check(value && valid_acceleration_state(*value), "invalid acceleration observation");
    } else if (gps_position) {
      const auto* value = std::get_if<GpsPositionState>(&observation.value);
      check(value && valid_gps_position_state(*value), "invalid GPS position observation");
    } else {
      const auto* value = std::get_if<IndicatorState>(&observation.value);
      check(value && (*value == IndicatorState::off || *value == IndicatorState::on ||
                      *value == IndicatorState::flashing),
            "invalid indicator vehicle observation");
    }
    check(observation.acquired_at >= candidate.observation_timing->started_at &&
              observation.acquired_at <= now,
          "vehicle acquisition time is future or precedes the source generation");
    auto& watermark = watermarks[index];
    if (watermark.acquired_at) {
      check(observation.acquired_at >= *watermark.acquired_at, "vehicle acquisition time reversed");
      if (observation.acquired_at == *watermark.acquired_at) {
        check(observation.value == watermark.value && observation.origin == watermark.origin &&
                  observation.availability == watermark.availability,
              "conflicting equal-time vehicle observation");
        continue;  // Idempotent, preserving withdrawal/expiry and its original acquisition date.
      }
    }
    watermark = {observation.value, observation.acquired_at, observation.origin,
                 observation.availability};
    if (trip && available)
      check(update_trip_progress(trip_progress, std::get<TripState>(observation.value),
                                 observation.acquired_at),
            "trip identity, reset or acquired counter history conflict");
    if (trip && !available)
      check(withdraw_trip_progress(trip_progress), "invalid retained unavailable trip history");
    auto& timing = vehicle_signal_timing(*candidate.observation_timing, index);
    timing.acquired_at = observation.acquired_at;
    timing.origin = observation.origin;
    timing.availability = observation.availability;
    if (reading) {
      auto& value = candidate.*reading->member;
      if (available)
        value = std::get<double>(observation.value);
      else
        value.reset();
    } else if (transmission) {
      if (available)
        candidate.transmission = std::get<TransmissionState>(observation.value);
      else
        candidate.transmission.reset();
    } else if (drive_mode) {
      if (available)
        candidate.drive_mode = std::get<DriveModeIdentifier>(observation.value);
      else
        candidate.drive_mode.reset();
    } else if (trip) {
      if (available)
        candidate.trip = std::get<TripState>(observation.value);
      else
        candidate.trip.reset();
    } else if (heading) {
      if (available)
        candidate.heading = std::get<HeadingState>(observation.value);
      else
        candidate.heading.reset();
    } else if (acceleration) {
      if (available)
        candidate.acceleration = std::get<AccelerationState>(observation.value);
      else
        candidate.acceleration.reset();
    } else if (gps_position) {
      if (available)
        candidate.gps_position = std::get<GpsPositionState>(observation.value);
      else
        candidate.gps_position.reset();
    } else {
      auto& value = candidate.indicator(indicator->id);
      if (available)
        value = std::get<IndicatorState>(observation.value);
      else
        value.reset();
    }
  }
  simulation_flag(candidate);
  check(valid_vehicle_sample(candidate), "invalid candidate vehicle observation snapshot");
  candidate = current_vehicle_sample(candidate, now);
  if (!candidate.trip)
    check(withdraw_trip_progress(trip_progress), "invalid retained trip history after filtering");
  sample_ = std::move(candidate);
  watermarks_ = std::move(watermarks);
  trip_progress_ = trip_progress;
  sequence_ = sequence;
  last_host_time_ = now;
}
void VehicleObservationCache::withdraw(Clock::time_point now) {
  check_owner_and_time(now);
  check(bool(sample_.observation_timing), "vehicle source generation has not begun");
  auto trip_progress = trip_progress_;
  check(withdraw_trip_progress(trip_progress), "invalid retained trip history during withdrawal");
  for (const auto& reading : reading_definitions)
    (sample_.*reading.member).reset();
  for (auto& indicator : sample_.indicators)
    indicator.reset();
  sample_.transmission.reset();
  sample_.drive_mode.reset();
  sample_.trip.reset();
  sample_.heading.reset();
  sample_.acceleration.reset();
  sample_.gps_position.reset();
  for (auto& timing : sample_.observation_timing->readings)
    timing.availability = VehicleAvailability::disconnected;
  for (auto& timing : sample_.observation_timing->indicators)
    timing.availability = VehicleAvailability::disconnected;
  // Preserve the untouched extension channel for legacy HVO1 producers. Once
  // observed, withdrawal retains its acquisition watermark like other signals.
  if (sample_.observation_timing->transmission.acquired_at)
    sample_.observation_timing->transmission.availability = VehicleAvailability::disconnected;
  if (sample_.observation_timing->drive_mode.acquired_at)
    sample_.observation_timing->drive_mode.availability = VehicleAvailability::disconnected;
  if (sample_.observation_timing->trip.acquired_at)
    sample_.observation_timing->trip.availability = VehicleAvailability::disconnected;
  if (sample_.observation_timing->heading.acquired_at)
    sample_.observation_timing->heading.availability = VehicleAvailability::disconnected;
  for (auto* timing :
       {&sample_.observation_timing->acceleration, &sample_.observation_timing->gps_position})
    if (timing->acquired_at) timing->availability = VehicleAvailability::disconnected;
  sample_.sampled_at = now;
  trip_progress_ = trip_progress;
  last_host_time_ = now;
}
VehicleSample VehicleObservationCache::sample(Clock::time_point now) {
  check_owner_and_time(now);
  check(bool(sample_.observation_timing), "vehicle source generation has not begun");
  expire(now);
  return sample_;
}
}  // namespace headunit
