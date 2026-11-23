// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace headunit::camera {
enum class ReverseBasis { engaged_gear, selector };
struct ControllerConfiguration {
  bool enabled = false;
  bool manual_enabled = true;
  // vehicle_socket is a dedicated broker fanout endpoint: SocketVehicle binds
  // and owns it, so it cannot reuse a cluster/Ultra receiver's socket path.
  std::string manual_request_file, vehicle_socket, output_request_file, output_owner_file,
      status_file;
  bool reverse_enabled = false;
  ReverseBasis reverse_basis = ReverseBasis::engaged_gear;
  unsigned reverse_maximum_age_ms = 250;
  bool allow_simulation = false;
  unsigned request_lease_ms = 250, status_maximum_age_ms = 500, startup_timeout_ms = 2000,
           poll_ms = 10;
};
// File/schema validation only. Runtime owners separately preflight private
// endpoint identities and sockets; validation never opens or creates them.
// The derived status_file + ".owner" endpoint is also reserved and distinct.
void validate_controller_configuration(const ControllerConfiguration&);
ControllerConfiguration decode_controller_configuration(std::string_view);
enum class ActivationReason {
  disabled,
  no_request,
  manual,
  reverse,
  unavailable,
  stale,
  simulation,
  unknown_component,
  invalid
};
struct ActivationDecision {
  bool requested = false;
  ActivationReason reason = ActivationReason::no_request;
  std::optional<Clock::time_point> acquisition_deadline = std::nullopt;
};
// Requires an original metadata-bearing transmission observation. The sample
// must come from the shared cache/receiver in this host's monotonic generation;
// this pure policy does not renew dates, infer R, latch requests or perform I/O.
// Manual activation is independent of unavailable/invalid vehicle observations.
ActivationDecision decide_activation(const ControllerConfiguration&, bool manual,
                                     const VehicleSample&, Clock::time_point now);
}  // namespace headunit::camera
