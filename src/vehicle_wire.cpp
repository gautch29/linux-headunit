// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_wire.hpp>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
#include <system_error>

namespace headunit {
namespace {
bool parse_reading(std::string_view field, double lower, double upper,
                   std::optional<double>& output) {
  if (field == "-") {
    output.reset();
    return true;
  }
  double value = 0;
  const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
  if (error != std::errc{} || end != field.data() + field.size() || !std::isfinite(value) ||
      value < lower || value > upper)
    return false;
  output = value;
  return true;
}
}  // namespace
std::optional<VehicleSample> parse_vehicle_datagram(std::string_view text,
                                                    Clock::time_point received_at) {
  if (text.size() > maximum_vehicle_transport_datagram || text.find('\0') != text.npos)
    return std::nullopt;
  auto sampled_at = received_at;
  if (text.starts_with("HUF1 ")) {
    text.remove_prefix(5);
    const auto space = text.find(' ');
    if (space == text.npos || !space || space > 19) return std::nullopt;
    const auto stamp = text.substr(0, space);
    std::int64_t nanos = 0;
    const auto [end, error] = std::from_chars(stamp.data(), stamp.data() + stamp.size(), nanos);
    if (error != std::errc{} || end != stamp.data() + stamp.size() || nanos <= 0)
      return std::nullopt;
    sampled_at = Clock::time_point(
        std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(nanos)));
    if (sampled_at > received_at) return std::nullopt;
    text.remove_prefix(space + 1);
    // No recursion: only one broker envelope around the original HUV packet.
  }
  if (text.size() > maximum_vehicle_datagram) return std::nullopt;
  if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
  VehicleSample result{};
  result.sampled_at = sampled_at;
  if (text.starts_with("HUV1 ")) {
    text.remove_prefix(5);
    const auto space = text.find(' ');
    if (space == text.npos || text.find(' ', space + 1) != text.npos ||
        !parse_reading(text.substr(0, space), 0, 300, result.speed_kmh) ||
        !parse_reading(text.substr(space + 1), 0, 9000, result.engine_rpm))
      return std::nullopt;
    return result;
  }
  if (!text.starts_with("HUV2 ") || text.size() == 5) return std::nullopt;
  text.remove_prefix(5);
  std::set<std::string_view> seen;
  while (!text.empty()) {
    const auto space = text.find(' ');
    const auto item = text.substr(0, space);
    const auto equal = item.find('=');
    if (equal == item.npos || equal == 0 || equal + 1 == item.size()) return std::nullopt;
    const auto key = item.substr(0, equal), value = item.substr(equal + 1);
    if (!seen.insert(key).second) return std::nullopt;
    if (key == "simulated") {
      if (value != "0" && value != "1") return std::nullopt;
      result.simulated = value == "1";
    } else if (const auto* reading = find_vehicle_reading(key)) {
      if (!parse_reading(value, reading->minimum, reading->maximum, result.*reading->member))
        return std::nullopt;
    } else if (const auto* indicator = find_vehicle_indicator(key)) {
      auto& state = result.indicator(indicator->id);
      if (value == "-")
        state.reset();
      else if (value == "off")
        state = IndicatorState::off;
      else if (value == "on")
        state = IndicatorState::on;
      else if (value == "flash")
        state = IndicatorState::flashing;
      else
        return std::nullopt;
    } else
      return std::nullopt;
    if (space == text.npos) break;
    if (space + 1 == text.size()) return std::nullopt;
    text.remove_prefix(space + 1);
  }
  return result;
}
std::string encode_vehicle_datagram(const VehicleSample& sample) {
  if (sample.trip || sample.heading || sample.acceleration || sample.gps_position)
    throw std::invalid_argument("HUV2 cannot preserve trip or heading state");
  if (sample.drive_mode_catalog || sample.drive_mode)
    throw std::invalid_argument("HUV2 cannot preserve drive modes");
  if (sample.transmission) throw std::invalid_argument("HUV2 cannot preserve transmission state");
  if (sample.observation_timing)
    throw std::invalid_argument("HUV2 cannot preserve per-signal acquisition metadata");
  if (!valid_vehicle_sample(sample)) throw std::invalid_argument("invalid vehicle sample");
  std::string text = sample.simulated ? "HUV2 simulated=1" : "HUV2 simulated=0";
  for (const auto& item : reading_definitions) {
    text += ' ';
    text += item.key;
    text += '=';
    if (const auto& value = sample.*item.member; value) {
      char buffer[64];
      const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), *value);
      if (error != std::errc{}) throw std::runtime_error("vehicle number encoding failed");
      text.append(buffer, end);
    } else
      text += '-';
  }
  for (const auto& item : indicator_definitions) {
    text += ' ';
    text += item.key;
    text += '=';
    const auto& state = sample.indicator(item.id);
    text += !state                          ? "-"
            : *state == IndicatorState::off ? "off"
            : *state == IndicatorState::on  ? "on"
                                            : "flash";
  }
  if (text.size() > maximum_vehicle_datagram)
    throw std::runtime_error("vehicle snapshot is too large");
  return text;
}
}  // namespace headunit
