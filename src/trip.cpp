// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/trip.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>

namespace headunit {
namespace {
bool letter(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}
bool character(char value) {
  return letter(value) || (value >= '0' && value <= '9') || value == '_' || value == '-';
}
bool bounded(const std::optional<double>& value, double maximum) {
  return !value || (std::isfinite(*value) && *value >= 0 && *value <= maximum);
}
std::string_view number_key(double value, std::array<char, 64>& buffer) {
  if (value == 0) value = 0;  // One canonical representation for either floating zero.
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                    std::chars_format::general);
  if (result.ec != std::errc{}) return {};
  return {buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())};
}
bool parse_number(std::string_view key, std::optional<double>& output) {
  if (key == "-") {
    output.reset();
    return true;
  }
  if (key.empty() || key.size() > 64) return false;
  double value = 0;
  const auto result =
      std::from_chars(key.data(), key.data() + key.size(), value, std::chars_format::general);
  if (result.ec != std::errc{} || result.ptr != key.data() + key.size() || !std::isfinite(value))
    return false;
  std::array<char, 64> canonical{};
  if (number_key(value, canonical) != key) return false;
  output = value;
  return true;
}
void append_number(std::string& output, const std::optional<double>& value) {
  if (!value) {
    output += '-';
    return;
  }
  std::array<char, 64> buffer{};
  output += number_key(*value, buffer);
}
}  // namespace
static_assert(std::is_trivially_copyable_v<TripState>);
static_assert(std::is_trivially_copyable_v<TripProgress>);
static_assert(std::is_nothrow_copy_assignable_v<TripProgress>);
bool valid_trip_identifier(const TripIdentifier& identifier) noexcept {
  if (identifier.length == 0 || identifier.length > identifier.bytes.size() ||
      !letter(identifier.bytes[0]))
    return false;
  for (std::size_t i = 0; i < identifier.length; ++i)
    if (!character(identifier.bytes[i])) return false;
  for (std::size_t i = identifier.length; i < identifier.bytes.size(); ++i)
    if (identifier.bytes[i] != 0) return false;
  return true;
}
std::optional<TripIdentifier> parse_trip_identifier(std::string_view key) noexcept {
  if (key.empty() || key.size() > 32 || !letter(key.front())) return std::nullopt;
  for (const auto value : key)
    if (!character(value)) return std::nullopt;
  TripIdentifier result;
  result.length = static_cast<std::uint8_t>(key.size());
  std::copy(key.begin(), key.end(), result.bytes.begin());
  return result;
}
std::string_view trip_identifier_key(const TripIdentifier& identifier) noexcept {
  if (!valid_trip_identifier(identifier)) return {};
  return {identifier.bytes.data(), identifier.length};
}
bool valid_trip_state(const TripState& trip) noexcept {
  if (!valid_trip_identifier(trip.identifier) || !trip.reset_generation) return false;
  if (!bounded(trip.distance_km, maximum_trip_distance_km) ||
      !bounded(trip.elapsed_seconds, maximum_trip_duration_seconds) ||
      !bounded(trip.moving_seconds, maximum_trip_duration_seconds) ||
      !bounded(trip.average_speed_kmh, maximum_trip_average_speed_kmh) ||
      !bounded(trip.fuel_efficiency_l_per_100km, maximum_trip_fuel_efficiency_l_per_100km))
    return false;
  if (!trip.distance_km && !trip.elapsed_seconds && !trip.moving_seconds &&
      !trip.average_speed_kmh && !trip.fuel_efficiency_l_per_100km)
    return false;
  if (trip.moving_seconds && trip.elapsed_seconds && *trip.moving_seconds > *trip.elapsed_seconds)
    return false;
  if (bool(trip.average_speed_kmh) != bool(trip.average_speed_basis)) return false;
  if (trip.average_speed_kmh) {
    if (*trip.average_speed_basis != TripAverageSpeedBasis::total &&
        *trip.average_speed_basis != TripAverageSpeedBasis::moving)
      return false;
    const auto& duration = *trip.average_speed_basis == TripAverageSpeedBasis::total
                               ? trip.elapsed_seconds
                               : trip.moving_seconds;
    if (!trip.distance_km || !duration || *duration <= 0) return false;
    const auto expected = *trip.distance_km * 3600 / *duration;
    if (!std::isfinite(expected)) return false;
    const auto epsilon = 16 * std::numeric_limits<double>::epsilon() *
                         std::max({1., expected, *trip.average_speed_kmh});
    if (std::abs(*trip.average_speed_kmh - expected) >
        trip_average_speed_rounding_tolerance_kmh + epsilon)
      return false;
  }
  if (trip.fuel_efficiency_l_per_100km && (!trip.distance_km || *trip.distance_km <= 0))
    return false;
  return true;
}
std::string trip_state_key(const TripState& trip) {
  if (!valid_trip_state(trip)) return {};
  std::string output(trip_identifier_key(trip.identifier));
  output += '/';
  output += std::to_string(trip.reset_generation);
  output += '/';
  append_number(output, trip.distance_km);
  output += '/';
  append_number(output, trip.elapsed_seconds);
  output += '/';
  append_number(output, trip.moving_seconds);
  output += '/';
  append_number(output, trip.average_speed_kmh);
  output += '/';
  output += !trip.average_speed_basis                                   ? "-"
            : *trip.average_speed_basis == TripAverageSpeedBasis::total ? "total"
                                                                        : "moving";
  output += '/';
  append_number(output, trip.fuel_efficiency_l_per_100km);
  return output;
}
std::optional<TripState> parse_trip_state_key(std::string_view key) noexcept {
  if (key.empty() || key.size() > 512) return std::nullopt;
  std::array<std::string_view, 8> parts{};
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto slash = key.find('/');
    parts[i] = key.substr(0, slash);
    if (i + 1 == parts.size()) {
      if (slash != std::string_view::npos) return std::nullopt;
    } else {
      if (slash == std::string_view::npos) return std::nullopt;
      key.remove_prefix(slash + 1);
    }
  }
  TripState output;
  const auto identifier = parse_trip_identifier(parts[0]);
  if (!identifier) return std::nullopt;
  output.identifier = *identifier;
  if (parts[1].empty() || parts[1].size() > 20 || parts[1][0] < '1' || parts[1][0] > '9')
    return std::nullopt;
  const auto reset =
      std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), output.reset_generation);
  if (reset.ec != std::errc{} || reset.ptr != parts[1].data() + parts[1].size())
    return std::nullopt;
  if (!parse_number(parts[2], output.distance_km) ||
      !parse_number(parts[3], output.elapsed_seconds) ||
      !parse_number(parts[4], output.moving_seconds) ||
      !parse_number(parts[5], output.average_speed_kmh) ||
      !parse_number(parts[7], output.fuel_efficiency_l_per_100km))
    return std::nullopt;
  if (parts[6] == "total")
    output.average_speed_basis = TripAverageSpeedBasis::total;
  else if (parts[6] == "moving")
    output.average_speed_basis = TripAverageSpeedBasis::moving;
  else if (parts[6] != "-")
    return std::nullopt;
  if (!valid_trip_state(output)) return std::nullopt;
  return output;
}
bool valid_trip_progress(const TripProgress& progress) noexcept {
  if (!progress.identifier) return progress == TripProgress{};
  if (!valid_trip_identifier(*progress.identifier) || !progress.reset_generation ||
      !progress.acquired_at || *progress.acquired_at <= std::chrono::steady_clock::time_point{} ||
      !progress.last_observation || !valid_trip_state(*progress.last_observation) ||
      progress.last_observation->identifier != *progress.identifier ||
      progress.last_observation->reset_generation != progress.reset_generation)
    return false;
  if (!bounded(progress.distance_km, maximum_trip_distance_km) ||
      !bounded(progress.elapsed_seconds, maximum_trip_duration_seconds) ||
      !bounded(progress.moving_seconds, maximum_trip_duration_seconds))
    return false;
  auto valid_date = [&](const auto& counter, const auto& date) {
    return bool(counter) == bool(date) &&
           (!date ||
            (*date > std::chrono::steady_clock::time_point{} && *date <= *progress.acquired_at));
  };
  if (!valid_date(progress.distance_km, progress.distance_acquired_at) ||
      !valid_date(progress.elapsed_seconds, progress.elapsed_acquired_at) ||
      !valid_date(progress.moving_seconds, progress.moving_acquired_at))
    return false;
  if (progress.elapsed_seconds && progress.moving_seconds &&
      *progress.elapsed_acquired_at >= *progress.moving_acquired_at &&
      *progress.moving_seconds > *progress.elapsed_seconds)
    return false;
  const auto& last = *progress.last_observation;
  if ((last.distance_km && last.distance_km != progress.distance_km) ||
      (last.elapsed_seconds && last.elapsed_seconds != progress.elapsed_seconds) ||
      (last.moving_seconds && last.moving_seconds != progress.moving_seconds))
    return false;
  if ((last.distance_km && progress.distance_acquired_at != progress.acquired_at) ||
      (last.elapsed_seconds && progress.elapsed_acquired_at != progress.acquired_at) ||
      (last.moving_seconds && progress.moving_acquired_at != progress.acquired_at))
    return false;
  return true;
}
bool withdraw_trip_progress(TripProgress& progress) noexcept {
  if (!valid_trip_progress(progress)) return false;
  if (progress.identifier) progress.requires_new_acquisition = true;
  return true;
}
bool update_trip_progress(TripProgress& progress, const TripState& trip,
                          std::chrono::steady_clock::time_point acquired) noexcept {
  if (!valid_trip_progress(progress) || !valid_trip_state(trip) ||
      acquired <= std::chrono::steady_clock::time_point{})
    return false;
  auto candidate = progress;
  if (progress.identifier) {
    if (trip.identifier != *progress.identifier ||
        trip.reset_generation < progress.reset_generation || acquired < *progress.acquired_at)
      return false;
    if (acquired == *progress.acquired_at)
      return !progress.requires_new_acquisition && progress.last_observation == trip;
  }
  const bool reset = !progress.identifier || trip.reset_generation > progress.reset_generation;
  if (reset) {
    candidate.distance_km.reset();
    candidate.elapsed_seconds.reset();
    candidate.moving_seconds.reset();
    candidate.distance_acquired_at.reset();
    candidate.elapsed_acquired_at.reset();
    candidate.moving_acquired_at.reset();
  }
  auto advance = [&](auto& previous, auto& previous_date, const auto& incoming) {
    if (!incoming) return true;
    if (previous && *incoming < *previous) return false;
    previous = incoming;
    previous_date = acquired;
    return true;
  };
  if (!advance(candidate.distance_km, candidate.distance_acquired_at, trip.distance_km) ||
      !advance(candidate.elapsed_seconds, candidate.elapsed_acquired_at, trip.elapsed_seconds) ||
      !advance(candidate.moving_seconds, candidate.moving_acquired_at, trip.moving_seconds))
    return false;
  candidate.identifier = trip.identifier;
  candidate.reset_generation = trip.reset_generation;
  candidate.acquired_at = acquired;
  candidate.last_observation = trip;
  candidate.requires_new_acquisition = false;
  if (!valid_trip_progress(candidate)) return false;
  progress = candidate;
  return true;
}
}  // namespace headunit
