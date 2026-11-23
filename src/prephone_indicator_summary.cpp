// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_indicator_summary.hpp>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace headunit {
namespace {
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
  throw std::logic_error("Invalid indicator summary state");
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
  throw std::logic_error("Invalid indicator summary availability");
}
std::string_view key(IndicatorState state) {
  switch (state) {
    case IndicatorState::off:
      return "OFF";
    case IndicatorState::on:
      return "ON";
    case IndicatorState::flashing:
      return "FLASHING";
  }
  throw std::logic_error("Invalid admitted indicator state");
}
std::string_view key(IndicatorColor color) {
  switch (color) {
    case IndicatorColor::green:
      return "green";
    case IndicatorColor::blue:
      return "blue";
    case IndicatorColor::amber:
      return "amber";
    case IndicatorColor::red:
      return "red";
  }
  throw std::logic_error("Invalid catalogue indicator color");
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
  throw std::logic_error("Invalid admitted indicator origin");
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
void duration(std::ostream& out, const std::optional<Clock::duration>& value) {
  if (value) {
    out << '"' << std::chrono::duration_cast<std::chrono::nanoseconds>(*value).count() << '"';
  } else {
    out << "null";
  }
}
VehicleSummaryAvailability availability(const VehicleSignalTiming& timing) {
  switch (timing.availability) {
    case VehicleAvailability::available:
      return VehicleSummaryAvailability::available;
    case VehicleAvailability::unavailable:
      return timing.acquired_at ? VehicleSummaryAvailability::unavailable
                                : VehicleSummaryAvailability::missing;
    case VehicleAvailability::stale:
      return VehicleSummaryAvailability::stale;
    case VehicleAvailability::disconnected:
      return VehicleSummaryAvailability::disconnected;
  }
  throw std::logic_error("Invalid admitted indicator availability");
}
}  // namespace
PrephoneIndicatorSummary prephone_indicator_summary(const VehicleSample& raw,
                                                    PrephoneIndicatorAdmission admission,
                                                    Clock::time_point now) {
  static_assert(indicator_definitions.size() == prephone_indicator_summary_row_limit);
  PrephoneIndicatorSummary result;
  for (std::size_t i = 0; i < result.rows.size(); ++i) {
    const auto& definition = indicator_definitions[i];
    auto& row = result.rows[i];
    row.indicator = definition.id;
    row.id = definition.key;
    row.label = definition.label;
    row.color = definition.color;
  }
  if (now > Clock::time_point{}) result.rendered_at = now;
  if (admission.clock_domain != PrephoneIndicatorClockDomain::host_steady) {
    result.state = VehicleSummaryState::invalid_snapshot;
    return result;
  }
  result.clock_domain = admission.clock_domain;
  // Reuse the established admission/metadata gate unchanged. It delegates to
  // valid_vehicle_sample/current_vehicle_sample; no parallel validator or cache.
  const auto admitted = prephone_vehicle_summary(raw, admission.source, now);
  result.state = admitted.state;
  result.source_generation = admitted.source_generation;
  result.source_started_at = admitted.source_started_at;
  result.source_sampled_at = admitted.source_sampled_at;
  if (!admitted.source_generation) return result;
  const auto current = current_vehicle_sample(raw, now);
  for (std::size_t i = 0; i < result.rows.size(); ++i) {
    const auto index = static_cast<std::size_t>(result.rows[i].indicator);
    const auto& timing = current.observation_timing->indicators[index];
    auto& row = result.rows[i];
    row.acquired_at = timing.acquired_at;
    row.maximum_age = timing.maximum_age;
    if (timing.acquired_at) {
      row.origin = timing.origin;
      row.age = now - *timing.acquired_at;
    }
    row.availability =
        admission.source.retired ? VehicleSummaryAvailability::disconnected : availability(timing);
    if (row.availability == VehicleSummaryAvailability::available)
      row.value = current.indicators[index];
  }
  return result;
}
std::string prephone_indicator_summary_json(const VehicleSample& raw,
                                            PrephoneIndicatorAdmission admission,
                                            Clock::time_point now) {
  const auto model = prephone_indicator_summary(raw, admission, now);
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\"version\":" << prephone_indicator_summary_json_version << ",\"state\":\""
      << key(model.state) << "\",\"clockDomain\":";
  if (model.clock_domain == PrephoneIndicatorClockDomain::host_steady) {
    out << "\"host-steady\"";
  } else {
    out << "null";
  }
  out << ",\"sourceGeneration\":";
  if (model.source_generation) {
    out << '"' << *model.source_generation << '"';
  } else {
    out << "null";
  }
  out << ",\"sourceStartedNs\":";
  date(out, model.source_started_at);
  out << ",\"sourceSampledNs\":";
  date(out, model.source_sampled_at);
  out << ",\"renderedNs\":";
  date(out, model.rendered_at);
  out << ",\"rows\":[";
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    const auto& row = model.rows[i];
    if (i) out << ',';
    out << "{\"id\":\"" << row.id << "\",\"label\":\"" << row.label << "\",\"color\":\""
        << key(row.color) << "\",\"availability\":\"" << key(row.availability) << "\",\"value\":";
    if (row.value) {
      out << '"' << key(*row.value) << '"';
    } else {
      out << "null";
    }
    out << ",\"acquiredNs\":";
    date(out, row.acquired_at);
    out << ",\"maximumAgeNs\":";
    duration(out, row.maximum_age);
    out << ",\"ageNs\":";
    duration(out, row.age);
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
  if (encoded.size() > prephone_indicator_summary_json_limit)
    throw std::length_error("Indicator summary JSON bound exceeded");
  return encoded;
}
}  // namespace headunit
