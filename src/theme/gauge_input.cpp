// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gauge_input.hpp>
#include <headunit/vehicle_facts.hpp>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace headunit::theme::vehicle {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
std::int64_t binding(const LocalProfile& profile, const std::array<std::string, 3>& path) {
  std::optional<std::int64_t> found;
  for (const auto& item : profile.bindings)
    if (item.path == path) {
      check(!found && !item.instance, "ambiguous gauge input binding");
      found = item.iid;
    }
  check(found && profile.source->timed(*found), "missing timed gauge input binding");
  return *found;
}
std::uint64_t milliseconds(headunit::Clock::time_point value) {
  const auto n =
      std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
  check(n >= 0, "invalid gauge input timestamp");
  return std::uint64_t(n);
}
std::optional<opack::Value> numeric(const VehicleInputBinding& item, std::optional<double> raw) {
  if (raw) {
    const double n = *raw * item.scale + item.offset;
    if (std::isfinite(n) && (!item.minimum || n >= *item.minimum) &&
        (!item.maximum || n <= *item.maximum)) {
      if (item.kind == ValueKind::real) return opack::Value{n};
      if (std::trunc(n) == n && std::abs(n) <= 9007199254740991.)
        return opack::Value{std::int64_t(n)};
    }
  }
  return {};
}
std::optional<opack::Value> mapped(const VehicleInputBinding& item, const VehicleSample& sample) {
  if (const auto* reading = find_vehicle_reading(item.field)) {
    return numeric(item, sample.*reading->member);
  } else if (item.field == "trip.fuel_efficiency_l_per_100km") {
    return numeric(item, sample.trip ? sample.trip->fuel_efficiency_l_per_100km : std::nullopt);
  } else if (const auto* lamp = find_vehicle_indicator(item.field)) {
    if (const auto& state = sample.indicator(lamp->id); state)
      return item.states.at(static_cast<std::size_t>(*state));
  } else if (item.field == "drive_mode") {
    if (sample.drive_mode) {
      const auto mapping =
          item.semantic_states.find(std::string(drive_mode_identifier_key(*sample.drive_mode)));
      if (mapping != item.semantic_states.end()) return mapping->second;
    }
  } else if (sample.transmission) {
    const auto& transmission = *sample.transmission;
    std::optional<std::string> key;
    if (item.field == "transmission.selector" && transmission.selector)
      key = transmission_selector_key(*transmission.selector);
    else if (item.field == "transmission.gear" && transmission.engaged_gear)
      key = engaged_gear_key(*transmission.engaged_gear);
    else if (item.field == "transmission.gearbox_type" && transmission.gearbox_type)
      key = gearbox_type_key(*transmission.gearbox_type);
    if (key) {
      const auto mapping = item.semantic_states.find(*key);
      if (mapping != item.semantic_states.end()) return mapping->second;
    }
  }
  return {};
}
std::size_t signal_index(const VehicleInputBinding& item) {
  if (const auto* reading = find_vehicle_reading(item.field))
    return std::size_t(reading - reading_definitions.data());
  if (const auto* lamp = find_vehicle_indicator(item.field))
    return reading_definitions.size() + static_cast<std::size_t>(lamp->id);
  if (item.field == "transmission.selector" || item.field == "transmission.gear" ||
      item.field == "transmission.gearbox_type")
    return transmission_observation_index;
  if (item.field == "drive_mode") return drive_mode_observation_index;
  if (item.field == "trip.fuel_efficiency_l_per_100km") return trip_observation_index;
  throw std::invalid_argument("unknown vehicle input field");
}
bool has_observation(const VehicleSample& sample, std::size_t index) {
  return !std::holds_alternative<std::monostate>(vehicle_observation_value(sample, index));
}
}  // namespace
GaugeInput::GaugeInput(const LocalProfile& profile, std::unique_ptr<headunit::VehicleSource> input)
    : service_(profile.source), input_(std::move(input)), additional_(profile.vehicle_inputs) {
  check(service_ && input_, "gauge input needs a profile and source");
  validate_local_vehicle_inputs(profile);
  ids_ = {binding(profile, {"vehicleMotion", "displayedSpeed", "speed"}),
          binding(profile, {"vehicleMotion", "displayedSpeed", "speedKMH"}),
          binding(profile, {"vehicleMotion", "displayedSpeed", "speedMPH"}),
          binding(profile, {"internalCombustionEngine", "engineRPM", "rotationalSpeed"}),
          binding(profile, {"internalCombustionEngine", "engineRPM", "rotationalSpeedState"})};
  for (const auto& item : additional_) {
    check(std::find(ids_.begin(), ids_.end(), item.iid) == ids_.end() && service_->timed(item.iid),
          "conflicting or fixed vehicle input binding");
    if (item.field == "transmission.gearbox_type")
      check(std::none_of(profile.bindings.begin(), profile.bindings.end(),
                         [&](const auto& binding) {
                           return binding.iid == item.iid &&
                                  binding.path == std::array<std::string, 3>{"driveState",
                                                                             "transmissionStatus",
                                                                             "transmissionMode"};
                         }),
            "gearbox type cannot select the transmission mode");
  }
}
void GaugeInput::pump(headunit::Clock::time_point now) {
  check(!last_now_ || now >= *last_now_, "reversed gauge input clock");
  const auto sample = input_->sample(now);
  const auto at = milliseconds(sample.sampled_at), time = milliseconds(now);
  check(sample.sampled_at <= now && (!last_sample_ || at >= *last_sample_),
        "future or reversed gauge input");
  check(valid_vehicle_sample(sample), "invalid vehicle input value");
  const bool observed = sample.observation_timing.has_value();
  check(!observed_format_ || *observed_format_ == observed,
        "vehicle source changed timestamp format");
  // A socket with no first packet has neither observations nor a receipt date.
  // Do not select the legacy timestamp contract during this startup state.
  if (!observed_format_ && !observed && sample.sampled_at == headunit::Clock::time_point{} &&
      !sample.simulated &&
      std::none_of(reading_definitions.begin(), reading_definitions.end(),
                   [&](const auto& item) { return (sample.*item.member).has_value(); }) &&
      !sample.transmission && !sample.drive_mode && !sample.drive_mode_catalog && !sample.trip &&
      !sample.heading &&
      std::none_of(sample.indicators.begin(), sample.indicators.end(),
                   [](const auto& item) { return item.has_value(); })) {
    service_->publish_batch({}, time);
    last_now_ = now;
    sample_ = sample;
    return;
  }
  const auto current = current_vehicle_sample(sample, now);
  const auto publish_facts = [&] {
    // Publish phone-policy facts only after source and service admission. A
    // rejected snapshot must preserve both prior values and their lease date.
    VehicleFacts facts;
    if (const auto lamp = current.indicators[static_cast<std::size_t>(Indicator::low_beam)])
      facts.low_beam = *lamp != IndicatorState::off;
    facts.speed_kmh = current.speed_kmh;
    facts.simulated = current.simulated;
    facts.observed_at = now;
    publish_vehicle_facts(facts);
  };
  auto trip_progress = trip_progress_;
  if (observed && sample_.observation_timing &&
      sample.observation_timing->generation != sample_.observation_timing->generation)
    check(withdraw_trip_progress(trip_progress), "invalid trip history on source restart");
  if (sample.trip) {
    const auto acquired =
        observed ? sample.observation_timing->trip.acquired_at : std::optional{sample.sampled_at};
    // A provider may repeatedly return the exact original snapshot after it
    // expires. This is retained historical data, not a new observation. Do
    // not revive it or feed it back through a sticky withdrawn watermark.
    const bool historical = !current.trip && acquired == trip_progress_.acquired_at &&
                            sample.trip == trip_progress_.last_observation;
    check(acquired && (historical || update_trip_progress(trip_progress, *sample.trip, *acquired)),
          "invalid lifetime trip transition");
  }
  if (!current.trip) check(withdraw_trip_progress(trip_progress), "invalid withdrawn trip history");
  check(!mode_catalog_selected_ || sample.drive_mode_catalog == mode_catalog_,
        "vehicle drive-mode catalogue changed");
  check(!observed || !last_sample_ || sample.sampled_at >= sample_.sampled_at,
        "reversed vehicle publication time");
  if (observed) {
    pump_observed(sample, current, now);
    trip_progress_ = trip_progress;
    publish_facts();
    return;
  }
  if (last_sample_ == at) {
    std::vector<Publication> withdrawals;
    if (!current.trip)
      for (const auto& item : additional_)
        if (item.field == "trip.fuel_efficiency_l_per_100km")
          withdrawals.push_back({item.iid, {}, {}});
    service_->publish_batch(std::move(withdrawals), time);
    last_now_ = now;
    trip_progress_ = trip_progress;
    publish_facts();
    return;
  }
  // All components are validated before any publication. Do not stamp an old
  // reading with 'time': the profile, including its own expiry, stays authoritative.
  std::vector<Publication> publications;
  publications.reserve(ids_.size() + additional_.size());
  const auto publish = [&](unsigned index, std::optional<double> n) {
    auto& publication = publications.emplace_back(ids_[index], std::nullopt, at);
    if (n) publication.value.emplace().data = *n;
  };
  publish(0, sample.speed_kmh ? std::optional{*sample.speed_kmh * 1000.} : std::nullopt);
  publish(1, sample.speed_kmh);
  publish(2, sample.speed_kmh ? std::optional{*sample.speed_kmh / 1.609344} : std::nullopt);
  publish(3, sample.engine_rpm);
  auto& rpm_state = publications.emplace_back(ids_[4], std::nullopt, at);
  if (sample.engine_rpm) rpm_state.value.emplace().data = std::int64_t{0};
  for (const auto& item : additional_) {
    publications.push_back(
        {item.iid,
         mapped(item, item.field == "trip.fuel_efficiency_l_per_100km" ? current : sample), at});
  }
  service_->publish_batch(std::move(publications), time);
  last_sample_ = at;
  last_now_ = now;
  observed_format_ = false;
  if (!mode_catalog_selected_) {
    mode_catalog_ = sample.drive_mode_catalog;
    mode_catalog_selected_ = true;
  }
  sample_ = sample;
  trip_progress_ = trip_progress;
  publish_facts();
}

void GaugeInput::pump_observed(const VehicleSample& sample, const VehicleSample& current,
                               headunit::Clock::time_point now) {
  const auto& meta = *sample.observation_timing;
  const auto* previous = sample_.observation_timing ? &*sample_.observation_timing : nullptr;
  const bool new_generation = !previous || meta.generation != previous->generation;
  if (previous) {
    check(meta.generation >= previous->generation, "replayed vehicle source generation");
    if (new_generation)
      check(meta.started_at >= sample_.sampled_at,
            "vehicle generation starts before previous source publication");
    else
      check(meta.started_at == previous->started_at, "vehicle generation start changed");
    for (std::size_t i = 0; i < observations_.size(); ++i)
      check(vehicle_signal_timing(meta, i).maximum_age ==
                vehicle_signal_timing(*previous, i).maximum_age,
            "vehicle age policy changed");
  }
  auto observations = new_generation ? decltype(observations_){} : observations_;
  auto delivered = delivered_ms_;
  std::array<bool, vehicle_observation_capacity> changed{};
  for (std::size_t i = 0; i < observations.size(); ++i) {
    auto& watermark = observations[i];
    const auto& item = vehicle_signal_timing(meta, i);
    const auto raw = vehicle_observation_value(sample, i);
    check(!item.acquired_at || *item.acquired_at <= now, "future vehicle acquisition");
    check(
        !watermark.acquired_at || (item.acquired_at && *item.acquired_at >= *watermark.acquired_at),
        "reversed or removed vehicle acquisition");
    const bool newer = item.acquired_at && item.acquired_at != watermark.acquired_at;
    const bool available = item.availability == VehicleAvailability::available;
    const bool unavailable = item.availability == VehicleAvailability::unavailable;
    if (newer) {
      watermark.acquired_at = item.acquired_at;
      watermark.origin = item.origin;
      watermark.original_value = raw;
      watermark.original_available =
          available || unavailable ? std::optional{available} : std::nullopt;
      watermark.withdrawn = false;
      changed[i] = true;
    } else if (item.acquired_at) {
      check(item.origin == watermark.origin, "same-time vehicle origin changed");
      if (available || unavailable) {
        check(!available || !watermark.withdrawn || !has_observation(current, i),
              "same-time observation revived a withdrawn source");
        if (watermark.original_available) {
          check(*watermark.original_available == available,
                "conflicting same-time vehicle availability");
          check(!available || raw == watermark.original_value,
                "conflicting same-time vehicle value");
        } else
          watermark.withdrawn = true;  // First sight was already stale/lost.
      }
    }
    const bool lost = !item.acquired_at || (!available && !unavailable) ||
                      (available && !has_observation(current, i));
    if (lost && !watermark.withdrawn) {
      watermark.withdrawn = true;
      changed[i] = true;
    }
    if (new_generation) changed[i] = true;  // Clear any old-generation service value.
  }
  std::vector<Publication> publications;
  publications.reserve(ids_.size() + additional_.size());
  const auto publish = [&](std::int64_t id, std::size_t signal, std::optional<opack::Value> value) {
    if (!changed[signal]) return;
    const auto& state = observations[signal];
    if (state.withdrawn || !state.acquired_at) {
      publications.push_back({id, {}, {}});
      return;
    }
    const auto at = milliseconds(*state.acquired_at);
    const auto prior = delivered.find(id);
    // The service clock is milliseconds. Two acquisitions in the same bin
    // cannot become two ordered publications: withdraw instead of fabricating
    // a later date, then recover on the next distinct acquired millisecond.
    if (value && prior != delivered.end() && at <= prior->second) {
      publications.push_back({id, {}, {}});
      return;
    }
    publications.push_back({id, std::move(value), at});
    delivered[id] = at;
  };
  const auto speed = current.speed_kmh, rpm = current.engine_rpm;
  publish(ids_[0], 0, speed ? std::optional{opack::Value{*speed * 1000.}} : std::nullopt);
  publish(ids_[1], 0, speed ? std::optional{opack::Value{*speed}} : std::nullopt);
  publish(ids_[2], 0, speed ? std::optional{opack::Value{*speed / 1.609344}} : std::nullopt);
  publish(ids_[3], 1, rpm ? std::optional{opack::Value{*rpm}} : std::nullopt);
  publish(ids_[4], 1, rpm ? std::optional{opack::Value{std::int64_t{0}}} : std::nullopt);
  for (const auto& item : additional_)
    publish(item.iid, signal_index(item), mapped(item, current));
  service_->publish_batch(std::move(publications), milliseconds(now));
  observations_ = std::move(observations);
  delivered_ms_ = std::move(delivered);
  sample_ = sample;
  last_sample_ = milliseconds(sample.sampled_at);
  last_now_ = now;
  observed_format_ = true;
  if (!mode_catalog_selected_) {
    mode_catalog_ = sample.drive_mode_catalog;
    mode_catalog_selected_ = true;
  }
}
}  // namespace headunit::theme::vehicle
