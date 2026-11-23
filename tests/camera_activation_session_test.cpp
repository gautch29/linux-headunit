// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_activation_session.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace c = headunit::camera;
using namespace std::chrono_literals;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
headunit::Clock::time_point at(unsigned ms) {
  return headunit::Clock::time_point{std::chrono::milliseconds(ms)};
}
c::ControllerConfiguration config() {
  c::ControllerConfiguration result;
  result.enabled = true;
  result.manual_request_file = "/tmp/private/manual";
  result.output_request_file = "/tmp/private/request";
  result.output_owner_file = "/tmp/private/controller-owner";
  result.status_file = "/tmp/private/status";
  result.startup_timeout_ms = 250;
  return result;
}
c::ControlStatus peer(unsigned observed, c::ControlPhase phase = c::ControlPhase::idle,
                      std::uint64_t sequence = 1, unsigned instance = 500) {
  return {std::uint64_t(instance) * 1'000'000, sequence, std::uint64_t(observed) * 1'000'000,
          phase};
}
const c::ActivationDecision manual{true, c::ActivationReason::manual};
void acknowledgement_and_expiry() {
  c::ActivationSession session(config());
  auto result = session.turn(manual, std::nullopt, at(1000));
  check(!result.requested && result.state == c::ControllerState::waiting_for_owner,
        "missing owner activated camera");
  result = session.turn(manual, peer(1001, c::ControlPhase::active), at(1001));
  check(!result.requested && result.state == c::ControllerState::waiting_for_owner,
        "old active owner bypassed initial withdrawal");
  result = session.turn(manual, peer(1002, c::ControlPhase::idle, 2), at(1002));
  check(result.requested && result.state == c::ControllerState::starting &&
            result.deadline_ns == 1'252'000'000,
        "fresh idle owner did not begin a bounded request");
  result = session.turn(manual, peer(1002, c::ControlPhase::idle, 2), at(1002));
  check(result.state == c::ControllerState::starting,
        "unchanged owner snapshot acknowledged request");
  result = session.turn(manual, peer(1003, c::ControlPhase::active, 3), at(1003));
  check(result.requested && result.state == c::ControllerState::active,
        "fresh actual owner acknowledgement rejected");
  result = session.turn(manual, peer(1003, c::ControlPhase::active, 3), at(1503));
  check(!result.requested && result.state == c::ControllerState::fault,
        "exact-boundary stale owner remained active");
  result = session.turn(manual, peer(1504, c::ControlPhase::active, 4), at(1504));
  check(!result.requested, "new heartbeat cleared a failed intended request");
  check(session.turn({}, peer(1505), at(1505)).state == c::ControllerState::fault,
        "unavailable request cleared fault latch");
  check(session.turn({}, peer(1505), at(1505), true).state == c::ControllerState::idle,
        "genuine off did not reset fault");
  check(session.turn(manual, peer(1506, c::ControlPhase::idle, 4), at(1506)).requested,
        "new off/on edge failed to recover");
}
void bounded_startup() {
  c::ActivationSession session(config());
  check(session.turn(manual, peer(1000), at(1000)).requested, "startup request missing");
  for (unsigned date = 1050; date < 1250; date += 50)
    check(session.turn(manual, peer(date, c::ControlPhase::waiting, date), at(date)).requested,
          "startup expired early");
  const auto failure = session.turn(manual, peer(1250, c::ControlPhase::waiting, 50), at(1250));
  check(!failure.requested && failure.fault == c::ControllerFault::startup_timeout,
        "waiting renewed the startup deadline");
  check(!session.turn(manual, peer(1251, c::ControlPhase::active, 51), at(1251)).requested,
        "late active bypassed timeout latch");
}
void owner_changes() {
  for (const auto phase : {c::ControlPhase::fault, c::ControlPhase::stopped}) {
    c::ActivationSession session(config());
    session.turn(manual, peer(1000), at(1000));
    check(!session.turn(manual, peer(1001, phase, 2), at(1001)).requested,
          "terminal owner phase retained request");
    check(!session.turn(manual, peer(1002, c::ControlPhase::idle, 3), at(1002)).requested,
          "terminal owner fault recovered without off");
  }
  c::ActivationSession session(config());
  session.turn(manual, peer(1000), at(1000));
  const auto changed = session.turn(manual, peer(1001, c::ControlPhase::active, 2, 1001), at(1001));
  check(!changed.requested && changed.fault == c::ControllerFault::owner_restarted,
        "new owner instance resumed old request");
  c::ActivationSession lost(config());
  lost.turn(manual, peer(1000), at(1000));
  lost.turn(manual, peer(1001, c::ControlPhase::active, 2), at(1001));
  const auto hidden = lost.turn(manual, peer(1002, c::ControlPhase::waiting, 3), at(1002));
  check(!hidden.requested && hidden.fault == c::ControllerFault::visibility_lost,
        "withdrawn active scene retained request");
}
void acquisition_floor() {
  c::ActivationSession session(config());
  c::ActivationDecision reverse{true, c::ActivationReason::reverse, at(1100)};
  check(session.turn(reverse, peer(1000), at(1000)).deadline_ns == 1'100'000'000,
        "lease exceeded original acquisition lifetime");
  check(session.turn(reverse, peer(1050, c::ControlPhase::active, 2), at(1050)).deadline_ns ==
            1'100'000'000,
        "fresh owner heartbeat renewed cached gear acquisition");
  check(!session.turn(reverse, peer(1100, c::ControlPhase::active, 3), at(1100)).requested,
        "stale reverse retained request");
  reverse.acquisition_deadline.reset();
  check(!session.turn(reverse, peer(1101), at(1101)).requested,
        "reverse without original deadline was accepted");
}
void clock_and_disabled() {
  c::ActivationSession session(config());
  session.turn(manual, peer(1000), at(1000));
  check(session.turn(manual, peer(999), at(999)).fault == c::ControllerFault::clock,
        "reversed controller clock accepted");
  session.turn({}, peer(1001), at(1001));
  check(!session.turn(manual, peer(1002), at(1002)).requested, "clock retirement recovered");
  auto disabled = config();
  disabled.enabled = false;
  c::ActivationSession off(disabled);
  check(!off.turn(manual, peer(1000), at(1000)).requested, "disabled controller activated");
  c::ActivationSession absent(config());
  check(!absent.turn(manual, peer(1001), at(1000)).requested, "future status accepted");
}
int main() try {
  acknowledgement_and_expiry();
  bounded_startup();
  owner_changes();
  acquisition_floor();
  clock_and_disabled();
  std::cout << "camera activation session: five bounded owner/acknowledgement/expiry/fault groups "
               "passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
