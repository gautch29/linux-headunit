// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <string>

namespace headunit {
enum class VehicleSummaryState {
  admitted,
  awaiting_source,
  undated_source,
  invalid_snapshot,
  generation_mismatch,
  disconnected
};
enum class VehicleSummaryAvailability { available, missing, unavailable, stale, disconnected };
// This is the VEHICLE source's admitted generation, not a phone/display epoch.
// The existing source owner supplies retirement even for a still-fresh cached snapshot.
struct VehicleSummaryAdmission {
  std::uint64_t generation = 0;
  bool retired = false;
};
struct VehicleSummaryRow {
  std::string_view id, group, label, unit;
  std::optional<double> value;
  VehicleSummaryAvailability availability = VehicleSummaryAvailability::missing;
  std::optional<Clock::time_point> acquired_at;
  std::optional<Clock::duration> maximum_age;
  std::optional<VehicleObservationOrigin> origin;
};
struct PrephoneVehicleSummary {
  VehicleSummaryState state = VehicleSummaryState::awaiting_source;
  std::optional<std::uint64_t> source_generation;
  std::optional<Clock::time_point> source_started_at, source_sampled_at;
  std::array<VehicleSummaryRow, 8> rows;
};
// Pure fixed-size projection. Caller supplies a snapshot already admitted by its
// existing VehicleSource/cache/receiver. This is not a decoder, history cache or
// replacement for source replay/immutable-policy admission. No I/O or acquisition.
PrephoneVehicleSummary prephone_vehicle_summary(const VehicleSample&, VehicleSummaryAdmission,
                                                Clock::time_point now);
// Same projection as a bounded version-1 data interface for Home2 grouped lists.
// Dates/generation are decimal strings (or null), values retain canonical units.
// Encode off control/VM/GPU callbacks. No widgets, actions or publication owner.
inline constexpr std::size_t prephone_vehicle_summary_json_limit = 8192;
std::string prephone_vehicle_summary_json(const VehicleSample&, VehicleSummaryAdmission,
                                          Clock::time_point now);
}  // namespace headunit
