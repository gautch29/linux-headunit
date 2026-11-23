// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/gpu_compositor.hpp>
#include <headunit/frame_schedule.hpp>
#include <headunit/theme/receiver_clock.hpp>
#include <headunit/theme/ui_worker.hpp>
#include <headunit/theme/display_video_input.hpp>
#include <string_view>

namespace headunit::theme::graphics {

// Content-free owner-thread snapshot for a slow scene draw. Durations
// are CPU wall times; cache counters describe only this frame, not the asset.
struct GpuSlowFrameTrace {
  std::uint64_t ordinal = 0, compose_ns = 0, validation_ns = 0, preparation_ns = 0;
  std::uint64_t traversal_ns = 0, flush_ns = 0, completion_wait_ns = 0, import_ns = 0;
  std::uint64_t retained_builds = 0, stroke_builds = 0, stroke_raster_ns = 0;
  std::uint64_t group_saves = 0, gpu_cache_bytes = 0;
};
struct GpuPresentationGapTrace {
  std::uint64_t scene_ordinal = 0, draw_ordinal = 0, gap_ns = 0;
  std::uint64_t submit_to_present_ns = 0, present_to_poll_ns = 0;
  std::uint32_t refresh_ns = 0;
  unsigned queued_behind = 0;
};
struct InputWithdrawalReceipt {
  std::uint64_t sequence = 0, submission_epoch = 0, current_epoch = 0, draw = 0;
  std::uint64_t submitted_ns = 0, presented_ns = 0, observed_ns = 0;
};

struct GpuDisplayOptions {
  WaylandOptions window;
  // Optional authored canvas, distinct from actual video/output/touch pixels.
  // Both zero keep the runtime's output geometry unchanged.
  Size asset_canvas{};
  // Optional bounded host decoration in authored coordinates, on this owner.
  // Returns a separate tree; never mutates the signed scene or its animation.
  std::function<std::shared_ptr<const Layer>(const std::shared_ptr<const Layer>&)> decorate_scene;
  // Optional immutable native overlay, expressed in output coordinates. Called
  // on the display owner only, before submission; never updates script scenes.
  std::function<std::shared_ptr<const Layer>(Size)> overlay;
  // Owner-thread callbacks into a connection-scoped bounded mailbox only.
  std::function<void(std::span<const headunit::TouchInput>)> touches;
  std::function<void()> cancel_touches;
  // Optional main-surface handoff. acquire is nonblocking, including while
  // already held; false requests withdrawal. release is called only after
  // the GPU/window has gone. Both callbacks run on the display owner.
  struct SurfaceLease {
    std::function<bool()> acquire;
    std::function<void()> release;
  };
  std::optional<SurfaceLease> surface_lease;
  // Construct/register streams before asset entry points. One display owns
  // each source; producer threads only use its separate transport queue API.
  std::vector<std::shared_ptr<video::DisplayVideoInput>> inputs;
  // Optional owner request stamp for one input. The stamp is copied into the
  // existing submission only when the composition has no source token for
  // that input. Completion uses that exact accepted current scene receipt.
  struct InputWithdrawal {
    std::size_t input_index = 0;
    std::function<std::uint64_t(std::uint64_t)> pending;
    std::function<void(const InputWithdrawalReceipt&)> presented;
  };
  std::optional<InputWithdrawal> input_withdrawal;
  std::uint64_t frame_interval_ns = headunit::ui_frame_period_ns;
  // Experimental single-owner overlap. Off by default until live validation.
  bool overlap_script_tick = false;
  // Longer budgets are only for explicitly selected offline scene diagnostics.
  std::uint64_t scene_completion_timeout_ns = 100000000;
  // Optional live-session fail-safe. A panel that stops producing qualified
  // scene receipts is withdrawn, so the independent cluster can be shown.
  // Zero disables this policy for offline and diagnostic renderers.
  std::uint64_t scene_stall_timeout_ns = 0;
  // Opt-in containment of output-only errors after real presentation and VM
  // startup. The failed window is retired for this session; healthy control
  // tasks may drain at the last actual PTS while its clock epoch remains valid.
  // Clock, input and VM failures still propagate. This cannot contain a hung
  // driver, and it never recreates the renderer or certifies gauge visibility.
  bool isolate_output_faults = false;
  // Opt-in cluster policy: transparent bootstrap/outage/missing-root commits
  // reveal an already mapped native backup. Only a fresh qualified scene
  // receipt enables scene visibility/touch again. No retry or invented PTS.
  bool reveal_underlay_while_waiting = false;
  // Called once on the owner after successful withdrawal/isolation. The reason
  // is at most 512 characters; the callback must be bounded and nonblocking.
  // Callback failures remain terminal. No scene data or receipt is synthesized.
  std::function<void(std::string_view)> isolated_output_fault;
  // Optional diagnostic sink. It must be fast and must not retain scene data.
  std::uint64_t slow_frame_threshold_ns = 0;
  std::function<void(const GpuSlowFrameTrace&)> slow_frame;
  std::uint64_t presentation_gap_threshold_ns = 0;
  std::function<void(const GpuPresentationGapTrace&)> presentation_gap;
};
struct GpuDisplayStats {
  std::uint64_t submissions = 0, presentations = 0, scene_presentations = 0;
  std::uint64_t scene_zero_copy_presentations = 0;  // Accepted Wayland ZERO_COPY feedback.
  std::uint64_t ignored_receipts = 0, suspensions = 0;
  // Each suspension has one primary cause. Kept separately so a live blank
  // can be attributed without logging phone content or changing display policy.
  std::uint64_t clock_epoch_suspensions = 0, clock_unavailable_suspensions = 0;
  std::uint64_t owner_gap_suspensions = 0, feedback_delay_suspensions = 0;
  std::uint64_t verification_declarations = 0;
  // Wall-time costs on the owner thread; never interpreted as presentation PTS.
  std::uint64_t scene_compositions = 0, compose_ns = 0, max_compose_ns = 0;
  std::uint64_t scene_submissions = 0, submit_ns = 0, max_submit_ns = 0;
  std::uint64_t pending_turns = 0;
  std::uint64_t feedback_receipts = 0, feedback_ns = 0, max_feedback_ns = 0;
  std::uint32_t last_refresh_ns = 0;
  std::uint64_t measured_scene_receipts = 0, present_latency_ns = 0, presentation_poll_ns = 0;
  std::uint64_t max_scene_present_gap_ns = 0, scene_gaps_over_20ms = 0, scene_gaps_over_25ms = 0;
  std::uint64_t preparation_ns = 0, traversal_ns = 0, flush_ns = 0, import_ns = 0;
  std::uint64_t validation_ns = 0, completion_wait_ns = 0, overlap_ns = 0;
  std::uint64_t max_preparation_ns = 0, max_traversal_ns = 0, max_flush_ns = 0,
                max_completion_wait_ns = 0;
  GpuSceneCacheStats cache;
  std::uint64_t surface_withdrawals = 0, surface_restorations = 0;
  std::uint64_t isolated_output_faults = 0;
  std::uint64_t underlay_submissions = 0;  // Actual transparent commits, not visibility proof.
  bool surface_withdrawn = false;
  bool output_isolated = false;  // Sticky until a new GpuDisplay/session.
  bool scene_visible = false;    // Current generation has a qualified scene receipt.
  bool waiting = true, faulted = false, pending = false;
};

// One VM/display/paired connection, entirely on the script owner's thread.
// Construct before evaluating the asset; Runtime must live through the last
// turn. Destruction releases the window without accessing the Runtime.
class GpuDisplay {
 public:
  using ClockSource = std::function<std::optional<AirplayClockSample>()>;
  GpuDisplay(script::Runtime&, const script::Display&, ClockSource, GpuDisplayOptions = {});
  ~GpuDisplay();
  GpuDisplay(const GpuDisplay&) = delete;
  GpuDisplay& operator=(const GpuDisplay&) = delete;
  // For UiWorker::Configuration::presentation. Real feedback bootstraps PTS;
  // ordinary turns submit asynchronously and complete staged video on receipts.
  script::UiWorker::DisplayTurn turn(
      script::UiWorker::DisplayPhase = script::UiWorker::DisplayPhase::frame);
  GpuDisplayStats stats() const;
  // Owner thread. Applies to the current surface and any later recreation.
  void set_presentation_queue_depth(unsigned depth);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::graphics
