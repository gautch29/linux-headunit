// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace headunit {

struct TripIdentifier {
  std::array<char, 32> bytes{};
  std::uint8_t length = 0;
  bool operator==(const TripIdentifier&) const = default;
};
enum class TripAverageSpeedBasis { total, moving };
struct TripState {
  TripIdentifier identifier{};
  std::uint64_t reset_generation = 0;
  std::optional<double> distance_km{}, elapsed_seconds{}, moving_seconds{}, average_speed_kmh{};
  std::optional<TripAverageSpeedBasis> average_speed_basis{};
  // Mean over this current trip, not an instantaneous fuel-flow reading.
  std::optional<double> fuel_efficiency_l_per_100km{};
  bool operator==(const TripState&) const = default;
};
// Protocol plausibility bounds, independent of vehicle calibration or warnings.
inline constexpr double maximum_trip_distance_km = 9999999;
inline constexpr double maximum_trip_duration_seconds = 315576000;
inline constexpr double maximum_trip_average_speed_kmh = 300;
inline constexpr double maximum_trip_fuel_efficiency_l_per_100km = 200;
// Supports an average rounded to the nearest whole km/h. Producers must omit
// it when their rounded distance/time cannot establish this coherence; the
// host does not synthesize an average or silently repair counters.
inline constexpr double trip_average_speed_rounding_tolerance_kmh = 0.5;

bool valid_trip_identifier(const TripIdentifier&) noexcept;
std::optional<TripIdentifier> parse_trip_identifier(std::string_view) noexcept;
std::string_view trip_identifier_key(const TripIdentifier&) noexcept;
bool valid_trip_state(const TripState&) noexcept;
// Canonical slash order: identifier/reset_generation/distance_km/
// elapsed_seconds/moving_seconds/average_speed_kmh/average_speed_basis/
// fuel_efficiency_l_per_100km. Optional components use '-', basis total/moving.
std::string trip_state_key(const TripState&);
std::optional<TripState> parse_trip_state_key(std::string_view) noexcept;

// Consumer/source lifetime history, retained through missing observations,
// withdrawal and source generations. Counter watermarks are not render values.
struct TripProgress {
  std::optional<TripIdentifier> identifier{};
  std::uint64_t reset_generation = 0;
  std::optional<double> distance_km{}, elapsed_seconds{}, moving_seconds{};
  // Retained counters can come from different observations. A newer/equal
  // elapsed counter must cover older moving time; an older elapsed watermark
  // is not substituted for an unavailable current elapsed value.
  std::optional<std::chrono::steady_clock::time_point> distance_acquired_at{},
      elapsed_acquired_at{}, moving_acquired_at{};
  std::optional<std::chrono::steady_clock::time_point> acquired_at{};
  std::optional<TripState> last_observation{};
  bool requires_new_acquisition = false;
  bool operator==(const TripProgress&) const = default;
};
bool valid_trip_progress(const TripProgress&) noexcept;
// Marks a known history on source restart, unavailable/withdrawal, or expiry.
// Pristine unknown history stays pristine. No mutation if history is malformed.
bool withdraw_trip_progress(TripProgress&) noexcept;
// Checks a candidate before replacing progress. No allocation or mutation on
// failure. Higher resets require a strictly newer actual acquisition, even
// after source restart; same-reset known counters cannot move backwards. A
// withdrawn history cannot revive at an identical old acquisition date.
bool update_trip_progress(TripProgress&, const TripState&,
                          std::chrono::steady_clock::time_point) noexcept;

}  // namespace headunit
