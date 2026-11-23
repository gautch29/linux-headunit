// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/output_fault.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
std::atomic<std::size_t> allocations{};
}
void* operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
void operator delete(void* value) noexcept {
  std::free(value);
}
void operator delete[](void* value) noexcept {
  std::free(value);
}
void operator delete(void* value, std::size_t) noexcept {
  std::free(value);
}
void operator delete[](void* value, std::size_t) noexcept {
  std::free(value);
}

using namespace headunit::theme::graphics;
namespace {
void check(bool valid, const char* reason) {
  if (!valid) throw std::runtime_error(reason);
}
template <class F>
void rejects(F&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid output fault input was accepted");
}
OutputFaultContext qualified(double pts = 200.25, std::uint64_t epoch = 7) {
  return {true, false, pts, {true, epoch}};
}
OutputFaultControlTurn running(double pts = 200.25) {
  return {pts, true, false};
}
void disabled_by_default() {
  OutputFaultIsolation policy;
  check(!policy.isolated() && policy.control_turn({true, 7}) == OutputFaultControlTurn{},
        "unfaulted policy invented a control presentation");
  check(policy.fault(qualified()) == OutputFaultAction::propagate && !policy.isolated() &&
            policy.control_turn({true, 7}) == OutputFaultControlTurn{},
        "default disabled policy isolated a display fault");
}
void initial_and_bootstrap_qualification() {
  OutputFaultIsolation policy(true);
  check(policy.fault({}) == OutputFaultAction::propagate && !policy.isolated(),
        "empty initial context isolated a panel");
  auto context = qualified();
  context.panel_started = false;
  check(policy.fault(context) == OutputFaultAction::propagate && !policy.isolated(),
        "bootstrap PTS started a never-started control VM");
  context = qualified();
  context.actual_presented_pts.reset();
  check(policy.fault(context) == OutputFaultAction::propagate && !policy.isolated(),
        "missing actual presentation was invented");
  context = qualified();
  context.clock.ready = false;
  check(policy.fault(context) == OutputFaultAction::propagate && !policy.isolated(),
        "unqualified clock allowed initial isolation");
  check(policy.control_turn({true, 7}) == OutputFaultControlTurn{},
        "rejected startup context bound a presentation timestamp");
  // A real blank bootstrap receipt is sufficient once tasks have genuinely
  // started; this policy never asserts that an instrument scene was visible.
  check(policy.fault(qualified(0, 0)) == OutputFaultAction::isolate &&
            policy.control_turn({true, 0}) == running(0),
        "valid epoch zero or actual blank receipt was rejected after startup");
}
void immutable_pts_and_duplicate_faults() {
  OutputFaultIsolation policy(true);
  const auto context = qualified();
  check(policy.fault(context) == OutputFaultAction::isolate && policy.isolated(),
        "qualified started output fault did not isolate");
  for (unsigned turn = 0; turn < 100; ++turn) {
    check(policy.control_turn({true, 7}) == running(),
          "control polling renewed or advanced retained PTS");
    check(policy.fault(context) == OutputFaultAction::isolate &&
              policy.control_turn({true, 7}) == running(),
          "duplicate output fault changed original presentation");
  }
  auto changed = context;
  changed.actual_presented_pts = 200.5;
  rejects([&] { policy.fault(changed); });
  changed = context;
  changed.actual_presented_pts = 200.;
  rejects([&] { policy.fault(changed); });
  changed = context;
  changed.actual_presented_pts.reset();
  rejects([&] { policy.fault(changed); });
  check(policy.control_turn({true, 7}) == running(),
        "invalid replacement or erasure changed retained PTS");
}
void clock_loss_is_sticky() {
  OutputFaultIsolation policy(true);
  const auto context = qualified();
  policy.fault(context);
  check(policy.control_turn({false, 7}) == OutputFaultControlTurn{} && policy.isolated(),
        "clock loss reused unqualified PTS or restored a renderer");
  for (unsigned turn = 0; turn < 8; ++turn) {
    check(policy.control_turn({true, 7}) == OutputFaultControlTurn{},
          "same-epoch relock revived stale actual presentation");
    check(policy.fault(context) == OutputFaultAction::isolate &&
              policy.control_turn({true, 7}) == OutputFaultControlTurn{},
          "duplicate fault requalified a withdrawn PTS");
  }
  auto lost = context;
  lost.clock.ready = false;
  check(policy.fault(lost) == OutputFaultAction::isolate,
        "duplicate qualified-history fault discarded isolation after loss");
  check(policy.control_turn({true, 8}) == OutputFaultControlTurn{},
        "new epoch revived a lost presentation");
}
void epoch_replacement_and_reconnect() {
  OutputFaultIsolation policy(true);
  auto context = qualified();
  policy.fault(context);
  check(policy.control_turn({true, 8}) == OutputFaultControlTurn{} && policy.isolated(),
        "replacement clock epoch kept old PTS eligible");
  context.clock.epoch = 8;
  check(policy.fault(context) == OutputFaultAction::isolate &&
            policy.control_turn({true, 8}) == OutputFaultControlTurn{},
        "duplicate fault rebound original PTS to a new epoch");
  rejects([&] { policy.control_turn({true, 7}); });
  check(policy.control_turn({true, 8}) == OutputFaultControlTurn{},
        "backwards-epoch rejection changed retired state");
  check(policy.control_turn({true, std::numeric_limits<std::uint64_t>::max()}) ==
            OutputFaultControlTurn{},
        "maximum clock epoch overflowed");
  rejects([&] { policy.control_turn({true, 8}); });
  // Reconnect must construct a new owner policy, with a genuinely new
  // presentation. The old policy has no reset/retry or PTS-replacement API.
  OutputFaultIsolation reconnected(true);
  check(reconnected.fault(qualified(201.75, 8)) == OutputFaultAction::isolate &&
            reconnected.control_turn({true, 8}) == running(201.75),
        "fresh connection could not isolate its own actual presentation");
}
void runtime_failures_escalate() {
  OutputFaultIsolation initial(true);
  auto broken = qualified();
  broken.runtime_faulted = true;
  check(initial.fault(broken) == OutputFaultAction::propagate && !initial.isolated(),
        "faulted initial Runtime was hidden as a display fault");
  check(initial.fault(qualified()) == OutputFaultAction::propagate && !initial.isolated() &&
            initial.control_turn({true, 7}) == OutputFaultControlTurn{{}, false, true},
        "Runtime failure was silently repaired by a later healthy flag");
  OutputFaultIsolation policy(true);
  policy.fault(qualified());
  check(policy.control_turn({true, 7}, true) == OutputFaultControlTurn{{}, false, true},
        "faulted isolated Runtime continued control tasks");
  check(policy.control_turn({true, 7}) == OutputFaultControlTurn{{}, false, true} &&
            policy.fault(qualified()) == OutputFaultAction::propagate,
        "Runtime escalation was not terminal for this policy");
  OutputFaultIsolation duplicate(true);
  duplicate.fault(qualified());
  check(duplicate.fault(broken) == OutputFaultAction::propagate &&
            duplicate.control_turn({true, 7}) == OutputFaultControlTurn{{}, false, true},
        "duplicate Runtime error was isolated");
}
void invalid_inputs_are_atomic() {
  OutputFaultIsolation pristine(true);
  for (const auto pts : std::array<double, 4>{-1., std::numeric_limits<double>::infinity(),
                                              -std::numeric_limits<double>::infinity(),
                                              std::numeric_limits<double>::quiet_NaN()}) {
    auto invalid = qualified(pts);
    invalid.runtime_faulted = true;
    rejects([&] { pristine.fault(invalid); });
    check(!pristine.isolated() && pristine.control_turn({true, 7}) == OutputFaultControlTurn{},
          "invalid initial PTS partially latched isolation or escalation");
  }
  pristine.fault(qualified());
  auto invalid = qualified(200.5, 8);
  invalid.clock.ready = false;
  invalid.runtime_faulted = true;
  rejects([&] { pristine.fault(invalid); });
  check(pristine.control_turn({true, 7}) == running(),
        "invalid duplicate changed epoch, clock eligibility, Runtime state or PTS");
  invalid = qualified();
  invalid.clock.epoch = 6;
  invalid.clock.ready = false;
  rejects([&] { pristine.fault(invalid); });
  rejects([&] { pristine.control_turn({false, 6}, true); });
  check(pristine.control_turn({true, 7}) == running(),
        "invalid clock input partially withdrew a healthy control VM");
  // Inputs passed to fault are snapshots; later caller changes cannot replace
  // the exact PTS or clock qualification copied into bounded private state.
  auto mutable_context = qualified();
  OutputFaultIsolation copied(true);
  copied.fault(mutable_context);
  mutable_context.actual_presented_pts = 500.;
  mutable_context.clock = {false, 99};
  check(copied.control_turn({true, 7}) == running(),
        "caller mutated the immutable fault context after acceptance");
}
void owner_thread_and_bounded_operations() {
  static_assert(!std::is_copy_constructible_v<OutputFaultIsolation>);
  static_assert(sizeof(OutputFaultIsolation) <= 128);
  OutputFaultIsolation policy(true);
  const auto context = qualified();
  policy.fault(context);
  bool fault_rejected = false, turn_rejected = false, status_rejected = false;
  std::thread other([&] {
    try {
      policy.fault(context);
    } catch (const std::invalid_argument&) {
      fault_rejected = true;
    }
    try {
      policy.control_turn({false, 8});
    } catch (const std::invalid_argument&) {
      turn_rejected = true;
    }
    try {
      policy.isolated();
    } catch (const std::invalid_argument&) {
      status_rejected = true;
    }
  });
  other.join();
  check(fault_rejected && turn_rejected && status_rejected &&
            policy.control_turn({true, 7}) == running(),
        "non-owner call mutated bounded fault policy");
  const auto before = allocations.load(std::memory_order_relaxed);
  bool correct = true;
  for (unsigned turn = 0; turn < 100; ++turn) {
    correct &= policy.fault(context) == OutputFaultAction::isolate;
    correct &= policy.control_turn({true, 7}) == running();
    correct &= policy.isolated();
  }
  correct &= policy.control_turn({false, 7}) == OutputFaultControlTurn{};
  correct &= policy.control_turn({true, 7}) == OutputFaultControlTurn{};
  const auto after = allocations.load(std::memory_order_relaxed);
  check(correct && before == after,
        "successful fault/control turns allocated or renewed retired PTS");
}
}  // namespace
int main() {
  try {
    disabled_by_default();
    initial_and_bootstrap_qualification();
    immutable_pts_and_duplicate_faults();
    clock_loss_is_sticky();
    epoch_replacement_and_reconnect();
    runtime_failures_escalate();
    invalid_inputs_are_atomic();
    owner_thread_and_bounded_operations();
    std::cout << "output fault tests passed (8 suites)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
