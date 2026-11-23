// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_display.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <stdexcept>

namespace headunit::camera {
namespace g = theme::graphics;
namespace {
std::uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
}  // namespace
struct NativeDisplay::Impl {
  struct Pending {
    std::uint64_t token;
    std::shared_ptr<const Frame> frame;
    CameraScene scene;
    Availability availability;
    bool newly_staged = false;
  };
  Configuration configuration;
  Queue& frames;
  DisplayOptions options;
  SceneBuilder builder;
  std::unique_ptr<g::GpuCompositor> gpu;
  LocalPresentationClock clock;
  std::deque<Pending> pending;
  std::optional<Availability> last_submitted_state;
  std::optional<ManualCloseState> last_close_state;
  DisplayStatistics stats;
  std::uint64_t serial = 0;
  Impl(Configuration c, Queue& queue, DisplayOptions o)
      : configuration(std::move(c)),
        frames(queue),
        options(std::move(o)),
        builder(configuration, bool(options.manual_close_state)) {
    validate_configuration(configuration);
    check(configuration.enabled, "native camera display is disabled");
    check(!options.window.defer_scene_fence,
          "native camera display currently requires synchronous GPU fences");
    check(options.gpu_completion_timeout_ns > 0 && options.gpu_completion_timeout_ns <= 2000000000,
          "invalid camera GPU completion deadline");
    options.window.max_pending_presentations = 2;
    options.window.opaque_background = true;
    if (options.window.title == "Headunit native video")
      options.window.title = "Headunit local camera";
    if (options.window.app_id == "org.headunit.ultra.display")
      options.window.app_id = "org.headunit.camera";
    gpu = std::make_unique<g::GpuCompositor>(configuration.canvas_width,
                                             configuration.canvas_height, options.window);
  }
  ~Impl() {
    // The compositor finishes/releases any GPU imports while the explicit
    // scene/frame leases below are still alive. Destruction never calls a VM.
    gpu.reset();
    frames.discard();
    pending.clear();
  }
  void receive() {
    for (unsigned i = 0; i < 2; ++i) {
      const auto receipt = gpu->poll_presentation();
      if (!receipt) return;
      check(!pending.empty() && pending.front().token == receipt->token,
            "native camera receipt order changed");
      const auto mapped = clock.accept(*receipt);
      auto& submission = pending.front();
      DisplayReceipt event;
      event.token = receipt->token;
      event.flags = receipt->flags;
      event.refresh_ns = receipt->refresh_ns;
      event.presented = bool(mapped);
      event.source = submission.availability;
      if (mapped) {
        event.presentation_monotonic_ns = mapped->monotonic_ns;
        ++stats.presentations;
      }
      if (submission.frame) {
        event.capture_monotonic_ns = submission.frame->capture_monotonic_ns;
        event.simulated = submission.frame->simulated;
        event.capture_reference = submission.frame->capture_reference;
        if (mapped && submission.availability == Availability::ready &&
            mapped->monotonic_ns >= submission.frame->capture_monotonic_ns) {
          event.capture_age_ns = mapped->monotonic_ns - submission.frame->capture_monotonic_ns;
          stats.max_capture_age_ns = std::max(stats.max_capture_age_ns, *event.capture_age_ns);
          event.fresh_camera =
              submission.availability == Availability::ready &&
              *event.capture_age_ns <= std::uint64_t(configuration.maximum_age_ms) * 1000000;
          if (event.fresh_camera) ++stats.fresh_camera_presentations;
        }
        if (submission.newly_staged)
          frames.complete(submission.frame->sequence,
                          bool(mapped) && submission.availability == Availability::ready);
      }
      if (submission.scene.native_stream && submission.scene.native_token) {
        if (mapped && submission.availability == Availability::ready)
          submission.scene.native_stream->presented(*submission.scene.native_token);
        else
          submission.scene.native_stream->cancel(*submission.scene.native_token);
      }
      if (mapped && submission.availability == Availability::missing) ++stats.missing_presentations;
      if (mapped && submission.availability == Availability::stale) ++stats.stale_presentations;
      pending.pop_front();
      stats.pending = pending.size();
      // An explicitly discarded last status commit left no visible update.
      // Permit one later retry; never manufacture a presentation or accumulate
      // extra pending status frames while another newer commit is outstanding.
      if (!mapped && pending.empty()) last_submitted_state.reset();
      if (options.receipt)
        options.receipt(event);  // Bounded metadata only; no frame storage escapes here.
    }
  }
  void turn() {
    check(!stats.faulted, "native camera display faulted");
    try {
      receive();
      if (pending.size() == 2 || !gpu->presentation_slot_available()) return;
      const auto at = now();
      auto frame = frames.stage(at);
      bool newly_staged = bool(frame);
      auto availability = frames.availability(at);
      const auto close_state =
          options.manual_close_state ? std::optional(options.manual_close_state()) : std::nullopt;
      if (!frame && last_submitted_state == availability && last_close_state == close_state) return;
      if (!frame && availability == Availability::ready) {
        frame = frames.current(at);
        // Pending capture submissions have already drawn the ready state;
        // no empty scene should cover their image while awaiting a receipt.
        if (!frame && !pending.empty()) return;
        if (!frame) availability = Availability::missing;
      }
      check(serial < std::numeric_limits<std::uint64_t>::max(),
            "native camera presentation tokens exhausted");
      auto scene = builder.build(frame, availability, close_state);
      // compose_scene's default path completes the actual GPU fence before
      // returning. Keep the camera lease in Pending until the Wayland receipt
      // too, so producer storage is never recycled during either operation.
      gpu->compose_scene(scene.root, options.gpu_completion_timeout_ns);
      if (frame && now() - frame->capture_monotonic_ns >
                       std::uint64_t(configuration.maximum_age_ms) * 1000000) {
        // Cold shader setup or a slow GPU must not submit an already-expired
        // capture. The synchronous fence above has finished reading its data.
        if (scene.native_stream && scene.native_token)
          scene.native_stream->cancel(*scene.native_token);
        // A newer staged capture may have an older receipt ahead of it. Keep
        // its outer permit/token in the stale-status submission and cancel it
        // only when that real status receipt reaches the front, in order.
        if (!newly_staged) frame.reset();
        availability = Availability::stale;
        scene = builder.build({}, availability, close_state);
        gpu->compose_scene(scene.root, options.gpu_completion_timeout_ns);
      }
      pending.push_back(
          {serial + 1, std::move(frame), std::move(scene), availability, newly_staged});
      gpu->submit(serial + 1);
      ++serial;
      ++stats.submissions;
      stats.pending = pending.size();
      stats.maximum_pending = std::max(stats.maximum_pending, stats.pending);
      last_submitted_state = availability;
      last_close_state = close_state;
    } catch (...) {
      stats.faulted = true;
      // The GPU destructor completes/withdraws its own work before camera
      // cancellation releases our source generation's staged references.
      gpu.reset();
      frames.discard();
      pending.clear();
      stats.pending = 0;
      throw;
    }
  }
};
NativeDisplay::NativeDisplay(Configuration c, Queue& q, DisplayOptions o)
    : impl_(std::make_unique<Impl>(std::move(c), q, std::move(o))) {}
NativeDisplay::~NativeDisplay() = default;
void NativeDisplay::turn() {
  impl_->turn();
}
DisplayStatistics NativeDisplay::statistics() const {
  return impl_->stats;
}
std::vector<headunit::TouchInput> NativeDisplay::take_touches() {
  return impl_->gpu ? impl_->gpu->take_touches(true) : std::vector<headunit::TouchInput>{};
}
std::string NativeDisplay::renderer() const {
  return impl_->gpu ? impl_->gpu->renderer() : std::string{};
}
}  // namespace headunit::camera
