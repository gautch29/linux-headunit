// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/sport_simulated_vehicle.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace headunit {
VehicleSample sport_simulation_sample(Clock::time_point started, Clock::time_point now) {
  if (started <= Clock::time_point{} || now < started)
    throw std::invalid_argument("sport simulation time reversed or nonpositive");
  const double elapsed = std::chrono::duration<double>(now - started).count();
  const double phase = std::fmod(elapsed, 40);
  const double angle = elapsed * 2 * std::numbers::pi / 20;
  VehicleSample sample;
  sample.speed_kmh = phase < 2 ? 0 : phase < 18 ? std::min(220., (phase - 2) * 20) : 0;
  sample.acceleration =
      AccelerationState{phase > 2 && phase < 13 ? 20 / 3.6 : 0, 3 * std::sin(angle)};
  sample.gps_position =
      GpsPositionState{48 + .001 * std::sin(angle), 2 + .001 * std::cos(angle), 1};
  sample.sampled_at = now;
  sample.simulated = true;
  sample.observation_timing.emplace();
  auto& timing = *sample.observation_timing;
  timing.generation = 1;
  timing.started_at = started;
  for (auto* signal : {&timing.readings[0], &timing.acceleration, &timing.gps_position}) {
    signal->acquired_at = now;
    signal->origin = VehicleObservationOrigin::simulation;
    signal->availability = VehicleAvailability::available;
  }
  return sample;
}
}  // namespace headunit
