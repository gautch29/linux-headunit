// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace headunit {
// Calibrated, gravity-compensated acceleration in the installed vehicle frame.
// Positive longitudinal is forward; positive lateral is left. Raw IMU specific
// force is not this observation. An unqualified orientation/filter is absence.
struct AccelerationState {
  double longitudinal_mps2 = 0;
  double lateral_mps2 = 0;
  bool operator==(const AccelerationState&) const = default;
};
// Qualified WGS84 fix at acquisition; accuracy is a supplied horizontal bound.
struct GpsPositionState {
  double latitude_deg = 0;
  double longitude_deg = 0;
  double horizontal_accuracy_m = 0;
  bool operator==(const GpsPositionState&) const = default;
};
bool valid_acceleration_state(const AccelerationState&);
bool valid_gps_position_state(const GpsPositionState&);
std::string acceleration_state_key(const AccelerationState&);
std::string gps_position_state_key(const GpsPositionState&);
std::optional<AccelerationState> parse_acceleration_state_key(std::string_view);
std::optional<GpsPositionState> parse_gps_position_state_key(std::string_view);
}  // namespace headunit
