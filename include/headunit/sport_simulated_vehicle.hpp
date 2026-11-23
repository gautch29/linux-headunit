// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>

namespace headunit {
// Separate sport fixture. Does not change the ordinary gauge simulation.
VehicleSample sport_simulation_sample(Clock::time_point started, Clock::time_point now);
}  // namespace headunit
