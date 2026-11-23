// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/ui_transport.hpp>
#include <chrono>
#include <array>
#include <memory>
#include <headunit/frame_schedule.hpp>

namespace headunit::theme::script {

// Connection-scoped script owner. No Runtime or scene pointer crosses threads.
// ResourceReader must own its immutable, verified bundle and must not be used
// concurrently elsewhere unless it is thread-safe. Configure runs on this owner.
class UiWorker {
 public:
  enum class DisplayPhase { bootstrap, frame, feedback };
  struct DisplayTurn {
    std::optional<double> pts;
    bool allow_tasks = true;
    // When the display executed this exact script tick while its GPU fence
    // was pending, the worker must not repeat it. This is owner wall time for
    // diagnostics only; it never supplies or changes the script timestamp.
    std::uint64_t inline_task_ns = 0;
    // Explicit owner policy for a panel hidden before its first task turn.
    // Legal only in bootstrap with no PTS, tasks or inline work. Other panels
    // may start while this panel remains deferred. Once deferral ends, its
    // ordinary ten-second bootstrap budget resumes and an allowed turn must
    // carry a genuinely presented PTS before evaluation/task startup.
    bool defer_startup = false;
  };
  // Diagnostic wall times from the single script/render owner. Presentation
  // includes display polling, video staging and any GPU draw; task is the Lua
  // tick that follows it. These are not phone PTS or compositor timestamps.
  struct FrameReport {
    // missed_slots counts extra period budgets consumed by ordinary turns,
    // not dropped phone frames or compositor presentations.
    std::uint64_t duration_ns = 0, turns = 0, missed_slots = 0;
    std::uint64_t owner_ns = 0, max_owner_ns = 0;
    std::array<std::uint64_t, 2> presentation_ns{}, task_ns{};
  };
  struct Configuration {
    ResourceReader resources;
    std::vector<Display> displays;
    std::vector<DisplayRoute> routes;
    Limits limits;
    std::uint64_t frame_interval_ns = headunit::ui_frame_period_ns;
    // Experimental owner-thread receipt cadence. Extra calls may render a
    // scene after real feedback but never advance scripts or their clock.
    // Zero disables this path.
    std::uint64_t feedback_poll_ns = 0;
    std::function<void(Channel&)> configure;
    // Optional display-owner hook, called only on this worker before startup
    // and each script turn. Supply the last actual compositor receipt mapped
    // through the connection clock, never wall time or submitted-frame time.
    // It may pump bounded rendering/feedback work; no receiver locks are held.
    // Return allow_tasks=false during bootstrap or clock/visibility suspension.
    // The worker keeps servicing transport while withholding script turns.
    // Bootstrap must consume its initial receipt without leaving a scene frame
    // pending while another display loads. Frame phase allows normal rendering.
    // Explicit defer_startup keeps an unstarted panel in bootstrap while other
    // panels run. Its routed input still has the normal bounded Runtime queue;
    // deferral neither acknowledges nor drops commands to avoid backpressure.
    std::function<DisplayTurn(Runtime&, const Display&, DisplayPhase)> presentation;
    // Optional bounded five-second report on this owner thread. The callback
    // must not access the worker or block on receiver/network state.
    std::function<void(const FrameReport&)> frame_report;
    // Same owner and default-off diagnostic route; queue acceptance only,
    // distinct from encrypted socket transfer, peer receipt or VM execution.
    UiCommandDiagnosticSink command_diagnostic;
    std::size_t command_diagnostic_limit = 256;
  };
  // Running means the owner loop is active; explicitly deferred panels may
  // still await their first qualified startup turn.
  enum class State { starting, running, stopping, stopped, faulted };
  struct Status {
    State state;
    std::size_t received, sent, queued_input, queued_output;
  };
  UiWorker(std::uint64_t stream_id, Configuration);
  ~UiWorker();  // Joins: reap only after finished() in a network polling loop.
  UiWorker(const UiWorker&) = delete;
  UiWorker& operator=(const UiWorker&) = delete;
  bool receive(std::uint64_t stream_id, const DataMessage&);
  // Callback must be bounded/nonblocking and not reenter this worker. False
  // retains exactly the same transport-owned message, with no duplicate enqueue.
  bool send_pending(const std::function<bool(const DataMessage&)>&);
  Status status() const;
  std::string failure_reason() const;  // Bounded private diagnostic; may name asset resources.
  void request_stop();                 // Nonblocking, terminal, immediately discards both queues.
  bool finished() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::script
