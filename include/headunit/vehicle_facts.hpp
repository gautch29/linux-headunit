// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <chrono>
#include <mutex>
#include <optional>

namespace headunit {

// Dated vehicle facts the receiver's phone policies may use (night mode from
// the low beam, driving restrictions from speed). Published by the single
// owner of the vehicle source after its own freshness filtering; a missing
// value is unknown, never "off" or zero.
struct VehicleFacts {
  std::optional<bool> low_beam;
  std::optional<double> speed_kmh;
  bool simulated = false;
  std::chrono::steady_clock::time_point observed_at{};
};

namespace detail {
inline std::mutex& vehicle_facts_mutex() {
  static std::mutex mutex;
  return mutex;
}
inline std::optional<VehicleFacts>& vehicle_facts_storage() {
  static std::optional<VehicleFacts> facts;
  return facts;
}
}  // namespace detail

inline void publish_vehicle_facts(const VehicleFacts& facts) {
  std::lock_guard lock(detail::vehicle_facts_mutex());
  detail::vehicle_facts_storage() = facts;
}
inline void clear_vehicle_facts() {
  std::lock_guard lock(detail::vehicle_facts_mutex());
  detail::vehicle_facts_storage().reset();
}
// Facts older than two seconds (or from the future) are not returned.
inline std::optional<VehicleFacts> fresh_vehicle_facts(
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
  std::lock_guard lock(detail::vehicle_facts_mutex());
  const auto& facts = detail::vehicle_facts_storage();
  if (!facts || facts->observed_at > now || now - facts->observed_at > std::chrono::seconds(2))
    return std::nullopt;
  return facts;
}

}  // namespace headunit
