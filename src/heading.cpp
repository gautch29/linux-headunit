// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/heading.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>

namespace headunit {
namespace {
std::string_view number_key(double value, std::array<char, 64>& buffer) {
  if (value == 0) value = 0;
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                    std::chars_format::general);
  if (result.ec != std::errc{}) return {};
  return {buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())};
}
}  // namespace
static_assert(std::is_trivially_copyable_v<HeadingState>);
bool valid_heading_state(const HeadingState& heading) noexcept {
  return std::isfinite(heading.degrees) && heading.degrees >= 0 && heading.degrees < 360 &&
         (heading.reference == HeadingReference::true_north ||
          heading.reference == HeadingReference::magnetic_north) &&
         (heading.source == HeadingSource::vehicle_orientation ||
          heading.source == HeadingSource::course_over_ground);
}
std::string heading_state_key(const HeadingState& heading) {
  if (!valid_heading_state(heading)) return {};
  std::array<char, 64> buffer{};
  std::string output(number_key(heading.degrees, buffer));
  output += '/';
  output += heading.reference == HeadingReference::true_north ? "true_north" : "magnetic_north";
  output += '/';
  output += heading.source == HeadingSource::vehicle_orientation ? "vehicle_orientation"
                                                                 : "course_over_ground";
  return output;
}
std::optional<HeadingState> parse_heading_state_key(std::string_view key) noexcept {
  if (key.empty() || key.size() > 128) return std::nullopt;
  const auto first = key.find('/');
  if (first == std::string_view::npos) return std::nullopt;
  const auto second = key.find('/', first + 1);
  if (second == std::string_view::npos || key.find('/', second + 1) != std::string_view::npos)
    return std::nullopt;
  const auto degrees = key.substr(0, first), reference = key.substr(first + 1, second - first - 1),
             source = key.substr(second + 1);
  if (degrees.empty() || degrees.size() > 64) return std::nullopt;
  HeadingState output;
  const auto parsed = std::from_chars(degrees.data(), degrees.data() + degrees.size(),
                                      output.degrees, std::chars_format::general);
  if (parsed.ec != std::errc{} || parsed.ptr != degrees.data() + degrees.size())
    return std::nullopt;
  std::array<char, 64> canonical{};
  if (number_key(output.degrees, canonical) != degrees) return std::nullopt;
  if (reference == "true_north")
    output.reference = HeadingReference::true_north;
  else if (reference == "magnetic_north")
    output.reference = HeadingReference::magnetic_north;
  else
    return std::nullopt;
  if (source == "vehicle_orientation")
    output.source = HeadingSource::vehicle_orientation;
  else if (source == "course_over_ground")
    output.source = HeadingSource::course_over_ground;
  else
    return std::nullopt;
  if (!valid_heading_state(output)) return std::nullopt;
  return output;
}
}  // namespace headunit
