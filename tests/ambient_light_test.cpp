// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/ambient_light.hpp>
#include <headunit/vehicle_command_configuration.hpp>
#include <headunit/vehicle_observation.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace headunit;
using namespace std::chrono_literals;
namespace control = vehicle_control;
namespace light = ambient;
#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) throw std::runtime_error("ambient check: " #condition); \
  } while (false)
void rejects(const auto& operation) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid ambient operation accepted");
}
const auto start = Clock::time_point{100s};
control::TargetPolicy policy(bool writable = true) {
  control::TargetPolicy result;
  result.key = *control::parse_key("cabin.ambient.rgbw");
  result.kind = control::ValueKind::ambient_light;
  result.writable = writable;
  result.maximum_age = 2s;
  return result;
}
light::Settings settings() {
  light::Settings result;
  result.enabled = true;
  result.mode_colours[0] = {*parse_drive_mode_identifier("comfort"), {0, 20, 255, 0}};
  result.mode_colours[1] = {*parse_drive_mode_identifier("sport"), {255, 0, 10, 0}};
  result.mode_count = 2;
  return result;
}
light::Inputs inputs(Clock::time_point now = start) {
  light::Inputs result;
  result.local_minute = 12 * 60;
  result.screen = light::Accent{
      {10, 80, 200, 0},
      1,
      start,
      {now, 500ms, VehicleObservationOrigin::calculated, VehicleAvailability::available}};
  VehicleAgePolicy ages;
  const auto catalog = parse_drive_mode_catalog_key("comfort,sport,eco");
  VehicleObservationCache cache(ages, catalog);
  cache.begin_generation(1, start, now);
  const std::array observations{
      VehicleObservation{"drive_mode", *parse_drive_mode_identifier("sport"), now,
                         VehicleObservationOrigin::simulation},
      VehicleObservation{"low_beam", IndicatorState::off, now,
                         VehicleObservationOrigin::simulation}};
  cache.accept(1, 1, observations, now);
  result.vehicle = cache.sample(now);
  return result;
}
control::Acknowledgement applied(const control::Request& request, Clock::time_point now,
                                 light::Value actual) {
  actual.transition_ms = 0;
  return {request.identifier,
          request.provider_generation,
          1,
          request.target,
          control::ReplyStatus::applied,
          now,
          control::ConfirmedState{request.provider_generation, request.target, actual, now,
                                  control::Origin::simulation}};
}
std::string read(std::string_view name) {
  std::ifstream file(std::string(HEADUNIT_SOURCE_DIR) + "/config/" + std::string(name));
  CHECK(file.good());
  return {std::istreambuf_iterator<char>(file), {}};
}
void configuration_and_wire() {
  auto config = read("ambient-light.example.json");
  const auto parsed = light::parse_settings(config);
  CHECK(!parsed.enabled && parsed.mode_count == 3);
  CHECK(parsed.night_policy.kind == NightModePolicy::Kind::headlights);
  rejects([&] { light::parse_settings(config + "{}"); });
  rejects([&] { light::parse_settings(std::string(16385, ' ')); });
  auto invalid = config;
  invalid.replace(invalid.find("\"enabled\": false"), 16, "\"enabled\": 1");
  rejects([&] { light::parse_settings(invalid); });
  invalid = config;
  invalid.replace(invalid.find("\"eco\""), 5, "\"sport\"");
  rejects([&] { light::parse_settings(invalid); });
  invalid = config;
  invalid.replace(invalid.find("800"), 3, "5001");
  rejects([&] { light::parse_settings(invalid); });
  invalid = config;
  invalid.replace(invalid.find("\"version\": 1"), 12, "\"version\": 1, \"version\": 1");
  rejects([&] { light::parse_settings(invalid); });
  const auto policies = control::parse_configuration(read("ambient-controls.example.json"));
  CHECK(policies.size() == 1 && policies[0].kind == control::ValueKind::ambient_light &&
        !policies[0].writable);
  rejects([&] {
    control::parse_configuration(
        R"({"schemaVersion":1,"targets":[{"key":"cabin.ambient.rgbw","kind":"ambient_light","max_age_ms":2000}]})");
  });
  light::Value value{{255, 1, 127, 8}, 1000, 5000};
  const auto encoded = control::encode_ambient_light_value(value);
  CHECK((encoded == std::array<std::uint8_t, 9>{1, 255, 1, 127, 8, 3, 232, 19, 136}));
  CHECK(control::decode_ambient_light_value(encoded) == value);
  CHECK(!control::decode_ambient_light_value(std::span(encoded).first(8)));
  auto bad = encoded;
  bad[0] = 2;
  CHECK(!control::decode_ambient_light_value(bad));
  bad = encoded;
  bad[5] = 4;
  CHECK(!control::decode_ambient_light_value(bad));
  bad = encoded;
  bad[7] = 20;
  CHECK(!control::decode_ambient_light_value(bad));
  CHECK(control::valid_value(policy(), value, true));
  CHECK(!control::valid_value(policy(), value, false));
  CHECK(!control::valid_value(policy(), true, true));
  value.transition_ms = 0;
  CHECK(control::valid_value(policy(), value, false));
}
void source_and_night() {
  auto config = settings();
  auto data = inputs();
  auto target = light::derive(config, data, start);
  CHECK(target && target->source == light::Source::screen &&
        target->value.rgbw == data.screen->rgbw);
  CHECK(!target->night && target->night_source == light::NightSource::headlights);
  CHECK(target->colour_observation->timing.acquired_at == start);
  data.screen.reset();
  target = light::derive(config, data, start);
  CHECK(target && target->source == light::Source::drive_mode);
  CHECK(target->colour_observation->timing.origin == VehicleObservationOrigin::simulation);
  CHECK(target->value.rgbw == config.mode_colours[1].rgbw);
  data.vehicle.indicator(Indicator::low_beam) = IndicatorState::on;
  target = light::derive(config, data, start);
  CHECK(target && target->night && target->value.brightness_permille == 60);
  CHECK(!light::derive(config, data, start + 500ms));
  data = inputs();
  data.screen->timing.acquired_at = start + 1ms;
  CHECK(light::derive(config, data, start)->source == light::Source::drive_mode);
  config.selection = light::Selection::screen;
  CHECK(!light::derive(config, data, start));
  data = inputs();
  data.vehicle = {};
  data.local_minute = 21 * 60;
  target = light::derive(config, data, start);
  CHECK(target && target->night && target->night_source == light::NightSource::schedule);
  CHECK(!target->headlights_observation);
  data.local_minute.reset();
  CHECK(!light::derive(config, data, start));
  config.night_policy.kind = NightModePolicy::Kind::off;
  CHECK(!light::derive(config, data, start)->night);
  config.enabled = false;
  target = light::derive(config, {}, start);
  CHECK(target && target->source == light::Source::disabled &&
        target->value.brightness_permille == 0);
  config = settings();
  config.selection = light::Selection::fixed;
  data = inputs();
  target = light::derive(config, data, start);
  CHECK(target && target->source == light::Source::fixed);
  config.night_brightness_permille = 301;
  rejects([&] { light::derive(config, data, start); });
}
void command_lifecycle() {
  light::Controller controller(settings(), policy());
  auto data = inputs();
  CHECK(!controller.update(data, start, true));
  controller.begin_generation(1, start, start);
  const auto request = controller.update(data, start, true);
  CHECK(request && controller.command(start)->status == control::Status::pending);
  CHECK(!controller.current(start));
  CHECK(!controller.update(data, start + 1ms, true));
  data.screen->rgbw = {255, 0, 0, 0};
  data.screen->timing.acquired_at = start + 2ms;
  CHECK(!controller.update(data, start + 2ms, true));
  auto actual = std::get<light::Value>(request->requested);
  actual.brightness_permille = 250;  // Actual MCU clamp survives; request stays 300.
  CHECK(controller.acknowledge(applied(*request, start + 3ms, actual), start + 3ms).status ==
        control::Status::applied);
  CHECK(std::get<light::Value>(controller.current(start + 3ms)->value).brightness_permille == 250);
  const auto latest = controller.update(data, start + 4ms, true);
  CHECK(latest && latest->identifier == 2 &&
        std::get<light::Value>(latest->requested).rgbw == data.screen->rgbw);
  auto wrong = applied(*latest, start + 5ms, actual);
  wrong.provider_generation = 2;
  rejects([&] { controller.acknowledge(wrong, start + 5ms); });
  controller.retire(start + 6ms);
  CHECK(!controller.current(start + 6ms));
  CHECK(controller.command(start + 6ms)->reason == control::Reason::provider_lost);
  rejects([&] { controller.acknowledge(applied(*latest, start + 7ms, actual), start + 7ms); });
  controller.begin_generation(2, start + 8ms, start + 8ms);
  const auto successor = controller.update(data, start + 8ms, true);
  CHECK(successor && successor->identifier == 3 && successor->provider_generation == 2);
  CHECK(controller.command(start + 10008ms)->status == control::Status::timed_out);
  CHECK(controller.acknowledge(applied(*successor, start + 10009ms, actual), start + 10009ms)
            .status == control::Status::timed_out);
  CHECK(!controller.current(start + 10009ms));
  data = inputs(start + 10010ms);
  data.screen->rgbw = {255, 0, 0, 0};
  CHECK(!controller.update(data, start + 10010ms, true));  // No automatic physical retry.
}
void screen_retirement_and_replay() {
  auto config = settings();
  config.selection = light::Selection::screen;
  light::Controller controller(config, policy());
  controller.begin_generation(1, start, start);
  const auto original = inputs();
  CHECK(controller.update(original, start, true));
  auto conflicting = original;
  conflicting.screen->rgbw[0] = 255;
  rejects([&] { controller.update(conflicting, start + 1ms, true); });
  auto missing = original;
  missing.screen.reset();
  CHECK(!controller.update(missing, start + 2ms, true));
  CHECK(!controller.requested());
  CHECK(!controller.update(original, start + 3ms, true));
  CHECK(!controller.requested());  // Identical old acquisition cannot revive loss.
  auto recovered = original;
  recovered.screen->timing.acquired_at = start + 4ms;
  CHECK(!controller.update(recovered, start + 4ms, true));  // Still one in-flight command.
  CHECK(controller.requested());
  CHECK(!controller.update(original, start + 5ms, true));
  CHECK(!controller.requested());
  recovered.screen->generation = 2;
  recovered.screen->started_at = start + 6ms;
  recovered.screen->timing.acquired_at = start + 6ms;
  controller.update(recovered, start + 6ms, true);
  CHECK(controller.requested());
  controller.update(original, start + 7ms, true);
  CHECK(!controller.requested());
}
void interlocks_expiry_and_fade() {
  light::Controller denied(settings(), policy());
  denied.begin_generation(1, start, start);
  CHECK(!denied.update(inputs(), start));
  CHECK(denied.command(start)->reason == control::Reason::interlock);
  CHECK(!denied.current(start));
  light::Controller read_only(settings(), policy(false));
  read_only.begin_generation(1, start, start);
  CHECK(!read_only.update(inputs(), start, true));
  CHECK(read_only.command(start)->reason == control::Reason::not_writable);
  light::Controller controller(settings(), policy());
  controller.begin_generation(1, start, start);
  const auto request = controller.update(inputs(), start, true);
  const light::Value from{{0, 0, 0, 0}, 0, 0};
  const auto to = std::get<light::Value>(request->requested);
  const auto middle = light::interpolate(from, to, .5);
  CHECK(middle.rgbw[2] == 30 && middle.transition_ms == 0);
  CHECK(light::interpolate(from, to, 0) == from);
  auto final = to;
  final.transition_ms = 0;
  CHECK(light::interpolate(from, to, 1) == final);
  rejects([&] { light::interpolate(from, to, -1); });
  rejects([&] { light::interpolate(from, to, std::numeric_limits<double>::quiet_NaN()); });
  controller.observe({1, request->target, middle, start + 100ms, control::Origin::simulation},
                     start + 100ms);
  CHECK(controller.command(start + 100ms)->status == control::Status::pending);
  CHECK(controller.current(start + 100ms));
  controller.acknowledge(applied(*request, start + 800ms, to), start + 800ms);
  CHECK(controller.current(start + 2799ms));
  CHECK(!controller.current(start + 2800ms));
  CHECK(light::snapshot_json(controller, start + 2800ms).find("\"current\":null") !=
        std::string::npos);
  CHECK(light::snapshot_json(controller, start + 2800ms).find("\"requested\":null") !=
        std::string::npos);
}
}  // namespace
int main() try {
  configuration_and_wire();
  source_and_night();
  command_lifecycle();
  screen_retirement_and_replay();
  interlocks_expiry_and_fade();
  std::cout << "Ambient controller/configuration/codec/fade/lifecycle checks passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
