// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Original, metadata-bearing shared observations are the policy input. These
// tests never create camera endpoints, connect sockets, render or use hardware.
#include <headunit/camera_activation.hpp>
#include <headunit/vehicle_observation.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace headunit;
using namespace std::chrono_literals;
namespace c = headunit::camera;
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
template <class F>
void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid camera controller configuration was accepted");
}
Clock::time_point at(std::int64_t ms) {
  return Clock::time_point{std::chrono::milliseconds(ms)};
}
constexpr std::string_view json = R"({"schemaVersion":1,"enabled":true,
  "manual":{"enabled":true,"requestFile":"/private/bench/manual"},
  "reverse":{"enabled":true,"basis":"engaged-gear","maximumAgeMs":250,"allowSimulation":false},
  "vehicleSocket":"/private/vehicle/socket",
  "output":{"requestFile":"/private/bench/request","ownerFile":"/private/bench/owner","statusFile":"/private/bench/status"},
  "timing":{"requestLeaseMs":250,"statusMaximumAgeMs":500,"startupTimeoutMs":2000,"pollMs":10}})";
std::string replace(std::string text, std::string_view old, std::string_view replacement) {
  const auto found = text.find(old);
  check(found != text.npos, "invalid test fixture mutation");
  text.replace(found, old.size(), replacement);
  return text;
}
c::ControllerConfiguration configuration() {
  return c::decode_controller_configuration(json);
}
const TransmissionState reverse{GearboxType::automatic, TransmissionSelector::reverse,
                                EngagedGear{EngagedGearKind::reverse, 0}};
VehicleSample observation(const TransmissionState& state = reverse,
                          VehicleObservationOrigin origin = VehicleObservationOrigin::measured,
                          Clock::duration age = 500ms) {
  VehicleAgePolicy ages;
  ages.transmission = age;
  VehicleObservationCache cache(ages);
  cache.begin_generation(1, at(1000), at(1000));
  const VehicleObservation change{"transmission", state, at(1000), origin};
  cache.accept(1, 1, {&change, 1}, at(1000));
  return cache.sample(at(1000));
}
void decision(const c::ActivationDecision& value, bool requested, c::ActivationReason reason) {
  check(value.requested == requested && value.reason == reason,
        "camera activation reason/request mismatch");
}
void schema() {
  auto config = configuration();
  check(config.enabled && config.manual_enabled && config.reverse_enabled &&
            !config.allow_simulation && config.reverse_basis == c::ReverseBasis::engaged_gear &&
            config.request_lease_ms == 250 && config.poll_ms == 10,
        "explicit controller schema was changed");
  config =
      c::decode_controller_configuration(replace(std::string(json), "engaged-gear", "selector"));
  check(config.reverse_basis == c::ReverseBasis::selector, "configured reverse basis was ignored");
  for (const auto& [old, value] : std::vector<std::pair<std::string, std::string>>{
           {"\"schemaVersion\":1", "\"schemaVersion\":2"},
           {"\"enabled\":true", "\"enabled\":1"},
           {"\"basis\":\"engaged-gear\"", "\"basis\":\"R\""},
           {"\"maximumAgeMs\":250", "\"maximumAgeMs\":29"},
           {"\"maximumAgeMs\":250", "\"maximumAgeMs\":1001"},
           {"\"requestLeaseMs\":250", "\"requestLeaseMs\":29"},
           {"\"statusMaximumAgeMs\":500", "\"statusMaximumAgeMs\":1001"},
           {"\"startupTimeoutMs\":2000", "\"startupTimeoutMs\":249"},
           {"\"startupTimeoutMs\":2000", "\"startupTimeoutMs\":10001"},
           {"\"pollMs\":10", "\"pollMs\":0"},
           {"\"pollMs\":10", "\"pollMs\":51"},
           {"\"pollMs\":10", "\"pollMs\":10.0"},
           {"\"pollMs\":10", "\"pollMs\":true"},
           {"\"allowSimulation\":false", "\"allowSimulation\":null"},
           {"\"manual\":{", "\"manual\":{\"extra\":false,"},
           {"\"reverse\":{", "\"reverse\":{\"extra\":false,"},
           {"\"output\":{", "\"output\":{\"extra\":false,"},
           {"\"timing\":{", "\"timing\":{\"extra\":false,"},
           {"\"pollMs\":10", "\"pollMs\":10,\"pollMs\":10"},
           {"\"schemaVersion\":1,", ""},
           {"\"allowSimulation\":false", ""},
           {"\"vehicleSocket\":\"/private/vehicle/socket\",", ""}})
    rejects([&] { c::decode_controller_configuration(replace(std::string(json), old, value)); });
  rejects([] { c::decode_controller_configuration("{}"); });
  rejects([] { c::decode_controller_configuration(std::string(8193, ' ')); });
  config = configuration();
  config.request_lease_ms = 30;
  config.status_maximum_age_ms = 30;
  config.poll_ms = 10;
  c::validate_controller_configuration(config);
  config.poll_ms = 11;
  rejects([&] { c::validate_controller_configuration(config); });
  config = configuration();
  config.status_maximum_age_ms = 30;
  config.poll_ms = 11;
  rejects([&] { c::validate_controller_configuration(config); });
  config = configuration();
  config.reverse_basis = static_cast<c::ReverseBasis>(99);
  rejects([&] { c::validate_controller_configuration(config); });
}
void paths() {
  for (const auto& bad : std::vector<std::string>{
           "", "relative", "/", "/private/../manual", "/private/./manual", "/private//manual",
           "/private/manual/", "/private/manual;echo", "/private/$(id)", "/private/`id`",
           "/private/manual\n", std::string("/private/manual\0after", 21),
           "/" + std::string(4000, 'a')}) {
    auto config = configuration();
    config.manual_request_file = bad;
    rejects([&] { c::validate_controller_configuration(config); });
  }
  for (std::size_t a = 0; a < 5; ++a)
    for (std::size_t b = a + 1; b < 5; ++b) {
      auto config = configuration();
      std::string* fields[] = {&config.manual_request_file, &config.vehicle_socket,
                               &config.output_request_file, &config.output_owner_file,
                               &config.status_file};
      *fields[b] = *fields[a];
      rejects([&] { c::validate_controller_configuration(config); });
    }
  for (std::size_t i = 0; i < 4; ++i) {
    auto config = configuration();
    std::string* fields[] = {&config.manual_request_file, &config.vehicle_socket,
                             &config.output_request_file, &config.output_owner_file};
    *fields[i] = config.status_file + ".owner";
    rejects([&] { c::validate_controller_configuration(config); });
  }
  for (std::size_t i = 0; i < 4; ++i) {
    auto config = configuration();
    std::string* fields[] = {&config.manual_request_file, &config.output_request_file,
                             &config.output_owner_file, &config.status_file};
    *fields[i] = config.vehicle_socket + ".lock";
    rejects([&] { c::validate_controller_configuration(config); });
  }
  auto maximum = configuration();
  maximum.status_file = "/" + std::string(3993, 's');
  c::validate_controller_configuration(maximum);
  maximum.status_file += 's';
  rejects([&] { c::validate_controller_configuration(maximum); });
  auto config = configuration();
  config.vehicle_socket = "/" + std::string(103, 's');
  rejects([&] { c::validate_controller_configuration(config); });
  config.vehicle_socket = "/" + std::string(102, 's');
  c::validate_controller_configuration(config);
  config = configuration();
  config.manual_enabled = false;
  config.manual_request_file.clear();
  config.reverse_enabled = false;
  config.vehicle_socket.clear();
  c::validate_controller_configuration(config);
  config.manual_enabled = true;
  rejects([&] { c::validate_controller_configuration(config); });
  config.manual_enabled = false;
  config.reverse_enabled = true;
  rejects([&] { c::validate_controller_configuration(config); });
  rejects([] {
    c::decode_controller_configuration(
        replace(std::string(json), "/private/bench/manual", "/private/bench/manual\\u0000after"));
  });
}
void exact_basis_and_manual() {
  auto config = configuration();
  auto sample = observation();
  decision(c::decide_activation(config, false, sample, at(1000)), true,
           c::ActivationReason::reverse);
  const TransmissionState selector_only{GearboxType::automatic, TransmissionSelector::reverse,
                                        std::nullopt};
  sample = observation(selector_only);
  decision(c::decide_activation(config, false, sample, at(1000)), false,
           c::ActivationReason::unknown_component);
  config.reverse_basis = c::ReverseBasis::selector;
  decision(c::decide_activation(config, false, sample, at(1000)), true,
           c::ActivationReason::reverse);
  sample =
      observation({GearboxType::manual, std::nullopt, EngagedGear{EngagedGearKind::reverse, 0}});
  decision(c::decide_activation(config, false, sample, at(1000)), false,
           c::ActivationReason::unknown_component);
  config.reverse_basis = c::ReverseBasis::engaged_gear;
  decision(c::decide_activation(config, false, sample, at(1000)), true,
           c::ActivationReason::reverse);
  sample = observation({GearboxType::automatic, std::nullopt, std::nullopt});
  sample.speed_kmh = 0;
  sample.engine_rpm = 1500;
  sample.observation_timing->readings[0] = {at(1000), 500ms, VehicleObservationOrigin::measured,
                                            VehicleAvailability::available};
  sample.observation_timing->readings[1] = sample.observation_timing->readings[0];
  decision(c::decide_activation(config, false, sample, at(1000)), false,
           c::ActivationReason::unknown_component);
  sample = observation({GearboxType::automatic, TransmissionSelector::drive,
                        EngagedGear{EngagedGearKind::forward, 1}});
  decision(c::decide_activation(config, false, sample, at(1000)), false,
           c::ActivationReason::no_request);
  sample.speed_kmh = std::numeric_limits<double>::quiet_NaN();
  const auto manual = c::decide_activation(config, true, sample, Clock::time_point{});
  decision(manual, true, c::ActivationReason::manual);
  check(!manual.acquisition_deadline, "manual request fabricated a vehicle acquisition");
  config.manual_enabled = false;
  decision(c::decide_activation(config, true, sample, at(1000)), false,
           c::ActivationReason::invalid);
  config.reverse_enabled = false;
  decision(c::decide_activation(config, true, sample, at(1000)), false,
           c::ActivationReason::no_request);
  config.enabled = false;
  decision(c::decide_activation(config, true, observation(), at(1000)), false,
           c::ActivationReason::disabled);
}
void metadata_and_provenance() {
  const auto config = configuration();
  VehicleSample legacy{0, 0, at(1000)};
  legacy.transmission = reverse;
  decision(c::decide_activation(config, false, legacy, at(1000)), false,
           c::ActivationReason::unavailable);
  auto sample = observation();
  sample.transmission->engaged_gear = EngagedGear{EngagedGearKind::forward, 1};
  decision(c::decide_activation(config, false, sample, at(1000)), false,
           c::ActivationReason::invalid);
  for (const auto mutation : {0, 1, 2, 3, 4, 5, 6}) {
    sample = observation();
    auto& timing = *sample.observation_timing;
    if (mutation == 0) timing.generation = 0;
    if (mutation == 1) timing.started_at = at(1001);
    if (mutation == 2) sample.sampled_at = at(1001);
    if (mutation == 3) timing.transmission.acquired_at = at(999);
    if (mutation == 4) timing.transmission.acquired_at.reset();
    if (mutation == 5) timing.transmission.maximum_age = 0ns;
    if (mutation == 6) timing.transmission.origin = static_cast<VehicleObservationOrigin>(99);
    decision(c::decide_activation(config, false, sample, at(1000)), false,
             c::ActivationReason::invalid);
  }
  sample = observation();
  decision(c::decide_activation(config, false, sample, at(999)), false,
           c::ActivationReason::invalid);
  for (const auto state : {VehicleAvailability::unavailable, VehicleAvailability::disconnected,
                           VehicleAvailability::stale}) {
    sample = observation();
    sample.transmission.reset();
    sample.observation_timing->transmission.availability = state;
    decision(c::decide_activation(config, false, sample, at(1200)), false,
             state == VehicleAvailability::stale ? c::ActivationReason::stale
                                                 : c::ActivationReason::unavailable);
  }
  sample = observation(reverse, VehicleObservationOrigin::simulation);
  decision(c::decide_activation(config, false, sample, at(1000)), false,
           c::ActivationReason::simulation);
  auto permitted = config;
  permitted.allow_simulation = true;
  decision(c::decide_activation(permitted, false, sample, at(1000)), true,
           c::ActivationReason::reverse);
  sample = observation(reverse, VehicleObservationOrigin::calculated);
  decision(c::decide_activation(config, false, sample, at(1000)), true,
           c::ActivationReason::reverse);
  sample = observation();
  sample.coolant_c = 88;
  sample.simulated = true;
  sample.observation_timing->readings[3] = {at(1000), 500ms, VehicleObservationOrigin::simulation,
                                            VehicleAvailability::available};
  decision(c::decide_activation(config, false, sample, at(1000)), true,
           c::ActivationReason::reverse);
  sample.simulated = false;
  decision(c::decide_activation(config, false, sample, at(1000)), false,
           c::ActivationReason::invalid);
}
void original_deadlines_and_cache() {
  const auto config = configuration();
  auto sample = observation();
  auto result = c::decide_activation(config, false, sample, at(1249));
  decision(result, true, c::ActivationReason::reverse);
  check(result.acquisition_deadline == at(1250), "configured deadline renewed");
  result = c::decide_activation(config, false, sample, at(1250));
  decision(result, false, c::ActivationReason::stale);
  check(result.acquisition_deadline == at(1250), "expired deadline renewed");
  sample = observation(reverse, VehicleObservationOrigin::measured, 100ms);
  result = c::decide_activation(config, false, sample, at(1099));
  decision(result, true, c::ActivationReason::reverse);
  check(result.acquisition_deadline == at(1100), "driver age was replaced by configured age");
  decision(c::decide_activation(config, false, sample, at(1100)), false,
           c::ActivationReason::stale);
  VehicleObservationCache cache;
  cache.begin_generation(1, at(1000), at(1000));
  const VehicleObservation change{"transmission", reverse, at(1000)};
  cache.accept(1, 1, {&change, 1}, at(1000));
  sample = cache.sample(at(1100));
  cache.accept(1, 2, {},
               at(1200));  // A fresh publication/heartbeat is not a new reverse observation.
  sample = cache.sample(at(1249));
  result = c::decide_activation(config, false, sample, at(1249));
  check(result.acquisition_deadline == at(1250) && sample.sampled_at == at(1249) &&
            sample.observation_timing->transmission.acquired_at == at(1000),
        "cache poll or heartbeat renewed reverse acquisition");
  decision(c::decide_activation(config, false, cache.sample(at(1250)), at(1250)), false,
           c::ActivationReason::stale);
  cache.withdraw(at(1260));
  decision(c::decide_activation(config, false, cache.sample(at(1260)), at(1260)), false,
           c::ActivationReason::unavailable);
  cache.accept(1, 3, {&change, 1}, at(1270));  // Idempotent replay cannot undo withdrawal.
  decision(c::decide_activation(config, false, cache.sample(at(1270)), at(1270)), false,
           c::ActivationReason::unavailable);
  cache.begin_generation(2, at(1300), at(1300));
  decision(c::decide_activation(config, false, cache.sample(at(1300)), at(1300)), false,
           c::ActivationReason::unavailable);
  bool rejected = false;
  try {
    cache.accept(1, 4, {&change, 1}, at(1300));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "cache admitted a previous monotonic generation");
  const VehicleObservation fresh{"transmission", reverse, at(1301)};
  cache.accept(2, 1, {&fresh, 1}, at(1301));
  result = c::decide_activation(config, false, cache.sample(at(1302)), at(1302));
  decision(result, true, c::ActivationReason::reverse);
  check(result.acquisition_deadline == at(1551), "new acquisition deadline wrong");
  const auto almost_end = Clock::time_point::max() - 1ms;
  sample = observation();
  sample.sampled_at = almost_end;
  sample.observation_timing->started_at = almost_end;
  sample.observation_timing->transmission.acquired_at = almost_end;
  decision(c::decide_activation(config, false, sample, almost_end), false,
           c::ActivationReason::invalid);
}
void disabled_example() {
  std::ifstream file(std::string(HEADUNIT_SOURCE_DIR) + "/config/camera-controller.example.json");
  check(bool(file), "camera controller example missing");
  std::stringstream text;
  text << file.rdbuf();
  const auto config = c::decode_controller_configuration(text.str());
  check(!config.enabled && !config.reverse_enabled && !config.allow_simulation &&
            config.manual_enabled,
        "example silently enabled reverse activation or simulation");
  decision(c::decide_activation(config, true, observation(), at(1000)), false,
           c::ActivationReason::disabled);
}
}  // namespace
int main() try {
  schema();
  paths();
  exact_basis_and_manual();
  metadata_and_provenance();
  original_deadlines_and_cache();
  disabled_example();
  std::cout << "Camera activation schema, exact reverse/manual policy, original "
               "acquisition/provenance and shared cache passed (no I/O)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
