// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_activation.hpp>
#include "theme/vehicle_json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>

namespace headunit::camera {
namespace {
using V = theme::opack::Value;
void check(bool condition, const char* why) {
  if (!condition) throw std::invalid_argument(why);
}
const theme::opack::Dictionary& object(const V& value,
                                       std::initializer_list<std::string_view> keys) {
  const auto* fields = std::get_if<theme::opack::Dictionary>(&value.data);
  check(fields && fields->size() == keys.size(), "invalid camera controller configuration fields");
  for (const auto& [key, unused] : *fields) {
    (void)unused;
    const auto* name = std::get_if<std::string>(&key.data);
    check(name && std::find(keys.begin(), keys.end(), *name) != keys.end(),
          "unknown camera controller configuration field");
  }
  return *fields;
}
const V& field(const theme::opack::Dictionary& fields, std::string_view key) {
  for (const auto& [name, value] : fields)
    if (std::get<std::string>(name.data) == key) return value;
  throw std::invalid_argument("missing camera controller configuration field");
}
std::string string(const V& value) {
  const auto* result = std::get_if<std::string>(&value.data);
  check(result, "camera controller configuration value must be a string");
  return *result;
}
bool flag(const V& value) {
  const auto* result = std::get_if<bool>(&value.data);
  check(result, "camera controller configuration flag must be boolean");
  return *result;
}
unsigned integer(const V& value, unsigned maximum) {
  const auto* result = std::get_if<std::int64_t>(&value.data);
  check(result && *result >= 0 && std::uint64_t(*result) <= maximum,
        "camera controller timing must be a bounded integer");
  return unsigned(*result);
}
bool path(std::string_view value, std::size_t maximum) {
  if (value.size() < 2 || value.size() > maximum || value.front() != '/' || value.back() == '/')
    return false;
  if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '/' || c == '-' || c == '_' || c == '.';
      }))
    return false;
  for (std::size_t begin = 1; begin < value.size();) {
    const auto slash = value.find('/', begin);
    const auto end = slash == value.npos ? value.size() : slash;
    const auto component = value.substr(begin, end - begin);
    if (component.empty() || component == "." || component == "..") return false;
    begin = end + 1;
  }
  return true;
}
}  // namespace
void validate_controller_configuration(const ControllerConfiguration& c) {
  check(c.reverse_basis == ReverseBasis::engaged_gear || c.reverse_basis == ReverseBasis::selector,
        "unknown reverse activation basis");
  check(c.reverse_maximum_age_ms >= 30 && c.reverse_maximum_age_ms <= 1000,
        "reverse activation age exceeds bounds");
  check(c.request_lease_ms >= 30 && c.request_lease_ms <= 1000,
        "camera request lease exceeds bounds");
  check(c.status_maximum_age_ms >= 30 && c.status_maximum_age_ms <= 1000,
        "camera status age exceeds bounds");
  check(c.startup_timeout_ms >= 250 && c.startup_timeout_ms <= 10000,
        "camera startup timeout exceeds bounds");
  check(c.poll_ms >= 1 && c.poll_ms <= 50 && c.poll_ms * 3 <= c.request_lease_ms &&
            c.poll_ms * 3 <= c.status_maximum_age_ms,
        "camera controller polling exceeds lease or status policy");
  check(!c.manual_enabled || !c.manual_request_file.empty(),
        "manual camera activation requires a request file");
  check(!c.reverse_enabled || !c.vehicle_socket.empty(),
        "reverse camera activation requires a vehicle socket");
  check(!c.output_request_file.empty() && !c.output_owner_file.empty() && !c.status_file.empty(),
        "camera controller requires output request, owner and status files");
  const std::array<std::string_view, 5> paths = {c.manual_request_file, c.vehicle_socket,
                                                 c.output_request_file, c.output_owner_file,
                                                 c.status_file};
  for (std::size_t i = 0; i < paths.size(); ++i) {
    if (paths[i].empty()) continue;
    check(path(paths[i], i == 1   ? 103
                         : i == 4 ? 3994
                                  : 4000),
          "camera controller paths must be bounded canonical absolute ASCII paths");
    for (std::size_t j = 0; j < i; ++j)
      check(paths[i] != paths[j], "camera controller endpoints must be distinct");
  }
  const auto status_owner = c.status_file + ".owner";
  for (const auto selected : paths)
    check(selected != status_owner, "camera controller endpoint collides with status owner");
  if (!c.vehicle_socket.empty()) {
    const auto vehicle_owner = c.vehicle_socket + ".lock";
    for (const auto selected : paths)
      check(selected != vehicle_owner, "camera controller endpoint collides with vehicle owner");
    check(status_owner != vehicle_owner, "camera controller derived owners collide");
  }
}
ControllerConfiguration decode_controller_configuration(std::string_view text) {
  check(text.size() <= 8192, "camera controller configuration exceeds 8 KiB");
  const auto parsed = theme::vehicle::detail::Json(text).parse();
  const auto& root = object(parsed, {"schemaVersion", "enabled", "manual", "reverse",
                                     "vehicleSocket", "output", "timing"});
  check(integer(field(root, "schemaVersion"), 1) == 1, "unsupported camera controller schema");
  ControllerConfiguration c;
  c.enabled = flag(field(root, "enabled"));
  const auto& manual = object(field(root, "manual"), {"enabled", "requestFile"});
  c.manual_enabled = flag(field(manual, "enabled"));
  c.manual_request_file = string(field(manual, "requestFile"));
  const auto& reverse =
      object(field(root, "reverse"), {"enabled", "basis", "maximumAgeMs", "allowSimulation"});
  c.reverse_enabled = flag(field(reverse, "enabled"));
  const auto basis = string(field(reverse, "basis"));
  if (basis == "engaged-gear")
    c.reverse_basis = ReverseBasis::engaged_gear;
  else if (basis == "selector")
    c.reverse_basis = ReverseBasis::selector;
  else
    throw std::invalid_argument("unknown reverse activation basis");
  c.reverse_maximum_age_ms = integer(field(reverse, "maximumAgeMs"), 1000);
  c.allow_simulation = flag(field(reverse, "allowSimulation"));
  c.vehicle_socket = string(field(root, "vehicleSocket"));
  const auto& output = object(field(root, "output"), {"requestFile", "ownerFile", "statusFile"});
  c.output_request_file = string(field(output, "requestFile"));
  c.output_owner_file = string(field(output, "ownerFile"));
  c.status_file = string(field(output, "statusFile"));
  const auto& timing = object(field(root, "timing"), {"requestLeaseMs", "statusMaximumAgeMs",
                                                      "startupTimeoutMs", "pollMs"});
  c.request_lease_ms = integer(field(timing, "requestLeaseMs"), 1000);
  c.status_maximum_age_ms = integer(field(timing, "statusMaximumAgeMs"), 1000);
  c.startup_timeout_ms = integer(field(timing, "startupTimeoutMs"), 10000);
  c.poll_ms = integer(field(timing, "pollMs"), 50);
  validate_controller_configuration(c);
  return c;
}
ActivationDecision decide_activation(const ControllerConfiguration& c, bool manual,
                                     const VehicleSample& sample, Clock::time_point now) {
  try {
    validate_controller_configuration(c);
  } catch (const std::invalid_argument&) {
    return {false, ActivationReason::invalid};
  }
  if (!c.enabled) return {false, ActivationReason::disabled};
  if (manual && c.manual_enabled) return {true, ActivationReason::manual};
  if (!c.reverse_enabled) return {false, ActivationReason::no_request};
  if (!sample.observation_timing) return {false, ActivationReason::unavailable};
  if (now <= Clock::time_point{} || sample.sampled_at > now || !valid_vehicle_sample(sample))
    return {false, ActivationReason::invalid};
  const auto& transmission = sample.observation_timing->transmission;
  if (transmission.availability != VehicleAvailability::available &&
      transmission.availability != VehicleAvailability::stale)
    return {false, ActivationReason::unavailable};
  // valid_vehicle_sample proves a positive generation, acquisition within its
  // original generation and publication, and a coherent available value (or
  // an already withdrawn stale observation with its original acquisition).
  const auto acquired = *transmission.acquired_at;
  const auto maximum_age =
      std::min(transmission.maximum_age, std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::milliseconds(c.reverse_maximum_age_ms)));
  if (acquired > Clock::time_point::max() - maximum_age) return {false, ActivationReason::invalid};
  const auto deadline = acquired + maximum_age;
  if (transmission.availability == VehicleAvailability::stale || now >= deadline)
    return {false, ActivationReason::stale, deadline};
  if (transmission.origin == VehicleObservationOrigin::simulation && !c.allow_simulation)
    return {false, ActivationReason::simulation, deadline};
  bool reverse = false;
  if (c.reverse_basis == ReverseBasis::engaged_gear) {
    if (!sample.transmission->engaged_gear)
      return {false, ActivationReason::unknown_component, deadline};
    reverse = sample.transmission->engaged_gear->kind == EngagedGearKind::reverse;
  } else {
    if (!sample.transmission->selector)
      return {false, ActivationReason::unknown_component, deadline};
    reverse = *sample.transmission->selector == TransmissionSelector::reverse;
  }
  return {reverse, reverse ? ActivationReason::reverse : ActivationReason::no_request, deadline};
}
}  // namespace headunit::camera
