// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/sport_observation.hpp>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace headunit {
namespace {
template <std::size_t N>
std::optional<std::array<double, N>> parse(std::string_view text) {
  if (text.empty() || text.size() > 192) return {};
  std::array<double, N> values{};
  for (std::size_t i = 0; i < N; ++i) {
    const auto separator = text.find('/');
    if ((i + 1 == N) != (separator == text.npos)) return {};
    const auto part = text.substr(0, separator);
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), values[i]);
    if (part.empty() || error != std::errc{} || end != part.data() + part.size() ||
        !std::isfinite(values[i]))
      return {};
    if (separator != text.npos) text.remove_prefix(separator + 1);
  }
  return values;
}
std::string number(double value) {
  char buffer[64];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) throw std::invalid_argument("sport observation numeric encoding");
  return {buffer, end};
}
}  // namespace
bool valid_acceleration_state(const AccelerationState& value) {
  return std::isfinite(value.longitudinal_mps2) && std::isfinite(value.lateral_mps2) &&
         std::abs(value.longitudinal_mps2) <= 100 && std::abs(value.lateral_mps2) <= 100;
}
bool valid_gps_position_state(const GpsPositionState& value) {
  return std::isfinite(value.latitude_deg) && std::abs(value.latitude_deg) <= 90 &&
         std::isfinite(value.longitude_deg) && std::abs(value.longitude_deg) <= 180 &&
         std::isfinite(value.horizontal_accuracy_m) && value.horizontal_accuracy_m > 0 &&
         value.horizontal_accuracy_m <= 10000;
}
std::string acceleration_state_key(const AccelerationState& value) {
  if (!valid_acceleration_state(value)) throw std::invalid_argument("invalid acceleration");
  return number(value.longitudinal_mps2) + '/' + number(value.lateral_mps2);
}
std::string gps_position_state_key(const GpsPositionState& value) {
  if (!valid_gps_position_state(value)) throw std::invalid_argument("invalid GPS position");
  return number(value.latitude_deg) + '/' + number(value.longitude_deg) + '/' +
         number(value.horizontal_accuracy_m);
}
std::optional<AccelerationState> parse_acceleration_state_key(std::string_view text) {
  const auto values = parse<2>(text);
  if (!values) return {};
  const AccelerationState result{(*values)[0], (*values)[1]};
  return valid_acceleration_state(result) ? std::optional{result} : std::nullopt;
}
std::optional<GpsPositionState> parse_gps_position_state_key(std::string_view text) {
  const auto values = parse<3>(text);
  if (!values) return {};
  const GpsPositionState result{(*values)[0], (*values)[1], (*values)[2]};
  return valid_gps_position_state(result) ? std::optional{result} : std::nullopt;
}
}  // namespace headunit
