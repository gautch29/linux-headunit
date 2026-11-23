// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle_observation.hpp>

namespace headunit {
// Explicit synthetic acquisition source, separate from legacy undated simulation.
// One existing native owner supplies its actual host-steady acquisition clock.
// Generation is scoped to that source/publisher lifetime, never a boot identity.
class DatedSimulatedVehicle final : public VehicleSource {
 public:
  DatedSimulatedVehicle(Clock::time_point started_at, std::uint64_t generation,
                        VehicleAgePolicy ages = {});
  VehicleSample sample(Clock::time_point acquired_at) override;
  // Terminal for this instance. Later polls preserve dates and simulation origin.
  void retire(Clock::time_point now);

 private:
  SimulatedVehicle generator_;
  VehicleObservationCache cache_;
  const std::uint64_t generation_;
  std::uint64_t sequence_ = 0;
  std::optional<Clock::time_point> acquired_at_;
  bool retired_ = false;
};
}  // namespace headunit
