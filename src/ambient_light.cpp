// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/ambient_light.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace headunit::ambient {
namespace {
void require(bool valid, const char* message) {
  if (!valid) throw std::invalid_argument(message);
}
bool valid_timing(const VehicleSignalTiming& timing, Clock::time_point started,
                  Clock::time_point now) {
  return timing.availability == VehicleAvailability::available && timing.acquired_at &&
         *timing.acquired_at >= started && *timing.acquired_at <= now &&
         timing.maximum_age > Clock::duration::zero() &&
         timing.maximum_age <= std::chrono::seconds(3) &&
         now - *timing.acquired_at < timing.maximum_age &&
         (timing.origin == VehicleObservationOrigin::measured ||
          timing.origin == VehicleObservationOrigin::calculated ||
          timing.origin == VehicleObservationOrigin::simulation);
}
bool active(vehicle_control::Status status) {
  return status == vehicle_control::Status::accepted || status == vehicle_control::Status::pending;
}
void value_json(std::ostream& out, const Value& value) {
  out << "{\"rgbw\":[";
  for (std::size_t i = 0; i < value.rgbw.size(); ++i) {
    if (i) out << ',';
    out << unsigned(value.rgbw[i]);
  }
  out << "],\"brightness_permille\":" << value.brightness_permille
      << ",\"transition_ms\":" << value.transition_ms << '}';
}
const char* origin_name(VehicleObservationOrigin origin) {
  if (origin == VehicleObservationOrigin::simulation) return "simulation";
  if (origin == VehicleObservationOrigin::calculated) return "calculated";
  return "measured";
}
void evidence_json(std::ostream& out, const std::optional<Evidence>& evidence) {
  if (!evidence) {
    out << "null";
    return;
  }
  out << "{\"generation\":\"" << evidence->generation << "\",\"acquired_ns\":\""
      << std::chrono::duration_cast<std::chrono::nanoseconds>(
             evidence->timing.acquired_at->time_since_epoch())
             .count()
      << "\",\"max_age_ns\":\""
      << std::chrono::duration_cast<std::chrono::nanoseconds>(evidence->timing.maximum_age).count()
      << "\",\"origin\":\"" << origin_name(evidence->timing.origin) << "\"}";
}
}  // namespace
bool valid_settings(const Settings& settings) noexcept {
  if (settings.selection != Selection::automatic && settings.selection != Selection::screen &&
      settings.selection != Selection::drive_mode && settings.selection != Selection::fixed)
    return false;
  if (settings.day_brightness_permille > 1000 ||
      settings.night_brightness_permille > settings.day_brightness_permille ||
      settings.transition_ms > vehicle_control::maximum_ambient_transition_ms ||
      settings.mode_count > settings.mode_colours.size())
    return false;
  const auto& night = settings.night_policy;
  if ((night.kind != NightModePolicy::Kind::off && night.kind != NightModePolicy::Kind::on &&
       night.kind != NightModePolicy::Kind::schedule &&
       night.kind != NightModePolicy::Kind::headlights) ||
      night.start_minute >= 1440 || night.end_minute >= 1440 ||
      night.start_minute == night.end_minute)
    return false;
  for (std::size_t i = 0; i < settings.mode_count; ++i) {
    if (!valid_drive_mode_identifier(settings.mode_colours[i].mode)) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (settings.mode_colours[j].mode == settings.mode_colours[i].mode) return false;
  }
  return true;
}
std::optional<Target> derive(const Settings& settings, const Inputs& inputs,
                             Clock::time_point now) {
  require(valid_settings(settings), "invalid ambient settings");
  require(now > Clock::time_point{}, "invalid ambient operation time");
  Target target;
  target.derived_at = now;
  target.valid_until = now + std::chrono::seconds(1);
  target.value.transition_ms = static_cast<std::uint16_t>(settings.transition_ms);
  if (!settings.enabled) return target;  // Explicit user Off; not a missing sensor.
  const auto sample = current_vehicle_sample(inputs.vehicle, now);
  std::optional<bool> low_beam;
  if (sample.observation_timing) {
    const auto& metadata = *sample.observation_timing;
    const auto& timing = metadata.indicators[static_cast<std::size_t>(Indicator::low_beam)];
    if (sample.indicator(Indicator::low_beam) && valid_timing(timing, metadata.started_at, now)) {
      low_beam = *sample.indicator(Indicator::low_beam) != IndicatorState::off;
      target.headlights_observation = Evidence{metadata.generation, timing};
      target.valid_until = std::min(target.valid_until, *timing.acquired_at + timing.maximum_age);
    }
  }
  const auto kind = settings.night_policy.kind;
  if (kind == NightModePolicy::Kind::headlights && low_beam)
    target.night_source = NightSource::headlights;
  else if (kind == NightModePolicy::Kind::headlights || kind == NightModePolicy::Kind::schedule) {
    if (!inputs.local_minute || *inputs.local_minute >= 1440) return {};
    target.night_source = NightSource::schedule;
    target.headlights_observation.reset();
  } else {
    target.headlights_observation.reset();
  }
  target.night = night_mode_at(settings.night_policy, inputs.local_minute.value_or(0), low_beam);
  target.value.brightness_permille = static_cast<std::uint16_t>(
      target.night ? settings.night_brightness_permille : settings.day_brightness_permille);
  if (settings.selection == Selection::fixed) {
    target.source = Source::fixed;
    target.value.rgbw = settings.fixed_rgbw;
    return target;
  }
  if (settings.selection == Selection::automatic || settings.selection == Selection::screen) {
    if (inputs.screen && inputs.screen->generation &&
        inputs.screen->started_at > Clock::time_point{} &&
        valid_timing(inputs.screen->timing, inputs.screen->started_at, now)) {
      target.source = Source::screen;
      target.value.rgbw = inputs.screen->rgbw;
      target.colour_observation = Evidence{inputs.screen->generation, inputs.screen->timing};
      target.valid_until = std::min(target.valid_until, *inputs.screen->timing.acquired_at +
                                                            inputs.screen->timing.maximum_age);
      return target;
    }
    if (settings.selection == Selection::screen) return {};
  }
  if (!sample.drive_mode || !sample.observation_timing) return {};
  const auto& metadata = *sample.observation_timing;
  if (!valid_timing(metadata.drive_mode, metadata.started_at, now)) return {};
  for (std::size_t i = 0; i < settings.mode_count; ++i) {
    if (settings.mode_colours[i].mode == *sample.drive_mode) {
      target.source = Source::drive_mode;
      target.value.rgbw = settings.mode_colours[i].rgbw;
      target.colour_observation = Evidence{metadata.generation, metadata.drive_mode};
      target.valid_until = std::min(
          target.valid_until, *metadata.drive_mode.acquired_at + metadata.drive_mode.maximum_age);
      return target;
    }
  }
  return {};
}
Controller::Controller(Settings settings, vehicle_control::TargetPolicy policy)
    : settings_(settings), policy_(policy), coordinator_(std::span(&policy_, 1)) {
  require(valid_settings(settings_), "invalid ambient settings");
  require(policy.kind == vehicle_control::ValueKind::ambient_light, "ambient target kind required");
}
void Controller::begin_generation(std::uint64_t generation, Clock::time_point started,
                                  Clock::time_point now) {
  coordinator_.begin_generation(generation, started, now);
  generation_ = generation;
  connected_ = true;
  issued_.reset();
  target_.reset();
}
void Controller::retire(Clock::time_point now) {
  coordinator_.disconnect(now);
  connected_ = false;
  issued_.reset();
  target_.reset();
}
void Controller::settings(Settings settings) {
  require(valid_settings(settings), "invalid ambient settings");
  settings_ = settings;
}
std::optional<vehicle_control::Request> Controller::update(const Inputs& inputs,
                                                           Clock::time_point now, bool permitted) {
  coordinator_.poll(now);
  auto filtered = inputs;
  if (!filtered.screen || !filtered.screen->generation ||
      filtered.screen->started_at <= Clock::time_point{} ||
      !valid_timing(filtered.screen->timing, filtered.screen->started_at, now)) {
    filtered.screen.reset();
    screen_withdrawn_ = true;
  } else {
    const auto& incoming = *filtered.screen;
    bool admitted = true;
    if (screen_frontier_) {
      const auto& previous = *screen_frontier_;
      if (incoming.generation < previous.generation)
        admitted = false;
      else if (incoming.generation == previous.generation) {
        require(incoming.started_at == previous.started_at &&
                    incoming.timing.maximum_age == previous.timing.maximum_age,
                "ambient screen generation policy changed");
        if (incoming.timing.acquired_at < previous.timing.acquired_at)
          admitted = false;
        else if (incoming.timing.acquired_at == previous.timing.acquired_at) {
          require(
              incoming.rgbw == previous.rgbw && incoming.timing.origin == previous.timing.origin,
              "ambient screen equal-date conflict");
          admitted = !screen_withdrawn_;
        }
      } else {
        admitted = incoming.started_at >= *previous.timing.acquired_at;
      }
    }
    if (admitted) {
      screen_frontier_ = incoming;
      screen_withdrawn_ = false;
    } else
      filtered.screen.reset();
  }
  const auto target = derive(settings_, filtered, now);
  target_ = target;
  if (!connected_ || !target) return {};
  if (const auto previous = command(now); previous && active(previous->status)) return {};
  if (issued_ && *issued_ == target->value) return {};
  require(identifier_ < std::numeric_limits<std::uint64_t>::max(),
          "ambient command identifiers exhausted");
  // A ten-second operation bound accommodates the maximum five-second MCU fade.
  const vehicle_control::Request request{
      identifier_ + 1, generation_, policy_.key,
      target->value,   now,         now + std::chrono::seconds(10)};
  const auto result = coordinator_.submit(request, now);
  ++identifier_;
  issued_ = target->value;
  if (result.status != vehicle_control::Status::accepted) return {};
  return coordinator_.dispatch(identifier_, now, permitted);
}
vehicle_control::Snapshot Controller::acknowledge(const vehicle_control::Acknowledgement& ack,
                                                  Clock::time_point now) {
  return coordinator_.acknowledge(ack, now);
}
void Controller::observe(const vehicle_control::ConfirmedState& state, Clock::time_point now) {
  coordinator_.observe(state, now);
}
std::optional<vehicle_control::ConfirmedState> Controller::current(Clock::time_point now) {
  return coordinator_.read(policy_.key, now);
}
std::optional<vehicle_control::Snapshot> Controller::command(Clock::time_point now) {
  return identifier_ ? coordinator_.snapshot(identifier_, now) : std::nullopt;
}
Value interpolate(const Value& from, const Value& to, double fraction) {
  require(vehicle_control::valid_ambient_light_value(from) &&
              vehicle_control::valid_ambient_light_value(to) && std::isfinite(fraction) &&
              fraction >= 0 && fraction <= 1,
          "invalid ambient fade inputs");
  const double weight = fraction * fraction * (3 - 2 * fraction);
  Value result;
  // Interpolate emitted intensity, avoiding a colour/intensity cross-product dip.
  result.brightness_permille = 1000;
  for (std::size_t i = 0; i < result.rgbw.size(); ++i) {
    const auto start = double(from.rgbw[i]) * from.brightness_permille / 1000;
    const auto end = double(to.rgbw[i]) * to.brightness_permille / 1000;
    result.rgbw[i] = static_cast<std::uint8_t>(std::lround(start + (end - start) * weight));
  }
  if (fraction == 0) {
    result = from;
    result.transition_ms = 0;
  }
  if (fraction == 1) {
    result = to;
    result.transition_ms = 0;
  }
  return result;
}
std::string snapshot_json(Controller& controller, Clock::time_point now) {
  const auto actual = controller.current(now);
  const auto command = controller.command(now);
  const auto& target = controller.requested();
  std::ostringstream out;
  out << "{\"version\":1,\"sampled_ns\":\""
      << std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()
      << "\",\"settings\":{\"enabled\":" << (controller.settings().enabled ? "true" : "false")
      << ",\"selection\":\""
      << std::array{"automatic", "screen", "drive_mode",
                    "fixed"}[static_cast<unsigned>(controller.settings().selection)]
      << "\",\"day_brightness_permille\":" << controller.settings().day_brightness_permille
      << ",\"night_brightness_permille\":" << controller.settings().night_brightness_permille
      << ",\"transition_ms\":" << controller.settings().transition_ms << "},\"requested\":";
  // A stored derived target is revalidated at every update; snapshots never
  // imply that it reached hardware. Data owners call update before publication.
  if (target && now < target->valid_until) {
    out << "{\"derived_ns\":\""
        << std::chrono::duration_cast<std::chrono::nanoseconds>(
               target->derived_at.time_since_epoch())
               .count()
        << "\",\"valid_until_ns\":\""
        << std::chrono::duration_cast<std::chrono::nanoseconds>(
               target->valid_until.time_since_epoch())
               .count()
        << "\",\"value\":";
    value_json(out, target->value);
    out << ",\"source\":\""
        << std::array{"screen", "drive_mode", "fixed",
                      "disabled"}[static_cast<unsigned>(target->source)]
        << "\",\"night\":" << (target->night ? "true" : "false") << ",\"night_source\":\""
        << std::array{"policy", "headlights",
                      "schedule"}[static_cast<unsigned>(target->night_source)]
        << "\",\"colour_observation\":";
    evidence_json(out, target->colour_observation);
    out << ",\"headlights_observation\":";
    evidence_json(out, target->headlights_observation);
    out << '}';
  } else
    out << "null";
  out << ",\"current\":";
  if (actual) {
    out << "{\"value\":";
    value_json(out, std::get<Value>(actual->value));
    out << ",\"generation\":\"" << actual->provider_generation << "\",\"confirmed_ns\":\""
        << std::chrono::duration_cast<std::chrono::nanoseconds>(
               actual->confirmed_at.time_since_epoch())
               .count()
        << "\",\"max_age_ns\":\""
        << std::chrono::duration_cast<std::chrono::nanoseconds>(controller.maximum_age()).count()
        << "\",\"origin\":\""
        << (actual->origin == vehicle_control::Origin::simulation ? "simulation"
            : actual->origin == vehicle_control::Origin::measured ? "measured"
                                                                  : "calculated")
        << "\"}";
  } else
    out << "null";
  out << ",\"command\":";
  if (command) {
    out << "{\"identifier\":\"" << command->request.identifier << "\",\"generation\":\""
        << command->request.provider_generation << "\",\"status\":\""
        << std::array{"accepted", "pending", "applied", "rejected", "failed", "timed_out"}
               [static_cast<unsigned>(command->status)]
        << "\",\"reason\":\""
        << std::array{"none",          "not_writable",      "busy",
                      "interlock",     "provider_rejected", "provider_failed",
                      "provider_lost", "deadline"}[static_cast<unsigned>(command->reason)]
        << "\",\"value\":";
    value_json(out, std::get<Value>(command->request.requested));
    out << '}';
  } else
    out << "null";
  out << '}';
  return out.str();
}
}  // namespace headunit::ambient
