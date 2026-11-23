// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/ambient_light.hpp>
#include <headunit/vehicle_command_configuration.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace headunit;
namespace light = ambient;
namespace control = vehicle_control;
using namespace std::chrono_literals;
std::string bounded_file(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::invalid_argument("cannot open ambient settings");
  std::string text(16385, '\0');
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(file.gcount()));
  return text;
}
}  // namespace
int main(int argc, char** argv) try {
  if (argc != 3)
    throw std::invalid_argument(
        "usage: headunit-ambient-light-simulator SETTINGS.json CONTROLS.json");
  auto settings = light::parse_settings(bounded_file(argv[1]));
  const auto policies = control::load_configuration(argv[2]);
  if (policies.size() != 1 || policies[0].kind != control::ValueKind::ambient_light)
    throw std::invalid_argument("simulator requires exactly one ambient_light target");
  // Overrides are explicit simulation only; no bus/GPIO/socket/display is opened.
  settings.enabled = true;
  auto policy = policies[0];
  policy.writable = true;
  light::Controller controller(settings, policy);
  const auto started = Clock::now();
  controller.begin_generation(1, started, started);
  light::Inputs input;
  input.local_minute = 12 * 60;
  input.screen = light::Accent{
      {32, 96, 255, 0},
      1,
      started,
      {started, 3s, VehicleObservationOrigin::simulation, VehicleAvailability::available}};
  std::cerr << "Simulation only: enabled/writable overridden; no phone, MCU or LED hardware.\n";
  auto request = controller.update(input, started, true);
  if (!request) throw std::runtime_error("simulation request unavailable");
  const auto destination = std::get<light::Value>(request->requested);
  const light::Value off{};
  std::cout << light::snapshot_json(controller, started) << '\n';
  const auto duration = std::chrono::milliseconds(settings.transition_ms);
  for (unsigned step = 1; step <= 10; ++step) {
    std::this_thread::sleep_until(started + duration * step / 10);
    const auto now = Clock::now();
    const auto value = light::interpolate(off, destination, double(step) / 10);
    if (step < 10)
      controller.observe({1, policy.key, value, now, control::Origin::simulation}, now);
    else {
      controller.acknowledge(
          {request->identifier, 1, 1, policy.key, control::ReplyStatus::applied, now,
           control::ConfirmedState{1, policy.key, value, now, control::Origin::simulation}},
          now);
    }
    controller.update(input, now, true);
    std::cout << light::snapshot_json(controller, now) << '\n';
  }
  // Fresh original acquisition of simulated night policy + changed screen accent.
  auto night = settings;
  night.night_policy.kind = NightModePolicy::Kind::on;
  controller.settings(night);
  auto now = Clock::now();
  input.screen->timing.acquired_at = now;
  input.screen->rgbw = {255, 32, 8, 0};
  request = controller.update(input, now, true);
  if (!request) throw std::runtime_error("night simulation request unavailable");
  std::cout << light::snapshot_json(controller, now) << '\n';
  std::this_thread::sleep_until(now + duration);
  now = Clock::now();
  auto value = std::get<light::Value>(request->requested);
  value.transition_ms = 0;
  controller.acknowledge(
      {request->identifier, 1, 1, policy.key, control::ReplyStatus::applied, now,
       control::ConfirmedState{1, policy.key, value, now, control::Origin::simulation}},
      now);
  controller.update(input, now, true);
  std::cout << light::snapshot_json(controller, now) << '\n';
  input.screen.reset();
  now = Clock::now();
  controller.update(input, now, true);
  std::cout << light::snapshot_json(controller, now) << '\n';
  controller.retire(now);
  std::cout << light::snapshot_json(controller, now) << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
