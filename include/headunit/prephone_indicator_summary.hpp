// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/prephone_vehicle_summary.hpp>

namespace headunit {
// Qualification is supplied by the existing source owner, never inferred from dates.
enum class PrephoneIndicatorClockDomain { unqualified, host_steady };
struct PrephoneIndicatorAdmission {
  VehicleSummaryAdmission source;
  PrephoneIndicatorClockDomain clock_domain = PrephoneIndicatorClockDomain::unqualified;
};
struct PrephoneIndicatorRow {
  Indicator indicator = Indicator::left_turn;
  std::string_view id, label;
  IndicatorColor color = IndicatorColor::green;
  std::optional<IndicatorState> value;
  VehicleSummaryAvailability availability = VehicleSummaryAvailability::missing;
  std::optional<Clock::time_point> acquired_at;
  std::optional<Clock::duration> maximum_age, age;
  std::optional<VehicleObservationOrigin> origin;
};
inline constexpr std::size_t prephone_indicator_summary_row_limit = 24;
inline constexpr std::size_t prephone_indicator_summary_json_version = 1;
inline constexpr std::size_t prephone_indicator_summary_json_limit = 12288;
struct PrephoneIndicatorSummary {
  VehicleSummaryState state = VehicleSummaryState::awaiting_source;
  PrephoneIndicatorClockDomain clock_domain = PrephoneIndicatorClockDomain::unqualified;
  std::optional<std::uint64_t> source_generation;
  std::optional<Clock::time_point> source_started_at, source_sampled_at, rendered_at;
  std::array<PrephoneIndicatorRow, prephone_indicator_summary_row_limit> rows;
};
// Pure fixed-size projection of an already admitted source snapshot. The source
// owns replay/order/immutable-policy validation, clock qualification and retirement.
// OFF/ON/FLASHING stay canonical; no animation, acquisition, history or I/O.
PrephoneIndicatorSummary prephone_indicator_summary(const VehicleSample&,
                                                    PrephoneIndicatorAdmission,
                                                    Clock::time_point now);
// Separate version-1 schema, no change to the legacy eight-row JSON or bridge.
// Encode off control/VM/GPU callbacks; only immutable catalogue text is emitted.
std::string prephone_indicator_summary_json(const VehicleSample&, PrephoneIndicatorAdmission,
                                            Clock::time_point now);
}  // namespace headunit
