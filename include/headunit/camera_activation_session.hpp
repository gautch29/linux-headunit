// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/camera_activation.hpp>
#include <headunit/camera_controller_io.hpp>
#include <cstdint>

namespace headunit::camera {
enum class ControllerState { idle, waiting_for_owner, starting, active, fault };
enum class ControllerFault {
  none,
  owner_unavailable,
  owner_restarted,
  owner_fault,
  startup_timeout,
  visibility_lost,
  clock
};
struct ActivationCommand {
  bool requested = false;
  std::uint64_t deadline_ns = 0;
  ControllerState state = ControllerState::idle;
  ControllerFault fault = ControllerFault::none;
};
// Single control owner. A failed attempt stays off while the intended request
// remains true. Only the genuine manual/reverse request returning false resets
// it; our own off publication is never an input. Native status acknowledges a
// new request only with a later snapshot from the same owner instance.
class ActivationSession {
 public:
  explicit ActivationSession(ControllerConfiguration);
  // off_confirmed distinguishes an intended valid off from missing/unsafe
  // request or unknown vehicle input. Input loss withdraws, never clears fault.
  ActivationCommand turn(const ActivationDecision&, const std::optional<ControlStatus>&,
                         Clock::time_point now, bool off_confirmed = false);

 private:
  ControllerConfiguration config_;
  std::uint64_t last_now_ = 0, instance_ = 0, sequence_floor_ = 0, started_ns_ = 0;
  bool attempted_ = false, confirmed_ = false, retired_ = false;
  ControllerFault fault_ = ControllerFault::none;
};
}  // namespace headunit::camera
