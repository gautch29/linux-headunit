// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/iap2_message.hpp"
#include "headunit/iap2_parameter.hpp"
#include "headunit/vehicle.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace headunit {
struct RouteGuidanceConfig {
  std::uint16_t component_id = 42;
  std::string name = "Instrument cluster";
};
struct NavigationManeuver {
  std::uint16_t index = 0;
  std::optional<std::uint8_t> type;
  std::string instruction, road;
  std::optional<std::int16_t> exit_angle;
};
struct NavigationSnapshot {
  bool available = false;
  std::optional<std::uint8_t> route_state;
  std::string source, current_road, destination;
  std::optional<std::uint32_t> distance_m, remaining_m;
  std::optional<std::uint64_t> remaining_seconds, arrival_unix_seconds;
  std::optional<NavigationManeuver> maneuver;
  // Original accepted source-change date, never renewed by renderer polling.
  std::optional<Clock::time_point> updated_at;
};
Iap2Parameter route_guidance_component(const RouteGuidanceConfig&);
ControlMessage start_route_guidance(const RouteGuidanceConfig&);

// One phone/session only. Partial wire updates are committed atomically after
// complete validation; the render worker only takes a small immutable snapshot.
class RouteGuidance {
 public:
  explicit RouteGuidance(RouteGuidanceConfig config);
  bool receive(const ControlMessage&, Clock::time_point);  // false: malformed/wrong component
  NavigationSnapshot snapshot(Clock::time_point) const;
  void clear();

 private:
  RouteGuidanceConfig config_;
  mutable std::mutex mutex_;
  NavigationSnapshot state_;
  std::optional<bool> supported_;
  std::vector<std::uint16_t> order_;
  std::map<std::uint16_t, NavigationManeuver> maneuvers_;
};
std::string navigation_prompt(const NavigationSnapshot&);
}  // namespace headunit
