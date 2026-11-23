// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_observation_wire.hpp>
#include <headunit/vehicle_observation.hpp>
#include <array>
#include <bitset>
#include <charconv>
#include <cmath>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <type_traits>

namespace headunit {
namespace {
static_assert(reading_definitions.size() + indicator_definitions.size() ==
              legacy_vehicle_observation_capacity);
static_assert(legacy_vehicle_observation_capacity == 34 &&
              transmission_vehicle_observation_capacity == 35 &&
              drive_mode_vehicle_observation_capacity == 36 &&
              trip_heading_vehicle_observation_capacity == 38 &&
              vehicle_observation_capacity == 40);
using NanosecondsPerTick = std::ratio_divide<Clock::duration::period, std::nano>;
using Rep = Clock::duration::rep;
static_assert(std::is_integral_v<Rep> && sizeof(Rep) <= sizeof(std::uint64_t));
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
bool number(std::string_view text, std::uint64_t& result) {
  if (text.empty() || text.size() > 20) return false;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  return error == std::errc{} && end == text.data() + text.size() && result > 0;
}
// Reduced integer ratios avoid overflowing duration_cast and reject clocks
// that cannot represent a source nanosecond date exactly. No floating clocks.
bool from_ns(std::uint64_t value, Clock::duration& result) {
  constexpr auto numerator = std::uint64_t(NanosecondsPerTick::num),
                 denominator = std::uint64_t(NanosecondsPerTick::den);
  if (!value || value % numerator) return false;
  const auto units = value / numerator;
  if (units > std::uint64_t(std::numeric_limits<Rep>::max()) / denominator) return false;
  result = Clock::duration{static_cast<Rep>(units * denominator)};
  return result > Clock::duration::zero();
}
bool as_ns(Clock::duration value, std::uint64_t& result) {
  constexpr auto numerator = std::uint64_t(NanosecondsPerTick::num),
                 denominator = std::uint64_t(NanosecondsPerTick::den);
  if (value <= Clock::duration::zero()) return false;
  const auto units = std::uint64_t(value.count());
  if (units % denominator ||
      units / denominator > std::numeric_limits<std::uint64_t>::max() / numerator)
    return false;
  result = (units / denominator) * numerator;
  return result > 0;
}
std::uint64_t exact_ns(Clock::duration duration) {
  std::uint64_t value = 0;
  check(as_ns(duration, value), "HVO requires exact positive host nanoseconds");
  return value;
}
template <std::size_t N>
bool split(std::string_view text, char separator, std::array<std::string_view, N>& output) {
  for (std::size_t i = 0; i < N; ++i) {
    const auto next = text.find(separator);
    if ((i + 1 == N) != (next == text.npos)) return false;
    output[i] = text.substr(0, next);
    if (output[i].empty()) return false;
    if (next != text.npos) text.remove_prefix(next + 1);
  }
  return true;
}
bool reading(std::string_view text, const ReadingDefinition& definition,
             std::optional<double>& result) {
  if (text == "-") {
    result.reset();
    return true;
  }
  double value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) ||
      value < definition.minimum || value > definition.maximum)
    return false;
  result = value;
  return true;
}
bool indicator(std::string_view text, std::optional<IndicatorState>& result) {
  if (text == "-")
    result.reset();
  else if (text == "off")
    result = IndicatorState::off;
  else if (text == "on")
    result = IndicatorState::on;
  else if (text == "flash")
    result = IndicatorState::flashing;
  else
    return false;
  return true;
}
bool transmission(std::string_view text, std::optional<TransmissionState>& result) {
  if (text == "-") {
    result.reset();
    return true;
  }
  result = parse_transmission_state_key(text);
  return result.has_value();
}
bool default_unknown_signal(const VehicleSignalTiming& signal) {
  return !signal.acquired_at && signal.maximum_age == std::chrono::milliseconds(500) &&
         signal.origin == VehicleObservationOrigin::measured &&
         signal.availability == VehicleAvailability::unavailable;
}
bool legacy_transmission_unknown(const VehicleSample& sample) {
  return !sample.transmission && default_unknown_signal(sample.observation_timing->transmission);
}
bool legacy_drive_mode_unknown(const VehicleSample& sample) {
  return !sample.drive_mode_catalog && !sample.drive_mode &&
         default_unknown_signal(sample.observation_timing->drive_mode);
}
bool drive_mode(std::string_view text, std::optional<DriveModeIdentifier>& result) {
  if (text == "-") {
    result.reset();
    return true;
  }
  result = parse_drive_mode_identifier(text);
  return result.has_value();
}
bool legacy_trip_heading_unknown(const VehicleSample& sample) {
  return !sample.trip && !sample.heading &&
         default_unknown_signal(sample.observation_timing->trip) &&
         default_unknown_signal(sample.observation_timing->heading);
}
bool trip(std::string_view text, std::optional<TripState>& result) {
  if (text == "-") {
    result.reset();
    return true;
  }
  result = parse_trip_state_key(text);
  return result.has_value();
}
bool heading(std::string_view text, std::optional<HeadingState>& result) {
  if (text == "-") {
    result.reset();
    return true;
  }
  result = parse_heading_state_key(text);
  return result.has_value();
}
bool signal_timing(const std::array<std::string_view, 5>& fields, VehicleSignalTiming& result) {
  std::uint64_t nanos = 0;
  Clock::duration duration;
  if (fields[1] != "-") {
    if (!number(fields[1], nanos) || !from_ns(nanos, duration)) return false;
    result.acquired_at = Clock::time_point{duration};
  }
  if (!number(fields[2], nanos) || !from_ns(nanos, duration) ||
      duration > maximum_vehicle_signal_age)
    return false;
  result.maximum_age = duration;
  if (fields[3] == "m")
    result.origin = VehicleObservationOrigin::measured;
  else if (fields[3] == "c")
    result.origin = VehicleObservationOrigin::calculated;
  else if (fields[3] == "s")
    result.origin = VehicleObservationOrigin::simulation;
  else
    return false;
  if (fields[4] == "a")
    result.availability = VehicleAvailability::available;
  else if (fields[4] == "u")
    result.availability = VehicleAvailability::unavailable;
  else if (fields[4] == "s")
    result.availability = VehicleAvailability::stale;
  else if (fields[4] == "d")
    result.availability = VehicleAvailability::disconnected;
  else
    return false;
  return true;
}
bool consistent(const VehicleSignalTiming& signal, Clock::time_point published) {
  std::uint64_t nanos = 0;
  if (!as_ns(signal.maximum_age, nanos)) return false;
  if (signal.acquired_at && !as_ns(signal.acquired_at->time_since_epoch(), nanos)) return false;
  if (signal.availability == VehicleAvailability::available)
    return signal.acquired_at && published - *signal.acquired_at < signal.maximum_age;
  if (signal.availability == VehicleAvailability::stale)
    return signal.acquired_at && published - *signal.acquired_at >= signal.maximum_age;
  return true;  // Shape/date/value/origin checks are shared with valid_vehicle_sample.
}
char origin(VehicleObservationOrigin value) {
  switch (value) {
    case VehicleObservationOrigin::measured:
      return 'm';
    case VehicleObservationOrigin::calculated:
      return 'c';
    case VehicleObservationOrigin::simulation:
      return 's';
  }
  throw std::invalid_argument("invalid HVO observation origin");
}
char availability(VehicleAvailability value) {
  switch (value) {
    case VehicleAvailability::available:
      return 'a';
    case VehicleAvailability::unavailable:
      return 'u';
    case VehicleAvailability::stale:
      return 's';
    case VehicleAvailability::disconnected:
      return 'd';
  }
  throw std::invalid_argument("invalid HVO availability");
}
void append_timing(std::string& text, const VehicleSignalTiming& signal) {
  text += ',';
  text +=
      signal.acquired_at ? std::to_string(exact_ns(signal.acquired_at->time_since_epoch())) : "-";
  text += ',';
  text += std::to_string(exact_ns(signal.maximum_age));
  text += ',';
  text += origin(signal.origin);
  text += ',';
  text += availability(signal.availability);
}
}  // namespace
bool valid_vehicle_observation_packet(const VehicleObservationPacket& packet,
                                      Clock::time_point received) {
  const auto& sample = packet.sample;
  if (!packet.sequence || !sample.observation_timing || !valid_vehicle_sample(sample) ||
      sample.sampled_at > received)
    return false;
  if (packet.version != VehicleObservationWireVersion::hvo4 &&
      packet.version != VehicleObservationWireVersion::hvo6 && !legacy_trip_heading_unknown(sample))
    return false;
  if (packet.version != VehicleObservationWireVersion::hvo6 &&
      (sample.acceleration || sample.gps_position ||
       !default_unknown_signal(sample.observation_timing->acceleration) ||
       !default_unknown_signal(sample.observation_timing->gps_position)))
    return false;
  switch (packet.version) {
    case VehicleObservationWireVersion::hvo1:
      if (!legacy_transmission_unknown(sample) || !legacy_drive_mode_unknown(sample)) return false;
      break;
    case VehicleObservationWireVersion::hvo2:
      if (!legacy_drive_mode_unknown(sample)) return false;
      break;
    case VehicleObservationWireVersion::hvo3:
    case VehicleObservationWireVersion::hvo4:
    case VehicleObservationWireVersion::hvo6:
      break;
    default:
      return false;
  }
  const auto& timing = *sample.observation_timing;
  std::uint64_t nanos = 0;
  if (!as_ns(timing.started_at.time_since_epoch(), nanos) ||
      !as_ns(sample.sampled_at.time_since_epoch(), nanos))
    return false;
  for (std::size_t i = 0; i < vehicle_observation_capacity; ++i)
    if (!consistent(vehicle_signal_timing(timing, i), sample.sampled_at)) return false;
  return true;
}
namespace {
template <std::size_t Count, bool Catalog = false>
std::optional<VehicleObservationPacket> parse_complete(std::string_view text,
                                                       Clock::time_point received,
                                                       VehicleObservationWireVersion version) {
  constexpr std::size_t header_count = Catalog ? 6 : 5;
  std::array<std::string_view, header_count + Count> tokens;
  if (!split(text, ' ', tokens)) return {};
  VehicleObservationPacket packet;
  packet.version = version;
  auto& sample = packet.sample;
  sample.observation_timing.emplace();
  auto& timing = *sample.observation_timing;
  std::uint64_t nanos = 0;
  Clock::duration duration;
  if (!number(tokens[1], timing.generation) || !number(tokens[2], packet.sequence) ||
      !number(tokens[3], nanos) || !from_ns(nanos, duration))
    return {};
  timing.started_at = Clock::time_point{duration};
  if (!number(tokens[4], nanos) || !from_ns(nanos, duration)) return {};
  sample.sampled_at = Clock::time_point{duration};
  std::bitset<Count> seen;
  if constexpr (Catalog) {
    constexpr std::string_view prefix = "drive_modes=";
    if (!tokens[5].starts_with(prefix)) return {};
    const auto catalog = tokens[5].substr(prefix.size());
    if (catalog != "-") {
      sample.drive_mode_catalog = parse_drive_mode_catalog_key(catalog);
      if (!sample.drive_mode_catalog) return {};
    }
  }
  for (std::size_t token = header_count; token < tokens.size(); ++token) {
    const auto item = tokens[token];
    const auto equal = item.find('=');
    if (equal == item.npos || !equal || equal + 1 == item.size() ||
        item.find('=', equal + 1) != item.npos)
      return {};
    const auto key = item.substr(0, equal);
    std::array<std::string_view, 5> fields;
    if (!split(item.substr(equal + 1), ',', fields)) return {};
    const auto* definition = find_vehicle_reading(key);
    const auto* lamp = find_vehicle_indicator(key);
    const bool is_transmission =
        Count >= transmission_vehicle_observation_capacity && key == "transmission";
    const bool is_drive_mode = Catalog && key == "drive_mode";
    const bool is_trip = Count >= trip_heading_vehicle_observation_capacity && key == "trip";
    const bool is_heading = Count >= trip_heading_vehicle_observation_capacity && key == "heading";
    const bool is_acceleration = Count == vehicle_observation_capacity && key == "acceleration";
    const bool is_gps = Count == vehicle_observation_capacity && key == "gps_position";
    if (!definition && !lamp && !is_transmission && !is_drive_mode && !is_trip && !is_heading &&
        !is_acceleration && !is_gps)
      return {};
    const auto index = definition        ? std::size_t(definition - reading_definitions.data())
                       : lamp            ? reading_definitions.size() + std::size_t(lamp->id)
                       : is_transmission ? transmission_observation_index
                       : is_drive_mode   ? drive_mode_observation_index
                       : is_trip         ? trip_observation_index
                       : is_heading      ? heading_observation_index
                       : is_acceleration ? acceleration_observation_index
                                         : gps_position_observation_index;
    if (seen.test(index)) return {};
    seen.set(index);
    auto& signal = vehicle_signal_timing(timing, index);
    if (!signal_timing(fields, signal)) return {};
    if (definition) {
      if (!reading(fields[0], *definition, sample.*definition->member)) return {};
    } else if (lamp) {
      if (!indicator(fields[0], sample.indicator(lamp->id))) return {};
    } else if (is_transmission) {
      if (!transmission(fields[0], sample.transmission)) return {};
    } else if (is_drive_mode) {
      if (!drive_mode(fields[0], sample.drive_mode)) return {};
    } else if (is_trip) {
      if (!trip(fields[0], sample.trip)) return {};
    } else if (is_heading) {
      if (!heading(fields[0], sample.heading)) return {};
    } else if (is_acceleration) {
      if (fields[0] != "-") {
        sample.acceleration = parse_acceleration_state_key(fields[0]);
        if (!sample.acceleration) return {};
      }
    } else if (fields[0] != "-") {
      sample.gps_position = parse_gps_position_state_key(fields[0]);
      if (!sample.gps_position) return {};
    }
    sample.simulated |= signal.acquired_at && signal.origin == VehicleObservationOrigin::simulation;
  }
  if (!seen.all() || !valid_vehicle_observation_packet(packet, received)) return {};
  return packet;
}
}  // namespace
std::optional<VehicleObservationPacket> parse_vehicle_observation_datagram(
    std::string_view text, Clock::time_point received) {
  if (text.empty() || text.size() > maximum_vehicle_observation_datagram ||
      text.find('\0') != text.npos || text.find_first_of("\r\n\t\v\f") != text.npos)
    return {};
  if (text.starts_with("HVO1 "))
    return parse_complete<legacy_vehicle_observation_capacity>(text, received,
                                                               VehicleObservationWireVersion::hvo1);
  if (text.starts_with("HVO2 "))
    return parse_complete<transmission_vehicle_observation_capacity>(
        text, received, VehicleObservationWireVersion::hvo2);
  if (text.starts_with("HVO3 "))
    return parse_complete<drive_mode_vehicle_observation_capacity, true>(
        text, received, VehicleObservationWireVersion::hvo3);
  if (text.starts_with("HVO4 "))
    return parse_complete<trip_heading_vehicle_observation_capacity, true>(
        text, received, VehicleObservationWireVersion::hvo4);
  if (text.starts_with("HVO6 "))
    return parse_complete<vehicle_observation_capacity, true>(text, received,
                                                              VehicleObservationWireVersion::hvo6);
  return {};
}
std::string encode_vehicle_observation_datagram(const VehicleObservationPacket& packet) {
  check(valid_vehicle_observation_packet(packet, packet.sample.sampled_at),
        "invalid complete vehicle observation snapshot");
  const auto& sample = packet.sample;
  const auto& timing = *sample.observation_timing;
  std::string text = packet.version == VehicleObservationWireVersion::hvo1   ? "HVO1 "
                     : packet.version == VehicleObservationWireVersion::hvo2 ? "HVO2 "
                     : packet.version == VehicleObservationWireVersion::hvo3 ? "HVO3 "
                     : packet.version == VehicleObservationWireVersion::hvo4 ? "HVO4 "
                                                                             : "HVO6 ";
  text += std::to_string(timing.generation) + ' ' + std::to_string(packet.sequence) + ' ' +
          std::to_string(exact_ns(timing.started_at.time_since_epoch())) + ' ' +
          std::to_string(exact_ns(sample.sampled_at.time_since_epoch()));
  text.reserve(4096);
  if (packet.version == VehicleObservationWireVersion::hvo3 ||
      packet.version == VehicleObservationWireVersion::hvo4 ||
      packet.version == VehicleObservationWireVersion::hvo6) {
    text += " drive_modes=";
    text += sample.drive_mode_catalog ? drive_mode_catalog_key(*sample.drive_mode_catalog) : "-";
  }
  for (std::size_t i = 0; i < reading_definitions.size(); ++i) {
    const auto& definition = reading_definitions[i];
    text += ' ';
    text += definition.key;
    text += '=';
    if (const auto& value = sample.*definition.member; value) {
      char buffer[64];
      const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), *value);
      check(error == std::errc{}, "vehicle observation numeric encoding failed");
      text.append(buffer, end);
    } else
      text += '-';
    append_timing(text, timing.readings[i]);
  }
  for (const auto& definition : indicator_definitions) {
    text += ' ';
    text += definition.key;
    text += '=';
    const auto& state = sample.indicator(definition.id);
    text += !state                          ? "-"
            : *state == IndicatorState::off ? "off"
            : *state == IndicatorState::on  ? "on"
                                            : "flash";
    append_timing(text, timing.indicators[std::size_t(definition.id)]);
  }
  if (packet.version != VehicleObservationWireVersion::hvo1) {
    text += " transmission=";
    text += sample.transmission ? transmission_state_key(*sample.transmission) : "-";
    append_timing(text, timing.transmission);
  }
  if (packet.version == VehicleObservationWireVersion::hvo3 ||
      packet.version == VehicleObservationWireVersion::hvo4 ||
      packet.version == VehicleObservationWireVersion::hvo6) {
    text += " drive_mode=";
    text += sample.drive_mode ? drive_mode_identifier_key(*sample.drive_mode) : "-";
    append_timing(text, timing.drive_mode);
  }
  if (packet.version == VehicleObservationWireVersion::hvo4 ||
      packet.version == VehicleObservationWireVersion::hvo6) {
    text += " trip=";
    text += sample.trip ? trip_state_key(*sample.trip) : "-";
    append_timing(text, timing.trip);
    text += " heading=";
    text += sample.heading ? heading_state_key(*sample.heading) : "-";
    append_timing(text, timing.heading);
  }
  if (packet.version == VehicleObservationWireVersion::hvo6) {
    text += " acceleration=";
    text += sample.acceleration ? acceleration_state_key(*sample.acceleration) : "-";
    append_timing(text, timing.acceleration);
    text += " gps_position=";
    text += sample.gps_position ? gps_position_state_key(*sample.gps_position) : "-";
    append_timing(text, timing.gps_position);
  }
  check(text.size() <= maximum_vehicle_observation_datagram,
        "complete vehicle observation snapshot exceeds bound");
  return text;
}
}  // namespace headunit
