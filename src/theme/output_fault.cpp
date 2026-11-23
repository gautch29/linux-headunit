// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/output_fault.hpp>

#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace headunit::theme::graphics {
namespace {
void check(bool valid, const char* reason) {
  if (!valid) throw std::invalid_argument(reason);
}
}  // namespace
OutputFaultIsolation::OutputFaultIsolation(bool enabled)
    : enabled_(enabled), owner_(std::this_thread::get_id()) {
  static_assert(std::is_trivially_copyable_v<State>);
  static_assert(std::is_nothrow_copy_assignable_v<State>);
}
void OutputFaultIsolation::check_owner() const {
  check(owner_ == std::this_thread::get_id(), "output fault policy used by another owner thread");
}
void OutputFaultIsolation::check_clock(OutputClockQualification clock) const {
  check(!state_.isolated || clock.epoch >= state_.latest_epoch,
        "output fault clock epoch went backwards");
}
void OutputFaultIsolation::qualify(State& state, OutputClockQualification clock) {
  state.latest_epoch = clock.epoch;
  if (!clock.ready || clock.epoch != state.presented_epoch) state.eligible = false;
}
OutputFaultAction OutputFaultIsolation::fault(const OutputFaultContext& context) {
  check_owner();
  check_clock(context.clock);
  check(!context.actual_presented_pts ||
            (std::isfinite(*context.actual_presented_pts) && *context.actual_presented_pts >= 0),
        "invalid actual presentation PTS for output fault");
  auto candidate = state_;
  if (candidate.isolated) {
    // A repeated fault can report the original presentation, but never replace
    // it with a newer projected/submitted timestamp or erase its history.
    check(context.actual_presented_pts == candidate.actual_pts,
          "output fault changed its retained actual presentation PTS");
    qualify(candidate, context.clock);
  }
  if (context.runtime_faulted) candidate.escalated = true;
  if (!enabled_ || !context.panel_started || candidate.escalated) {
    state_ = candidate;
    return OutputFaultAction::propagate;
  }
  if (candidate.isolated) {
    state_ = candidate;
    return OutputFaultAction::isolate;
  }
  if (!context.clock.ready || !context.actual_presented_pts) return OutputFaultAction::propagate;
  candidate.isolated = true;
  candidate.eligible = true;
  candidate.actual_pts = context.actual_presented_pts;
  candidate.presented_epoch = candidate.latest_epoch = context.clock.epoch;
  state_ = candidate;
  return OutputFaultAction::isolate;
}
OutputFaultControlTurn OutputFaultIsolation::control_turn(OutputClockQualification clock,
                                                          bool runtime_faulted) {
  check_owner();
  check_clock(clock);
  auto candidate = state_;
  if (candidate.isolated) qualify(candidate, clock);
  if (runtime_faulted) candidate.escalated = true;
  OutputFaultControlTurn result;
  result.escalate = candidate.escalated;
  if (candidate.isolated && candidate.eligible && !candidate.escalated) {
    result.pts = candidate.actual_pts;
    result.allow_tasks = true;
  }
  state_ = candidate;
  return result;
}
bool OutputFaultIsolation::isolated() const {
  check_owner();
  return state_.isolated;
}

}  // namespace headunit::theme::graphics
