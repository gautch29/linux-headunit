// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_worker.hpp>
#include <headunit/frame_schedule.hpp>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <pthread.h>

namespace headunit::theme::script {
namespace {
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
constexpr std::size_t max_bytes = 2 * 1024 * 1024;
constexpr auto blocked_limit = std::chrono::seconds(5);
Clock::time_point next_frame(Clock::time_point started, Clock::time_point finished,
                             std::uint64_t period) {
  const auto previous = std::chrono::duration_cast<Nanoseconds>(started.time_since_epoch()).count();
  const auto current = std::chrono::duration_cast<Nanoseconds>(finished.time_since_epoch()).count();
  if (previous < 0 || current < 0) throw std::runtime_error("invalid worker monotonic clock");
  const auto next = headunit::next_owner_frame_deadline(previous, current, period);
  if (next > std::uint64_t(std::numeric_limits<Nanoseconds::rep>::max()))
    throw std::overflow_error("worker deadline overflow");
  return Clock::time_point(Nanoseconds(static_cast<Nanoseconds::rep>(next)));
}
struct Queued {
  DataMessage message;
  Clock::time_point enqueued;
};
std::size_t charge(const DataMessage& m) {
  return m.payload.size() + 32;
}
}  // namespace

struct UiWorker::Impl {
  const std::uint64_t stream_id;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::deque<Queued> input, output;
  std::size_t input_bytes = 0, output_bytes = 0, received = 0, sent = 0;
  State state = State::starting;
  bool stop = false, done = false;
  std::string failure;
  pthread_t thread{};
  bool joinable = false;

  explicit Impl(std::uint64_t id) : stream_id(id) {}
  bool stopping() {
    std::lock_guard lock(mutex);
    return stop;
  }
  void clear() {
    input.clear();
    output.clear();
    input_bytes = output_bytes = 0;
  }
  void run(Configuration& config) noexcept {
    bool fault = false;
    std::string reason;
    try {
      // Destruction as well as all calls occur on this thread. Initialization
      // never holds the queue mutex, so receiver polling and stop remain prompt.
      if (config.frame_interval_ns < 8333333 || config.frame_interval_ns > 66666667)
        throw std::invalid_argument("worker frame interval must be 15..120 Hz");
      if (config.feedback_poll_ns &&
          (config.feedback_poll_ns < 500000 ||
           config.feedback_poll_ns > config.frame_interval_ns / 2 || !config.presentation))
        throw std::invalid_argument("invalid worker feedback poll interval");
      Channel channel(std::move(config.resources), config.displays, config.limits);
      UiTransport transport(channel, stream_id, std::move(config.routes),
                            std::move(config.command_diagnostic), config.command_diagnostic_limit);
      if (config.configure) config.configure(channel);
      struct Startup {
        bool evaluated = false, started = false, was_deferred = false;
        std::optional<Clock::time_point> deadline;
      };
      std::array<Startup, 2> startup;
      const auto validate_turn = [](const DisplayTurn& turn, DisplayPhase phase) {
        if (turn.defer_startup && (phase != DisplayPhase::bootstrap || turn.allow_tasks ||
                                   turn.pts || turn.inline_task_ns))
          throw std::runtime_error("invalid deferred display startup turn");
      };
      const auto poll_startup = [&](Runtime& runtime, const Display& display, Startup& progress) {
        if (!progress.deadline) progress.deadline = Clock::now() + std::chrono::seconds(10);
        auto turn = config.presentation
                        ? config.presentation(runtime, display, DisplayPhase::bootstrap)
                        : DisplayTurn{};
        validate_turn(turn, DisplayPhase::bootstrap);
        if (turn.defer_startup) {
          progress.was_deferred = true;
          progress.deadline.reset();
          return turn;
        }
        if (Clock::now() >= *progress.deadline)
          throw std::runtime_error("UI display bootstrap timed out");
        if (turn.allow_tasks && progress.was_deferred && !turn.pts)
          throw std::runtime_error("deferred display startup needs a presented timestamp");
        if (turn.allow_tasks) progress.deadline.reset();
        return turn;
      };
      const auto startup_turn = [&](Runtime& runtime, const Display& display, Startup& progress) {
        while (!stopping()) {
          auto turn = poll_startup(runtime, display, progress);
          if (turn.allow_tasks || turn.defer_startup) return turn;
          std::unique_lock lock(mutex);
          if (!stop) wake.wait_for(lock, std::chrono::milliseconds(16));
        }
        return DisplayTurn{{}, false};
      };
      if (!stopping()) {
        transport.start();
        for (std::size_t i = 0; i < config.displays.size(); ++i) {
          if (stopping()) break;
          const auto& display = config.displays[i];
          auto& progress = startup[i];
          auto& runtime = channel.runtime(display.identifier);
          if (config.presentation) {
            const auto turn = startup_turn(runtime, display, progress);
            if (stopping()) break;
            if (turn.defer_startup) continue;
            runtime.tick(turn.pts);
          }
          runtime.evaluate("require 'main'", "receiver-theme-startup");
          progress.evaluated = true;
        }
        // Load every entry point before starting scheduled tasks. One display's
        // session timers must not run while another spends seconds in require.
        // An explicitly deferred entry point is the sole exception: it waits
        // for its own real bootstrap feedback without blocking another panel.
        for (std::size_t i = 0; i < config.displays.size(); ++i) {
          if (stopping()) break;
          const auto& display = config.displays[i];
          auto& progress = startup[i];
          if (!progress.evaluated) continue;
          auto& runtime = channel.runtime(display.identifier);
          const auto turn = startup_turn(runtime, display, progress);
          if (!stopping() && !turn.defer_startup) {
            runtime.start_tasks(turn.pts);
            progress.started = true;
          }
        }
      }
      {
        std::lock_guard lock(mutex);
        if (!stop) state = State::running;
      }
      auto next_tick = Clock::now();
      auto next_feedback = Clock::time_point::max();
      auto report_started = next_tick;
      auto report_next = report_started + std::chrono::seconds(5);
      FrameReport frame_report;
      std::optional<Clock::time_point> input_blocked;
      while (!stopping()) {
        // Deliver already received phone commands before script timers.
        for (unsigned count = 0; count < 8 && !stopping(); ++count) {
          std::optional<DataMessage> incoming;
          {
            std::lock_guard lock(mutex);
            if (!input.empty()) incoming = input.front().message;
          }
          if (!incoming) break;
          const auto result = transport.receive(stream_id, *incoming);
          if (result == TransportDelivery::backpressure) {
            if (!input_blocked) input_blocked = Clock::now();
            if (Clock::now() - *input_blocked >= blocked_limit)
              throw std::runtime_error("UI consumer stalled");
            break;
          }
          if (result != TransportDelivery::accepted)
            throw std::runtime_error("UI consumer rejected input (" + std::to_string(int(result)) +
                                     "): " + transport.rejection_reason());
          input_blocked.reset();
          std::lock_guard lock(mutex);
          if (stop) break;
          input_bytes -= charge(input.front().message);
          input.pop_front();
          ++received;
        }
        if (stopping()) break;
        if (Clock::now() >= next_tick) {
          const auto owner_started = Clock::now();
          for (std::size_t i = 0; i < config.displays.size(); ++i) {
            if (stopping()) break;
            const auto& display = config.displays[i];
            auto& runtime = channel.runtime(display.identifier);
            // Absence of a display owner remains nil, never CPU elapsed time.
            const auto presentation_started = Clock::now();
            auto& progress = startup[i];
            const bool bootstrapping = !progress.started;
            const auto turn =
                bootstrapping ? poll_startup(runtime, display, progress)
                              : (config.presentation
                                     ? config.presentation(runtime, display, DisplayPhase::frame)
                                     : DisplayTurn{});
            validate_turn(turn, bootstrapping ? DisplayPhase::bootstrap : DisplayPhase::frame);
            const auto task_started = Clock::now();
            if (!stopping() && turn.allow_tasks) {
              if (!bootstrapping)
                runtime.tick(turn.pts);
              else if (!progress.evaluated) {
                runtime.tick(turn.pts);
                runtime.evaluate("require 'main'", "receiver-theme-startup");
                progress.evaluated = true;
              } else {
                runtime.start_tasks(turn.pts);
                progress.started = true;
              }
            }
            if (config.frame_report) {
              const auto finished = Clock::now();
              const auto display_ns =
                  std::chrono::duration_cast<Nanoseconds>(task_started - presentation_started)
                      .count();
              if (turn.inline_task_ns > std::uint64_t(display_ns))
                throw std::runtime_error("invalid inline task timing");
              frame_report.presentation_ns[i] += display_ns - turn.inline_task_ns;
              frame_report.task_ns[i] +=
                  std::chrono::duration_cast<Nanoseconds>(finished - task_started).count() +
                  turn.inline_task_ns;
            }
          }
          // Both panels consume one serial owner's budget. A 22 ms turn must
          // not wait for the 33.3 ms fixed-phase slot as well: that rounds a
          // modest overrun down to 30 turns/s. Under budget, wait only for the
          // remainder of the period since this actual start. Over budget,
          // resume without accumulating debt or replaying script ticks.
          const auto owner_finished = Clock::now();
          const auto following =
              next_frame(owner_started, owner_finished, config.frame_interval_ns);
          if (config.frame_report) {
            const auto owner_ns =
                std::chrono::duration_cast<Nanoseconds>(owner_finished - owner_started).count();
            frame_report.owner_ns += owner_ns;
            frame_report.max_owner_ns =
                std::max(frame_report.max_owner_ns, std::uint64_t(owner_ns));
            ++frame_report.turns;
            if (owner_ns > 0)
              frame_report.missed_slots += (owner_ns - 1) / config.frame_interval_ns;
            if (owner_finished >= report_next) {
              frame_report.duration_ns =
                  std::chrono::duration_cast<Nanoseconds>(owner_finished - report_started).count();
              config.frame_report(frame_report);
              frame_report = {};
              report_started = owner_finished;
              report_next = owner_finished + std::chrono::seconds(5);
            }
          }
          next_tick = following;
          next_feedback =
              config.feedback_poll_ns
                  ? std::min(owner_finished + Nanoseconds(config.feedback_poll_ns), next_tick)
                  : Clock::time_point::max();
        }
        if (Clock::now() >= next_feedback && Clock::now() < next_tick) {
          for (std::size_t i = 0; i < config.displays.size(); ++i) {
            if (stopping()) break;
            // Only ordinary frame turns may evaluate or start a deferred VM.
            // Feedback remains a receipt pump, never an extra scheduler turn.
            if (!startup[i].started) continue;
            const auto& display = config.displays[i];
            const auto turn = config.presentation(channel.runtime(display.identifier), display,
                                                  DisplayPhase::feedback);
            validate_turn(turn, DisplayPhase::feedback);
          }
          next_feedback = std::min(Clock::now() + Nanoseconds(config.feedback_poll_ns), next_tick);
        }
        for (unsigned count = 0; count < 32 && !stopping(); ++count) {
          const auto* pending = transport.pending();
          if (!pending) break;
          const auto size = charge(*pending);
          if (size > max_bytes) throw std::runtime_error("UI output too large");
          std::lock_guard lock(mutex);
          if (stop || output.size() >= 32 || size > max_bytes - output_bytes) break;
          output.push_back({*pending, Clock::now()});
          output_bytes += size;
          // Acceptance here transfers ownership to the ordered transport queue.
          // Later VM resets cannot recall accepted bytes, just as on a socket.
          transport.accepted();
        }
        std::unique_lock lock(mutex);
        if (!output.empty() && Clock::now() - output.front().enqueued >= blocked_limit)
          throw std::runtime_error("UI transport stalled");
        // Backpressured input must not cause a busy spin. Poll at most once per
        // scheduler turn unless a new input/stop notification wakes this wait.
        if (!stop) wake.wait_until(lock, std::min(next_tick, next_feedback));
      }
      transport.close();
    } catch (const std::exception& e) {
      fault = true;
      try {
        reason = std::string_view(e.what()).substr(0, 512);
      } catch (...) {
      }
    } catch (...) {
      fault = true;
    }
    // A socket EOF can request stop while GPU owners are being destroyed.
    // Preserve the original exception before that cleanup/stop race.
    if (fault) {
      std::lock_guard lock(mutex);
      failure = std::move(reason);
    }
    // Renderer/resource captures must also die on their owner before finished()
    // permits the network supervisor to reap this worker without a long join.
    config = Configuration{};
    std::lock_guard lock(mutex);
    clear();
    state = fault && !stop ? State::faulted : State::stopped;
    done = true;
    wake.notify_all();
  }
};

UiWorker::UiWorker(std::uint64_t stream_id, Configuration config)
    : impl_(std::make_unique<Impl>(stream_id)) {
  if (!stream_id || !config.resources || config.displays.empty() || config.displays.size() > 2)
    throw std::invalid_argument("invalid UI worker configuration");
  // macOS defaults secondary threads to only 512 KiB, insufficient for the
  // instrumented Luau interpreter. Give this owner an explicit bounded stack,
  // matching the conventional Linux/main-thread size on both supported hosts.
  struct Start {
    Impl* impl;
    Configuration configuration;
  };
  auto start = std::make_unique<Start>(Start{impl_.get(), {}});
  // std::function moves may leave a small target in the source (libc++).
  // Transfer into one thread-owned configuration, then run it by reference so
  // config={} really destroys worker captures before publishing finished().
  std::swap(start->configuration, config);
  pthread_attr_t attributes;
  int error = pthread_attr_init(&attributes);
  if (error) throw std::system_error(error, std::generic_category(), "UI thread attributes");
  error = pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
  if (!error)
    error = pthread_create(
        &impl_->thread, &attributes,
        [](void* raw) -> void* {
          std::unique_ptr<Start> task(static_cast<Start*>(raw));
          task->impl->run(task->configuration);
          return nullptr;
        },
        start.get());
  pthread_attr_destroy(&attributes);
  if (error) throw std::system_error(error, std::generic_category(), "UI thread creation");
  (void)start.release();
  impl_->joinable = true;
}
UiWorker::~UiWorker() {
  request_stop();
  if (impl_->joinable && pthread_join(impl_->thread, nullptr) != 0) std::terminate();
}
bool UiWorker::receive(std::uint64_t stream_id, const DataMessage& message) {
  // Check before addition so adversarial lengths cannot overflow accounting.
  if (stream_id != impl_->stream_id || message.payload.size() > 512 * 1024) return false;
  std::lock_guard lock(impl_->mutex);
  if (impl_->stop || impl_->done || impl_->input.size() >= 64 ||
      charge(message) > max_bytes - impl_->input_bytes)
    return false;
  impl_->input.push_back({message, Clock::now()});
  impl_->input_bytes += charge(message);
  impl_->wake.notify_one();
  return true;
}
bool UiWorker::send_pending(const std::function<bool(const DataMessage&)>& send) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->stop || impl_->done || impl_->output.empty()) return false;
  if (!send(impl_->output.front().message)) return false;
  impl_->output_bytes -= charge(impl_->output.front().message);
  impl_->output.pop_front();
  ++impl_->sent;
  impl_->wake.notify_one();
  return true;
}
UiWorker::Status UiWorker::status() const {
  std::lock_guard lock(impl_->mutex);
  return {impl_->state, impl_->received, impl_->sent, impl_->input.size(), impl_->output.size()};
}
std::string UiWorker::failure_reason() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->failure;
}
void UiWorker::request_stop() {
  std::lock_guard lock(impl_->mutex);
  impl_->stop = true;
  if (!impl_->done) impl_->state = State::stopping;
  impl_->clear();
  impl_->wake.notify_one();
}
bool UiWorker::finished() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->done;
}
}  // namespace headunit::theme::script
