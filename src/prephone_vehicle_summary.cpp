// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_vehicle_summary.hpp>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace headunit {
namespace {
struct RowDefinition {
  std::size_t reading;
  std::string_view group, label;
};
constexpr std::array definitions{RowDefinition{2, "energy", "Fuel"},
                                 RowDefinition{8, "energy", "Range"},
                                 RowDefinition{6, "energy", "Battery voltage"},
                                 RowDefinition{7, "temperatures", "Outside temperature"},
                                 RowDefinition{3, "temperatures", "Coolant temperature"},
                                 RowDefinition{4, "temperatures", "Oil temperature"},
                                 RowDefinition{5, "engine", "Oil pressure"},
                                 RowDefinition{9, "distance", "Odometer"}};
VehicleSummaryAvailability availability(const VehicleSignalTiming& signal) {
  switch (signal.availability) {
    case VehicleAvailability::available:
      return VehicleSummaryAvailability::available;
    case VehicleAvailability::unavailable:
      return signal.acquired_at ? VehicleSummaryAvailability::unavailable
                                : VehicleSummaryAvailability::missing;
    case VehicleAvailability::stale:
      return VehicleSummaryAvailability::stale;
    case VehicleAvailability::disconnected:
      return VehicleSummaryAvailability::disconnected;
  }
  throw std::logic_error("Invalid admitted vehicle availability");
}
std::string_view key(VehicleSummaryState state) {
  switch (state) {
    case VehicleSummaryState::admitted:
      return "admitted";
    case VehicleSummaryState::awaiting_source:
      return "awaiting_source";
    case VehicleSummaryState::undated_source:
      return "undated_source";
    case VehicleSummaryState::invalid_snapshot:
      return "invalid_snapshot";
    case VehicleSummaryState::generation_mismatch:
      return "generation_mismatch";
    case VehicleSummaryState::disconnected:
      return "disconnected";
  }
  throw std::logic_error("Invalid vehicle summary state");
}
std::string_view key(VehicleSummaryAvailability state) {
  switch (state) {
    case VehicleSummaryAvailability::available:
      return "available";
    case VehicleSummaryAvailability::missing:
      return "missing";
    case VehicleSummaryAvailability::unavailable:
      return "unavailable";
    case VehicleSummaryAvailability::stale:
      return "stale";
    case VehicleSummaryAvailability::disconnected:
      return "disconnected";
  }
  throw std::logic_error("Invalid vehicle summary availability");
}
std::string_view key(VehicleObservationOrigin origin) {
  switch (origin) {
    case VehicleObservationOrigin::measured:
      return "measured";
    case VehicleObservationOrigin::calculated:
      return "calculated";
    case VehicleObservationOrigin::simulation:
      return "simulation";
  }
  throw std::logic_error("Invalid admitted vehicle origin");
}
void date(std::ostream& out, const std::optional<Clock::time_point>& value) {
  if (value) {
    out << '"'
        << std::chrono::duration_cast<std::chrono::nanoseconds>(value->time_since_epoch()).count()
        << '"';
  } else {
    out << "null";
  }
}
}  // namespace
PrephoneVehicleSummary prephone_vehicle_summary(const VehicleSample& raw,
                                                VehicleSummaryAdmission admission,
                                                Clock::time_point now) {
  PrephoneVehicleSummary result;
  for (std::size_t i = 0; i < definitions.size(); ++i) {
    const auto& definition = definitions[i];
    const auto& reading = reading_definitions[definition.reading];
    auto& row = result.rows[i];
    row.id = reading.key;
    row.group = definition.group;
    row.label = definition.label;
    row.unit = reading.unit;
  }
  if (!admission.generation) return result;
  if (now <= Clock::time_point{} || raw.sampled_at > now || !valid_vehicle_sample(raw)) {
    result.state = VehicleSummaryState::invalid_snapshot;
    return result;
  }
  if (!raw.observation_timing) {
    result.state = VehicleSummaryState::undated_source;
    return result;
  }
  if (raw.observation_timing->generation != admission.generation) {
    result.state = VehicleSummaryState::generation_mismatch;
    return result;
  }
  const auto current = current_vehicle_sample(raw, now);
  const auto& timing = *current.observation_timing;
  result.state =
      admission.retired ? VehicleSummaryState::disconnected : VehicleSummaryState::admitted;
  result.source_generation = timing.generation;
  result.source_started_at = timing.started_at;
  result.source_sampled_at = current.sampled_at;
  for (std::size_t i = 0; i < definitions.size(); ++i) {
    const auto index = definitions[i].reading;
    const auto& signal = timing.readings[index];
    auto& row = result.rows[i];
    row.acquired_at = signal.acquired_at;
    // No acquisition means no observed provenance. Keep the actual age policy.
    row.maximum_age = signal.maximum_age;
    if (signal.acquired_at) row.origin = signal.origin;
    row.availability =
        admission.retired ? VehicleSummaryAvailability::disconnected : availability(signal);
    if (row.availability == VehicleSummaryAvailability::available)
      row.value = current.*reading_definitions[index].member;
  }
  return result;
}
std::string prephone_vehicle_summary_json(const VehicleSample& sample,
                                          VehicleSummaryAdmission admission,
                                          Clock::time_point now) {
  const auto model = prephone_vehicle_summary(sample, admission, now);
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(std::numeric_limits<double>::max_digits10);
  out << "{\"version\":1,\"state\":\"" << key(model.state)
      << "\",\"clockDomain\":\"host-steady\",\"sourceGeneration\":";
  if (model.source_generation) {
    out << '"' << *model.source_generation << '"';
  } else {
    out << "null";
  }
  out << ",\"sourceStartedNs\":";
  date(out, model.source_started_at);
  out << ",\"sourceSampledNs\":";
  date(out, model.source_sampled_at);
  out << ",\"rows\":[";
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    const auto& row = model.rows[i];
    if (i) out << ',';
    // All strings are immutable first-party catalogue labels, never wire/user text.
    out << "{\"id\":\"" << row.id << "\",\"group\":\"" << row.group << "\",\"label\":\""
        << row.label << "\",\"unit\":\"" << row.unit << "\",\"availability\":\""
        << key(row.availability) << "\",\"value\":";
    if (row.value) {
      out << *row.value;
    } else {
      out << "null";
    }
    out << ",\"acquiredNs\":";
    date(out, row.acquired_at);
    out << ",\"maximumAgeNs\":";
    if (row.maximum_age) {
      out << '"' << std::chrono::duration_cast<std::chrono::nanoseconds>(*row.maximum_age).count()
          << '"';
    } else {
      out << "null";
    }
    out << ",\"origin\":";
    if (row.origin) {
      out << '"' << key(*row.origin) << '"';
    } else {
      out << "null";
    }
    out << '}';
  }
  out << "]}";
  auto encoded = out.str();
  if (encoded.size() > prephone_vehicle_summary_json_limit)
    throw std::length_error("Vehicle summary JSON bound exceeded");
  return encoded;
}
}  // namespace headunit
