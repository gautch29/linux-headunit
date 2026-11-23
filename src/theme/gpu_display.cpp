// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_display.hpp>
#include <headunit/frame_schedule.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <headunit/theme/output_fault.hpp>
#include <chrono>
#include <algorithm>
#include <deque>
#include <stdexcept>
#include <set>
#include <limits>
#include <array>
#include <exception>
#include <type_traits>
#include <utility>

namespace headunit::theme::graphics {
namespace {
std::uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
bool stale_preview_receipt(const PresentationReceipt& receipt) {
  if (!receipt.presented || !receipt.correlation || receipt.nanoseconds >= 1000000000 ||
      receipt.seconds >
          (std::numeric_limits<std::uint64_t>::max() - receipt.nanoseconds) / 1000000000)
    return false;  // Not an age-only case; never hide malformed timing as idle.
  const auto presented = receipt.seconds * 1000000000 + receipt.nanoseconds;
  const auto sampled = receipt.correlation->source_ns;
  return sampled >= presented &&
         sampled - presented > video::PresentationLimits{}.max_feedback_age_ns;
}
bool uses_stream(const std::shared_ptr<const Layer>& root,
                 const std::shared_ptr<video::Stream>& input) {
  std::set<const Layer*> visited;
  std::vector<std::shared_ptr<const Layer>> pending{root};
  while (!pending.empty()) {
    auto layer = std::move(pending.back());
    pending.pop_back();
    if (!layer || layer->hidden || layer->opacity == 0 || !layer->transform.inverse()) continue;
    if (!visited.insert(layer.get()).second) continue;
    if (visited.size() > 4096 || pending.size() + layer->children.size() > 4096)
      throw std::runtime_error("native input scene exceeds traversal limit");
    if (layer->kind == Kind::video && layer->video && layer->video->active() &&
        layer->video->stream == input)
      return true;
    pending.push_back(layer->mask);
    pending.insert(pending.end(), layer->children.begin(), layer->children.end());
  }
  return false;
}
// An exception is containable only while executing a classified output call.
// In particular, a clock/input/task failure after that call is never relabelled
// as a render fault merely because this turn previously submitted GPU work.
template <class Operation>
std::invoke_result_t<Operation> output_operation(bool& failed, Operation&& operation) {
  failed = true;
  if constexpr (std::is_void_v<std::invoke_result_t<Operation>>) {
    std::forward<Operation>(operation)();
    failed = false;
  } else {
    auto result = std::forward<Operation>(operation)();
    failed = false;
    return result;
  }
}
}  // namespace
struct GpuDisplay::Impl {
  enum class SuspendReason { clock_epoch, clock_unavailable, owner_gap, feedback_delay };
  script::Runtime& runtime;
  ClockSource source;
  GpuDisplayOptions options;
  OutputFaultIsolation output_fault;
  std::unique_ptr<GpuCompositor> gpu;
  video::ReceiverClock clock;
  video::PresentationClock presentation;
  GpuDisplayStats stats;
  Size output_size;
  std::optional<double> pts;
  std::optional<double> withdrawn_pts;
  bool surface_claimed = false;
  std::optional<script::VerificationReport> report;
  struct PendingScene {
    std::uint64_t epoch = 0, submitted_at = 0, draw_ordinal = 0, scene_generation = 0;
    bool valid = false, scene = false;
    std::uint8_t staged_inputs = 0;
    std::array<std::uint64_t, 4> source_tokens{};  // Actual source drawn; zero means no frame.
    std::uint64_t withdrawal_sequence = 0;
  };
  std::deque<PendingScene> pending;
  std::uint64_t epoch = 0, next_draw = 0, last_turn = 0;
  std::uint64_t last_scene_present_ns = 0;
  std::uint64_t first_ready_wall_ns = 0, last_qualified_scene_wall_ns = 0;
  bool ready_before = false;
  bool outage_submitted = false, preview_waiting = false, scene_visible = false,
       bootstrap_idle = false;
  // Visibility is separate from clock/receipt validity and source completion.
  // An older scene may really present after its root vanished; its timing and
  // leases remain genuine, but it cannot qualify this new scene generation.
  std::uint64_t scene_generation = 1;
  bool scene_available = false, underlay_submitted = false;
  bool terminal_fault = false;

  void invalidate_scene() {
    if (scene_generation == std::numeric_limits<std::uint64_t>::max())
      throw std::runtime_error("GPU scene generation exhausted");
    ++scene_generation;
    scene_visible = false;
    scene_available = false;
    underlay_submitted = false;
  }
  void observe_scene(bool available) {
    if (!options.reveal_underlay_while_waiting) return;
    if (scene_available && !available) {
      invalidate_scene();
      if (options.cancel_touches) options.cancel_touches();
    }
    scene_available = available;
    if (!available) scene_visible = false;
  }

  Impl(script::Runtime& r, const script::Display& display, ClockSource s, GpuDisplayOptions o)
      : runtime(r),
        source(std::move(s)),
        options(std::move(o)),
        output_fault(options.isolate_output_faults) {
    if (!source || options.frame_interval_ns < 1000000 || options.frame_interval_ns > 1000000000 ||
        !options.scene_completion_timeout_ns || options.scene_completion_timeout_ns > 2000000000)
      throw std::invalid_argument("invalid GPU display policy");
    if (options.scene_stall_timeout_ns && (options.scene_stall_timeout_ns < 1000000000 ||
                                           options.scene_stall_timeout_ns > 30000000000ULL))
      throw std::invalid_argument("invalid scene stall timeout");
    if (options.inputs.size() > 4) throw std::invalid_argument("too many display video inputs");
    if (options.input_withdrawal &&
        (options.input_withdrawal->input_index >= options.inputs.size() ||
         !options.input_withdrawal->pending || !options.input_withdrawal->presented))
      throw std::invalid_argument("invalid input withdrawal binding");
    if (options.surface_lease &&
        (!options.surface_lease->acquire || !options.surface_lease->release))
      throw std::invalid_argument("incomplete display surface lease policy");
    std::set<const video::Stream*> streams;
    for (const auto& input : options.inputs)
      if (!input || !input->stream() || input->stream()->closed() ||
          !streams.insert(input->stream().get()).second)
        throw std::invalid_argument("invalid or duplicate display video input");
    output_size = {double(display.width), double(display.height)};
    if (options.asset_canvas.width == 0 && options.asset_canvas.height == 0)
      options.asset_canvas = output_size;
    (void)map_scene_canvas({}, options.asset_canvas, output_size);
    if (!options.surface_lease || options.surface_lease->acquire())
      create_surface();
    else {
      stats.surface_withdrawn = true;
      runtime.set_foregrounded(false);
    }
    try {
      runtime.bind_verification_reporter([this](const script::VerificationReport& value) {
        report = value;  // Bounded declarations only. No independent pixel verification.
        ++stats.verification_declarations;
      });
    } catch (...) {
      gpu.reset();
      release_surface();
      throw;
    }
  }
  ~Impl() {
    if (options.cancel_touches) options.cancel_touches();
    gpu.reset();
    release_surface();  // No Runtime access during destruction.
  }
  void release_surface() {
    if (surface_claimed) {
      options.surface_lease->release();
      surface_claimed = false;
    }
  }
  void create_surface() {
    surface_claimed = bool(options.surface_lease);
    try {
      gpu = std::make_unique<GpuCompositor>(unsigned(output_size.width),
                                            unsigned(output_size.height), options.window);
    } catch (...) {
      release_surface();
      throw;
    }
    if (stats.surface_withdrawn) ++stats.surface_restorations;
    stats.surface_withdrawn = false;
  }
  void withdraw_surface() {
    if (!gpu) return;
    if (options.cancel_touches) options.cancel_touches();
    runtime.set_foregrounded(false);
    // A brief restore may not have qualified a new receipt yet. Preserve the
    // last actual PTS across that empty generation; never derive one from CPU time.
    if (pts) withdrawn_pts = pts;
    pts.reset();
    // Retire GPU imports/window first, then discard source preparations and
    // release the shared lease. A native owner can never map over this window.
    gpu.reset();
    for (const auto& input : options.inputs)
      input->suspend();
    pending.clear();
    presentation.resume_after_idle();
    last_turn = last_scene_present_ns = first_ready_wall_ns = last_qualified_scene_wall_ns =
        next_draw = 0;
    outage_submitted = preview_waiting = scene_visible = false;
    bootstrap_idle = true;
    if (options.reveal_underlay_while_waiting) invalidate_scene();
    stats.waiting = true;
    stats.surface_withdrawn = true;
    ++stats.surface_withdrawals;
    release_surface();
  }
  void suspend(SuspendReason reason) {
    if (options.cancel_touches) options.cancel_touches();
    if (gpu && options.touches) (void)gpu->take_touches(false);
    pts.reset();
    withdrawn_pts.reset();
    last_scene_present_ns = 0;
    for (auto& scene : pending) {
      scene.valid = false;
      scene.staged_inputs = 0;
    }
    outage_submitted = false;
    scene_visible = false;
    if (options.reveal_underlay_while_waiting) invalidate_scene();
    for (const auto& input : options.inputs)
      input->suspend();
    runtime.set_foregrounded(false);
    presentation.resume_after_idle();
    stats.waiting = true;
    ++stats.suspensions;
    switch (reason) {
      case SuspendReason::clock_epoch:
        ++stats.clock_epoch_suspensions;
        break;
      case SuspendReason::clock_unavailable:
        ++stats.clock_unavailable_suspensions;
        break;
      case SuspendReason::owner_gap:
        ++stats.owner_gap_suspensions;
        break;
      case SuspendReason::feedback_delay:
        ++stats.feedback_delay_suspensions;
        break;
    }
  }
  bool refresh() {
    clock.update(source());
    const auto current_epoch = clock.clock().suspension_epoch();
    const bool ready = clock.clock().ready(now_ns());
    // A loss of readiness often also advances the clock epoch. Report the
    // actual blackout cause first; reserve epoch for a still-ready timeline
    // replacement.
    if (!ready && ready_before)
      suspend(SuspendReason::clock_unavailable);
    else if (current_epoch != epoch)
      suspend(SuspendReason::clock_epoch);
    epoch = current_epoch;
    ready_before = ready;
    return ready;
  }
  void submit(bool& output_failure, bool valid, bool scene, std::uint8_t staged_inputs = 0,
              std::array<std::uint64_t, 4> source_tokens = {}, std::uint64_t generation = 0,
              std::uint64_t withdrawal_sequence = 0) {
    const auto began = now_ns();
    const auto content = !scene && options.reveal_underlay_while_waiting
                             ? WindowContent::reveal_underlay
                             : WindowContent::rendered_scene;
    output_operation(output_failure, [&] { gpu->submit(++stats.submissions, nullptr, content); });
    if (content == WindowContent::reveal_underlay) {
      ++stats.underlay_submissions;
      underlay_submitted = true;
    } else if (scene)
      underlay_submitted = false;
    if (scene) {
      const auto elapsed = now_ns() - began;
      ++stats.scene_submissions;
      stats.submit_ns += elapsed;
      stats.max_submit_ns = std::max(stats.max_submit_ns, elapsed);
    }
    pending.push_back({epoch, began, scene ? stats.scene_compositions : 0, generation, valid, scene,
                       staged_inputs, source_tokens, withdrawal_sequence});
  }
  void retire_failed_output() {
    stats.faulted = true;
    stats.waiting = true;
    const bool had_surface = bool(gpu);
    // GPU leases must retire before any staged source is discarded. Window
    // destruction reveals the independent backup; driver hangs are not bounded
    // here because GPU destruction may wait for outstanding work to complete.
    gpu.reset();
    pending.clear();
    pts.reset();
    withdrawn_pts.reset();
    report.reset();
    scene_visible = outage_submitted = preview_waiting = false;
    if (options.reveal_underlay_while_waiting) invalidate_scene();
    last_turn = last_scene_present_ns = first_ready_wall_ns = last_qualified_scene_wall_ns =
        next_draw = 0;
    stats.surface_withdrawn = true;
    if (had_surface) ++stats.surface_withdrawals;
    // Complete every cleanup attempt, but never acknowledge isolation if any
    // callback, Runtime or input cleanup failed. Keep the input objects/queues
    // alive so subsequent control-only turns can drain phone deltas.
    std::exception_ptr cleanup_error;
    const auto attempt = [&](auto&& operation) {
      try {
        operation();
      } catch (...) {
        if (!cleanup_error) cleanup_error = std::current_exception();
      }
    };
    if (options.cancel_touches) attempt([&] { options.cancel_touches(); });
    // A faulted VM rejects all state mutation already. Preserve its original
    // exception detail instead of replacing it with that redundant rejection.
    attempt([&] {
      if (!runtime.stats().faulted) runtime.set_foregrounded(false);
    });
    for (const auto& input : options.inputs)
      attempt([&] { input->suspend(); });
    attempt([&] { release_surface(); });
    if (cleanup_error) std::rethrow_exception(cleanup_error);
  }
  script::UiWorker::DisplayTurn isolated_turn(script::UiWorker::DisplayPhase phase) {
    try {
      // A withdrawn output has no receipts to accept or scene to retry. Keep
      // sampling the genuine phone clock, never projecting a new control PTS.
      clock.update(source());
      epoch = clock.clock().suspension_epoch();
      ready_before = clock.clock().ready(now_ns());
      const auto control =
          output_fault.control_turn({ready_before, epoch}, runtime.stats().faulted);
      if (control.escalate) throw std::runtime_error("isolated GPU output Runtime is faulted");
      for (const auto& input : options.inputs)
        input->pump(nullptr);
      if (phase == script::UiWorker::DisplayPhase::bootstrap) return {{}, false, 0, true};
      return {control.pts, control.allow_tasks && phase == script::UiWorker::DisplayPhase::frame};
    } catch (...) {
      // A failed clock query or input drain must not leave a usable retained
      // timestamp behind even if the caller catches the terminal exception.
      terminal_fault = true;
      (void)output_fault.control_turn({false, clock.clock().suspension_epoch()},
                                      runtime.stats().faulted);
      throw;
    }
  }
  script::UiWorker::DisplayTurn turn(script::UiWorker::DisplayPhase phase) {
    if (terminal_fault) throw std::runtime_error("GPU display is terminally faulted");
    if (output_fault.isolated()) return isolated_turn(phase);
    if (stats.faulted) throw std::runtime_error("GPU display is faulted");
    bool output_failure = false;
    bool completed_inline_tick = false;
    std::uint64_t completed_inline_task_ns = 0;
    try {
      if (options.surface_lease) {
        if (!options.surface_lease->acquire()) {
          withdraw_surface();
          stats.surface_withdrawn = true;
          stats.waiting = true;
          // Surface ownership does not suspend the phone's network clock.
          // Continue sampling it so a long camera view cannot expire continuity.
          const bool ready = refresh();
          // Continue draining compressed deltas without making decoded output
          // eligible. Keep started control tasks on their last actual PTS;
          // a never-started panel explicitly defers its bootstrap.
          for (const auto& input : options.inputs)
            input->pump(nullptr);
          if (phase == script::UiWorker::DisplayPhase::bootstrap) return {{}, false, 0, true};
          return {
              ready ? withdrawn_pts : std::nullopt,
              ready && phase == script::UiWorker::DisplayPhase::frame && withdrawn_pts.has_value()};
        }
        if (!gpu) create_surface();
      }
      // Bootstrap deliberately does not submit scenes while other entry points
      // load. Regular bootstrap calls do not imply continuous presentations.
      // Resume from a fresh real correlation at the first ordinary frame,
      // retaining token/domain/last-presented guards and all receipt age limits.
      if (phase == script::UiWorker::DisplayPhase::bootstrap)
        bootstrap_idle = true;
      else if (bootstrap_idle) {
        presentation.resume_after_idle();
        bootstrap_idle = false;
      }
      const auto time = now_ns();
      // Theme/module loading may pause this owner. Preserve timeline guards,
      // but require a fresh presentation after a long scheduling gap.
      if (last_turn && time - last_turn > 1000000000) suspend(SuspendReason::owner_gap);
      last_turn = time;
      bool ready = refresh();
      if (gpu->preview_feedback_delayed() && !preview_waiting) {
        suspend(SuspendReason::feedback_delay);
        preview_waiting = true;
      }
      // Observe root loss before polling an older receipt. Never requalify that
      // scene merely because it still has a valid timestamp/source lease.
      observe_scene(ready && !preview_waiting && bool(runtime.scene_root()));
      if (const auto receipt =
              output_operation(output_failure, [&] { return gpu->poll_presentation(); })) {
        if (pending.empty() || receipt->token != stats.submissions - pending.size() + 1)
          throw std::runtime_error("GPU scene receipt order changed");
        const auto submission = pending.front();
        pending.pop_front();
        if (options.reveal_underlay_while_waiting && !submission.scene && !receipt->presented) {
          // Submission is not visibility evidence. Retry a discarded clear
          // through the same bounded real-feedback queue; never renew the
          // qualified-scene stall budget or invent a bootstrap timestamp.
          underlay_submitted = false;
          outage_submitted = false;
        }
        const auto submitted_at = submission.submitted_at;
        if (submitted_at && receipt->received_ns >= submitted_at) {
          const auto latency = receipt->received_ns - submitted_at;
          ++stats.feedback_receipts;
          stats.feedback_ns += latency;
          stats.max_feedback_ns = std::max(stats.max_feedback_ns, latency);
          stats.last_refresh_ns = receipt->refresh_ns;
        }
        bool presented = false;
        std::optional<video::InputPresentationReceipt> input_receipt;
        std::optional<InputWithdrawalReceipt> withdrawal;
        if (!submission.valid || !ready || submission.epoch != epoch || preview_waiting) {
          ++stats.ignored_receipts;
        } else if (const auto mapped = presentation.accept(*receipt, clock.clock())) {
          pts = mapped->pts_seconds;
          withdrawn_pts = pts;
          ++stats.presentations;
          if (submission.scene) {
            ++stats.scene_presentations;
            presented = true;
            input_receipt = video::InputPresentationReceipt{submission.epoch, epoch, submitted_at,
                                                            *receipt, *mapped};
            stats.scene_zero_copy_presentations +=
                (receipt->flags & 8u) != 0;  // wp_presentation ZERO_COPY.
            if (submission.withdrawal_sequence && submission.scene_generation == scene_generation &&
                runtime.scene_root())
              withdrawal = InputWithdrawalReceipt{submission.withdrawal_sequence,
                                                  submission.epoch,
                                                  epoch,
                                                  submission.draw_ordinal,
                                                  submitted_at,
                                                  mapped->monotonic_ns,
                                                  receipt->received_ns};
            if (!options.reveal_underlay_while_waiting ||
                (scene_available && submission.scene_generation == scene_generation)) {
              scene_visible = true;
              last_qualified_scene_wall_ns = now_ns();
            }
            if (last_scene_present_ns && mapped->monotonic_ns >= last_scene_present_ns) {
              const auto gap = mapped->monotonic_ns - last_scene_present_ns;
              stats.max_scene_present_gap_ns = std::max(stats.max_scene_present_gap_ns, gap);
              stats.scene_gaps_over_20ms += gap > 20000000;
              stats.scene_gaps_over_25ms += gap > 25000000;
              if (options.presentation_gap && options.presentation_gap_threshold_ns &&
                  gap >= options.presentation_gap_threshold_ns) {
                options.presentation_gap({stats.scene_presentations, submission.draw_ordinal, gap,
                                          submitted_at && mapped->monotonic_ns >= submitted_at
                                              ? mapped->monotonic_ns - submitted_at
                                              : 0,
                                          receipt->received_ns >= mapped->monotonic_ns
                                              ? receipt->received_ns - mapped->monotonic_ns
                                              : 0,
                                          receipt->refresh_ns, unsigned(pending.size())});
              }
            }
            last_scene_present_ns = mapped->monotonic_ns;
            if (submitted_at && mapped->monotonic_ns >= submitted_at &&
                receipt->received_ns >= mapped->monotonic_ns) {
              ++stats.measured_scene_receipts;
              stats.present_latency_ns += mapped->monotonic_ns - submitted_at;
              stats.presentation_poll_ns += receipt->received_ns - mapped->monotonic_ns;
            }
          }
        }
        for (std::size_t index = 0; index < options.inputs.size(); ++index) {
          const auto bit = std::uint8_t(1u << index);
          if (!(submission.staged_inputs & bit)) continue;
          // Transfer a discarded commit's ownership only if the next scene
          // drew that exact source. A newer token has its own lease/receipt.
          if (!presented && !pending.empty() && pending.front().valid &&
              pending.front().epoch == epoch &&
              pending.front().source_tokens[index] == submission.source_tokens[index])
            pending.front().staged_inputs |= bit;
          else
            options.inputs[index]->complete(submission.source_tokens[index], presented,
                                            input_receipt ? &*input_receipt : nullptr);
        }
        // After a hidden/stalled readback preview resumes, a compositor may
        // deliver several backdated receipts before its repaint clock catches
        // up. Keep withdrawing them rather than mapping the next old receipt
        // as fresh. Only a recent presented receipt ends this observed idle;
        // discarded updates cannot prove recovery. Direct output remains
        // strict, and PresentationClock's age/domain/token guards are intact.
        if (receipt->presented && !stale_preview_receipt(*receipt)) preview_waiting = false;
        // Polling stamped received_ns after our last clock query. Map against
        // that clock first: a new query here could install an anchor newer than
        // received_ns. Then recheck synchronization before publishing any PTS.
        ready = refresh();
        if (withdrawal && ready && !preview_waiting && withdrawal->submission_epoch == epoch &&
            submission.scene_generation == scene_generation && runtime.scene_root()) {
          withdrawal->current_epoch = epoch;
          options.input_withdrawal->presented(*withdrawal);
        }
      }
      observe_scene(ready && !preview_waiting && bool(runtime.scene_root()));
      if (options.scene_stall_timeout_ns && phase == script::UiWorker::DisplayPhase::frame) {
        const auto at = now_ns();
        if (ready && !first_ready_wall_ns) first_ready_wall_ns = at;
        if (first_ready_wall_ns) {
          const auto since = last_qualified_scene_wall_ns ? at - last_qualified_scene_wall_ns
                                                          : at - first_ready_wall_ns;
          const auto limit = last_qualified_scene_wall_ns ? options.scene_stall_timeout_ns
                                                          : 2 * options.scene_stall_timeout_ns;
          if (since > limit) {
            output_failure = true;
            throw std::runtime_error("qualified scene presentation stalled");
          }
        }
      }
      if (options.touches) {
        const auto reports =
            gpu->take_touches(ready && pts.has_value() && !preview_waiting && scene_visible);
        options.touches(reports);
      }
      for (const auto& input : options.inputs)
        // Bootstrap does not compose video while the other panel loads. Drain
        // compressed references without leasing decoded frames to a queue that
        // cannot receive presentation receipts yet. The control owner requests
        // fresh startup video when both entry points are running.
        input->pump(phase != script::UiWorker::DisplayPhase::bootstrap && ready && !preview_waiting
                        ? &clock.clock()
                        : nullptr);
      if (!ready) {
        if (!gpu->presentation_pending() && !outage_submitted) {
          output_operation(output_failure, [&] { gpu->compose_conical({}, Color{0, 0, 0, 1}); });
          submit(output_failure, false, false);
          outage_submitted = true;
        }
        stats.waiting = true;
        return {{}, false};
      }
      if (!pts) {
        if (!gpu->presentation_pending()) {
          output_operation(output_failure, [&] { gpu->compose_conical({}, Color{0, 0, 0, 1}); });
          if (refresh()) submit(output_failure, true, false);
        }
        stats.waiting = true;
        return {{}, false};
      }
      stats.waiting = options.reveal_underlay_while_waiting && !scene_visible;
      runtime.set_foregrounded(true);
      if (options.reveal_underlay_while_waiting && !runtime.scene_root() &&
          phase != script::UiWorker::DisplayPhase::bootstrap) {
        if (!gpu->presentation_pending() && !underlay_submitted) {
          output_operation(output_failure, [&] { gpu->compose_conical({}, Color{0, 0, 0, 0}); });
          if (refresh()) submit(output_failure, true, false);
        }
        // The VM can still consume control messages at genuine PTS and build a
        // new root. Waiting neither manufactures timing nor stops those tasks.
        return {pts, phase == script::UiWorker::DisplayPhase::frame};
      }
      if (phase == script::UiWorker::DisplayPhase::frame && gpu->presentation_pending())
        ++stats.pending_turns;
      if (phase != script::UiWorker::DisplayPhase::bootstrap &&
          gpu->presentation_slot_available() && time >= next_draw) {
        if (const auto root = runtime.scene_root()) {
          const auto composing_epoch = epoch;
          const auto composing_generation = scene_generation;
          std::uint8_t staged_inputs = 0;
          std::array<std::uint64_t, 4> source_tokens{};
          for (std::size_t index = 0; index < options.inputs.size(); ++index) {
            const auto bit = std::uint8_t(1u << index);
            if (!uses_stream(root, options.inputs[index]->stream())) continue;
            if (options.inputs[index]->stage(clock.clock())) staged_inputs |= bit;
            source_tokens[index] = options.inputs[index]->stream()->composition_token().value_or(0);
          }
          const auto began = now_ns();
          const auto withdrawal_sequence =
              options.input_withdrawal && !source_tokens[options.input_withdrawal->input_index]
                  ? options.input_withdrawal->pending(began)
                  : 0;
          const auto previous_cache = stats.cache;
          std::uint64_t inline_task_ns = 0;
          bool ticked = false;
          const auto tick_pts = pts;
          const auto during_completion = [&] {
            // The compositor invokes this callback inside its output call.
            // Restore the gate only after successful clock/runtime work, so
            // their exceptions cannot be contained as a GPU rendering error.
            output_failure = false;
            if (phase == script::UiWorker::DisplayPhase::frame && options.overlap_script_tick &&
                refresh() && epoch == composing_epoch) {
              const auto tick_started = now_ns();
              runtime.tick(tick_pts);  // Last accepted presentation, never the frame being drawn.
              inline_task_ns = now_ns() - tick_started;
              ticked = true;
              completed_inline_tick = true;
              completed_inline_task_ns = inline_task_ns;
            }
            output_failure = true;
          };
          const auto scene = with_scene_overlay(
              map_scene_canvas(options.decorate_scene ? options.decorate_scene(root) : root,
                               options.asset_canvas, output_size),
              options.overlay ? options.overlay(output_size) : std::shared_ptr<const Layer>{});
          const auto phases = output_operation(output_failure, [&] {
            return gpu->compose_scene(scene, options.scene_completion_timeout_ns,
                                      options.overlap_script_tick
                                          ? std::function<void()>(during_completion)
                                          : std::function<void()>{});
          });
          const auto elapsed = now_ns() - began;
          stats.cache = gpu->scene_cache_stats();
          stats.preparation_ns += stats.cache.preparation_ns;
          stats.traversal_ns += stats.cache.traversal_ns;
          stats.flush_ns += stats.cache.submit_ns;
          stats.max_preparation_ns = std::max(stats.max_preparation_ns, stats.cache.preparation_ns);
          stats.max_traversal_ns = std::max(stats.max_traversal_ns, stats.cache.traversal_ns);
          stats.max_flush_ns = std::max(stats.max_flush_ns, stats.cache.submit_ns);
          stats.import_ns += phases.import_ns;
          stats.validation_ns += phases.validation_ns;
          stats.completion_wait_ns += phases.completion_wait_ns;
          stats.max_completion_wait_ns =
              std::max(stats.max_completion_wait_ns, phases.completion_wait_ns);
          stats.overlap_ns += phases.overlap_ns;
          ++stats.scene_compositions;
          stats.compose_ns += elapsed;
          stats.max_compose_ns = std::max(stats.max_compose_ns, elapsed);
          if (options.slow_frame && options.slow_frame_threshold_ns &&
              elapsed >= options.slow_frame_threshold_ns) {
            const auto delta = [](std::uint64_t after, std::uint64_t before) {
              return after >= before ? after - before : 0;
            };
            options.slow_frame(
                {stats.scene_compositions, elapsed, phases.validation_ns,
                 stats.cache.preparation_ns, stats.cache.traversal_ns, stats.cache.submit_ns,
                 phases.completion_wait_ns, phases.import_ns,
                 delta(stats.cache.builds, previous_cache.builds),
                 delta(stats.cache.stroke_builds, previous_cache.stroke_builds),
                 delta(stats.cache.stroke_raster_ns, previous_cache.stroke_raster_ns),
                 delta(stats.cache.group_saves, previous_cache.group_saves),
                 stats.cache.gpu_cache_bytes});
          }
          if (!refresh() || composing_epoch != epoch) return {{}, false, inline_task_ns};
          bool stale_input = false;
          for (std::size_t index = 0; index < options.inputs.size(); ++index) {
            if (source_tokens[index] && !options.inputs[index]->composition_fresh()) {
              options.inputs[index]->suspend();
              // suspend() retires this input's outstanding source tokens too.
              // Older scene receipts must not complete those tokens later.
              for (auto& scene : pending) {
                scene.staged_inputs &= std::uint8_t(~(1u << index));
                scene.source_tokens[index] = 0;
              }
              stale_input = true;
              staged_inputs &= std::uint8_t(~(1u << index));
              source_tokens[index] = 0;
            }
          }
          // The rendered framebuffer still contains the retired/expired input.
          // Recompose without it before swapping. Deferred GPU work retains its
          // imports behind bounded completion fences in the same GL context.
          // Other freshly staged sources retain their receipt and must not be
          // left unsubmitted.
          if (stale_input) {
            const auto retry_started = now_ns();
            const auto cleared = with_scene_overlay(
                map_scene_canvas(options.decorate_scene
                                     ? options.decorate_scene(runtime.scene_root())
                                     : runtime.scene_root(),
                                 options.asset_canvas, output_size),
                options.overlay ? options.overlay(output_size) : std::shared_ptr<const Layer>{});
            const auto retry = output_operation(output_failure, [&] {
              return gpu->compose_scene(cleared, options.scene_completion_timeout_ns);
            });
            const auto retry_ns = now_ns() - retry_started;
            stats.compose_ns += retry_ns;
            stats.max_compose_ns = std::max(stats.max_compose_ns, elapsed + retry_ns);
            stats.import_ns += retry.import_ns;
            stats.validation_ns += retry.validation_ns;
            stats.completion_wait_ns += retry.completion_wait_ns;
            stats.max_completion_wait_ns =
                std::max(stats.max_completion_wait_ns, retry.completion_wait_ns);
            stats.cache = gpu->scene_cache_stats();
            stats.preparation_ns += stats.cache.preparation_ns;
            stats.traversal_ns += stats.cache.traversal_ns;
            stats.flush_ns += stats.cache.submit_ns;
            stats.max_preparation_ns =
                std::max(stats.max_preparation_ns, stats.cache.preparation_ns);
            stats.max_traversal_ns = std::max(stats.max_traversal_ns, stats.cache.traversal_ns);
            stats.max_flush_ns = std::max(stats.max_flush_ns, stats.cache.submit_ns);
            if (!refresh() || composing_epoch != epoch) return {{}, false, inline_task_ns};
          }
          observe_scene(bool(runtime.scene_root()));  // An overlapping tick may remove the root.
          if (options.reveal_underlay_while_waiting && !scene_available) {
            // The drawn sources are omitted from this real transparent commit.
            // Retain their exact preparation leases until its receipt, then
            // reject or transfer them to a newer scene that drew that source.
            submit(output_failure, true, false, staged_inputs, source_tokens);
            stats.waiting = true;
          } else
            submit(output_failure, true, true, staged_inputs, source_tokens, composing_generation,
                   withdrawal_sequence);
          // Rate-limit frame starts, not draw completion plus another interval.
          // The real presentation receipt remains the independent in-flight gate.
          next_draw = headunit::next_frame_deadline(next_draw, time, options.frame_interval_ns);
          return {pts, phase == script::UiWorker::DisplayPhase::frame && !ticked, inline_task_ns};
        }
      }
      return {pts, phase != script::UiWorker::DisplayPhase::feedback};
    } catch (...) {
      const auto failure = std::current_exception();
      terminal_fault = true;
      const OutputFaultContext context{
          phase != script::UiWorker::DisplayPhase::bootstrap,
          runtime.stats().faulted,
          pts,
          {clock.clock().ready(now_ns()), clock.clock().suspension_epoch()}};
      retire_failed_output();
      if (output_failure && output_fault.fault(context) == OutputFaultAction::isolate) {
        stats.output_isolated = true;
        ++stats.isolated_output_faults;
        if (options.isolated_output_fault) {
          try {
            std::rethrow_exception(failure);
          } catch (const std::exception& error) {
            options.isolated_output_fault(std::string_view(error.what()).substr(0, 512));
          } catch (...) {
            options.isolated_output_fault("nonstandard GPU output error");
          }
        }
        terminal_fault = false;
        auto turn = isolated_turn(phase);
        if (completed_inline_tick) {
          // A GPU failure after successful overlap must not run the same
          // script task turn a second time in the worker's fallback path.
          turn.allow_tasks = false;
          turn.inline_task_ns = completed_inline_task_ns;
        }
        return turn;
      }
      std::rethrow_exception(failure);
    }
  }
};
GpuDisplay::GpuDisplay(script::Runtime& runtime, const script::Display& display, ClockSource source,
                       GpuDisplayOptions options)
    : impl_(std::make_unique<Impl>(runtime, display, std::move(source), std::move(options))) {}
GpuDisplay::~GpuDisplay() = default;
script::UiWorker::DisplayTurn GpuDisplay::turn(script::UiWorker::DisplayPhase phase) {
  return impl_->turn(phase);
}
void GpuDisplay::set_presentation_queue_depth(unsigned depth) {
  if (depth < 1 || depth > 2)
    throw std::invalid_argument("invalid Wayland presentation queue bound");
  impl_->options.window.max_pending_presentations = depth;
  if (impl_->gpu) impl_->gpu->set_presentation_queue_depth(depth);
}
GpuDisplayStats GpuDisplay::stats() const {
  auto result = impl_->stats;
  result.pending = impl_->gpu && impl_->gpu->presentation_pending();
  result.scene_visible = impl_->scene_visible;
  return result;
}
}  // namespace headunit::theme::graphics
