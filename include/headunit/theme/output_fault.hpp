// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <optional>
#include <thread>

namespace headunit::theme::graphics {

// Qualification comes from the paired receiver clock. Epoch zero is a valid
// initial timeline; readiness alone never asserts that a frame was presented.
struct OutputClockQualification {
  bool ready = false;
  std::uint64_t epoch = 0;
  bool operator==(const OutputClockQualification&) const = default;
};
struct OutputFaultContext {
  bool panel_started = false;
  bool runtime_faulted = false;
  // The last genuinely accepted presentation PTS, not a submitted scene, wall
  // time, projected network time, or a timestamp invented for this failure.
  std::optional<double> actual_presented_pts{};
  OutputClockQualification clock{};
};
enum class OutputFaultAction { propagate, isolate };
struct OutputFaultControlTurn {
  std::optional<double> pts{};
  bool allow_tasks = false;
  bool escalate = false;
  bool operator==(const OutputFaultControlTurn&) const = default;
};

// Disabled by default. The owner calls fault only for a classified output-only
// failure, after withdrawing its failed surface and retaining transport state.
// This policy neither renders nor consumes/acknowledges phone messages. A
// faulted Runtime cannot be repaired or hidden behind output-only containment.
// Bootstrap may have a real blank-frame PTS; tasks must already have started
// before it is sufficient for control-only turns. No gauge visibility is claimed.
class OutputFaultIsolation {
 public:
  explicit OutputFaultIsolation(bool enabled = false);
  OutputFaultIsolation(const OutputFaultIsolation&) = delete;
  OutputFaultIsolation& operator=(const OutputFaultIsolation&) = delete;
  OutputFaultAction fault(const OutputFaultContext&);
  // Only a previously isolated, healthy Runtime may execute a control turn at
  // the retained actual PTS. No call advances or renews that PTS. Clock loss or
  // an epoch replacement irreversibly retires it for this policy's lifetime;
  // relock is insufficient without a fresh connection/policy and presentation.
  OutputFaultControlTurn control_turn(OutputClockQualification, bool runtime_faulted = false);
  bool isolated() const;

 private:
  struct State {
    bool isolated = false, eligible = false, escalated = false;
    std::optional<double> actual_pts{};
    std::uint64_t presented_epoch = 0, latest_epoch = 0;
  };
  const bool enabled_;
  const std::thread::id owner_;
  State state_{};
  void check_owner() const;
  void check_clock(OutputClockQualification) const;
  static void qualify(State&, OutputClockQualification);
};

}  // namespace headunit::theme::graphics
