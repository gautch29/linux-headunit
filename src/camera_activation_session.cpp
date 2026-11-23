// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_activation_session.hpp>
#include <algorithm>
#include <limits>
#include <utility>

namespace headunit::camera {
namespace {
std::uint64_t date(Clock::time_point time) {
  if (time <= Clock::time_point{}) return 0;
  return std::uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}
bool healthy(const ControlStatus& peer, std::uint64_t now, unsigned maximum_age_ms) {
  return peer.instance_started_ns && peer.sequence &&
         peer.observed_ns >= peer.instance_started_ns && peer.observed_ns <= now &&
         now - peer.observed_ns < std::uint64_t(maximum_age_ms) * 1'000'000 &&
         (peer.phase == ControlPhase::idle || peer.phase == ControlPhase::waiting ||
          peer.phase == ControlPhase::active);
}
}  // namespace
ActivationSession::ActivationSession(ControllerConfiguration config) : config_(std::move(config)) {
  validate_controller_configuration(config_);
}
ActivationCommand ActivationSession::turn(const ActivationDecision& desired,
                                          const std::optional<ControlStatus>& peer,
                                          Clock::time_point time, bool off_confirmed) {
  const auto now = date(time);
  if (!now || now < last_now_) retired_ = true;
  if (retired_) return {false, 0, ControllerState::fault, ControllerFault::clock};
  last_now_ = now;
  bool wanted = config_.enabled && desired.requested;
  if (desired.reason == ActivationReason::reverse && !desired.acquisition_deadline) wanted = false;
  if (desired.acquisition_deadline && time >= *desired.acquisition_deadline) wanted = false;
  if (!wanted) {
    if (fault_ != ControllerFault::none && !off_confirmed)
      return {false, 0, ControllerState::fault, fault_};
    attempted_ = false;
    confirmed_ = false;
    fault_ = ControllerFault::none;
    instance_ = 0;
    sequence_floor_ = 0;
    started_ns_ = 0;
    return {};
  }
  if (fault_ != ControllerFault::none) return {false, 0, ControllerState::fault, fault_};
  const auto fail = [&](ControllerFault why) {
    fault_ = why;
    return ActivationCommand{false, 0, ControllerState::fault, why};
  };
  if (!peer || !healthy(*peer, now, config_.status_maximum_age_ms)) {
    if (peer && (peer->phase == ControlPhase::fault || peer->phase == ControlPhase::stopped))
      return fail(ControllerFault::owner_fault);
    if (attempted_) return fail(ControllerFault::owner_unavailable);
    return {false, 0, ControllerState::waiting_for_owner, ControllerFault::none};
  }
  if (!attempted_) {
    // Constructor publication is OFF. Observe its withdrawal before issuing a
    // new ON; a surviving old active surface/heartbeat is not a new receipt.
    if (peer->phase != ControlPhase::idle)
      return {false, 0, ControllerState::waiting_for_owner, ControllerFault::none};
    attempted_ = true;
    instance_ = peer->instance_started_ns;
    sequence_floor_ = peer->sequence;
    started_ns_ = now;
  } else if (peer->instance_started_ns != instance_)
    return fail(ControllerFault::owner_restarted);
  if (peer->phase == ControlPhase::active && peer->sequence > sequence_floor_ &&
      peer->observed_ns >= started_ns_)
    confirmed_ = true;
  else if (confirmed_)
    return fail(ControllerFault::visibility_lost);
  if (!confirmed_ && now - started_ns_ >= std::uint64_t(config_.startup_timeout_ms) * 1'000'000)
    return fail(ControllerFault::startup_timeout);
  const auto lease = std::uint64_t(config_.request_lease_ms) * 1'000'000;
  const auto maximum = std::uint64_t(std::numeric_limits<Clock::duration::rep>::max());
  auto deadline = now > maximum - lease ? maximum : now + lease;
  if (desired.acquisition_deadline)
    deadline = std::min(deadline, date(*desired.acquisition_deadline));
  if (deadline <= now) return {};
  return {true, deadline, confirmed_ ? ControllerState::active : ControllerState::starting,
          ControllerFault::none};
}
}  // namespace headunit::camera
