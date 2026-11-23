// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/dated_simulated_vehicle.hpp>
#include <limits>
#include <stdexcept>

namespace headunit {
DatedSimulatedVehicle::DatedSimulatedVehicle(Clock::time_point started, std::uint64_t generation,
                                             VehicleAgePolicy ages)
    : generator_(started),
      cache_(std::move(ages), generator_.sample(started).drive_mode_catalog),
      generation_(generation) {
  cache_.begin_generation(generation_, started, started);
}
VehicleSample DatedSimulatedVehicle::sample(Clock::time_point now) {
  // Validate owner and clock before evaluating or changing generator state.
  auto current = cache_.sample(now);
  if (retired_ || (acquired_at_ && now == *acquired_at_)) return current;
  if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    retire(now);
    throw std::overflow_error("dated simulation sequence exhausted");
  }
  // Genuinely evaluate a NEW synthetic sample at this acquisition. Never date a
  // previously cached legacy record or infer physical fields from this curve.
  const auto generated = generator_.sample(now);
  std::array<VehicleObservation, drive_mode_vehicle_observation_capacity> observations;
  std::size_t count = 0;
  for (const auto& reading : reading_definitions) {
    if (const auto value = generated.*reading.member)
      observations[count++] = {reading.key, *value, generated.sampled_at,
                               VehicleObservationOrigin::simulation};
  }
  for (const auto& indicator : indicator_definitions) {
    if (const auto value = generated.indicator(indicator.id))
      observations[count++] = {indicator.key, *value, generated.sampled_at,
                               VehicleObservationOrigin::simulation};
  }
  if (generated.transmission)
    observations[count++] = {"transmission", *generated.transmission, generated.sampled_at,
                             VehicleObservationOrigin::simulation};
  if (generated.drive_mode)
    observations[count++] = {"drive_mode", *generated.drive_mode, generated.sampled_at,
                             VehicleObservationOrigin::simulation};
  cache_.accept(generation_, sequence_ + 1, std::span(observations).first(count), now);
  ++sequence_;
  acquired_at_ = generated.sampled_at;
  return cache_.sample(now);
}
void DatedSimulatedVehicle::retire(Clock::time_point now) {
  cache_.withdraw(now);
  retired_ = true;
}
}  // namespace headunit
