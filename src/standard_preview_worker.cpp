// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/standard_preview_worker.hpp>
#include <headunit/presentation_status.hpp>
#include <headunit/camera_display.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <headunit/touch_mailbox.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace headunit {
namespace g = theme::graphics;
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
std::shared_ptr<const g::Layer> scene(const GstPreviewFrames::Frame& frame, unsigned width,
                                      unsigned height) {
  auto root = std::make_shared<g::Layer>();
  root->set_frame({{0, 0}, {double(width), double(height)}});
  root->background = {0, 0, 0, 1};
  auto picture = std::make_shared<g::Layer>(g::Kind::image);
  picture->set_frame(root->frame());
  // The legacy graphics field is mutable, but this private scene is never
  // mutated after construction and reads only immutable producer storage.
  picture->image = std::const_pointer_cast<g::Image>(frame.image);
  root->insert(std::move(picture), 0);
  return root;
}
}  // namespace
struct StandardPreviewWorker::Impl {
  struct WorkFrame {
    GstPreviewFrames::Frame source;
    unsigned retries = 0;
  };
  struct Pending {
    std::uint64_t token = 0;
    Generation generation = 0;
    GstPreviewFrames::Frame source;
    std::shared_ptr<const g::Layer> root;
    unsigned retries = 0;
  };
  Options options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  Status state;
  bool visible = true;
  std::optional<std::uint64_t> floor, last_arrival;
  std::optional<WorkFrame> latest;
  TouchMailbox touches;
  TouchMailbox::Generation touch_generation = 0;
  std::jthread worker;

  explicit Impl(Options value) : options(std::move(value)), visible(options.initially_visible) {
    check(options.width >= 320 && options.width <= 4096 && options.height >= 240 &&
              options.height <= 2160,
          "invalid Standard GPU dimensions");
    check(options.window.readback_render_node.empty(),
          "Standard GPU preview forbids readback output");
    check(!options.window.defer_scene_fence, "Standard GPU preview requires explicit completion");
    check(options.window.max_pending_presentations >= 1 &&
              options.window.max_pending_presentations <= 2,
          "invalid Standard GPU presentation bound");
    options.window.accept_touch = true;
    options.window.opaque_background = true;
    if (!options.surface_lease_file.empty())
      camera::CameraSurfaceLease::validate(options.surface_lease_file);
    check(!options.native_page_control || options.surface_lease_file.empty(),
          "Standard native page and camera leases cannot be combined");
    touch_generation = touches.begin();
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  ~Impl() {
    worker.request_stop();
    wake.notify_all();
    if (worker.joinable()) worker.join();
  }
  bool current(Generation generation) const {
    std::lock_guard lock(mutex);
    return visible && state.requested == generation && !state.faulted;
  }
  bool current_source(Generation generation) const {
    std::lock_guard lock(mutex);
    return visible && state.requested == generation && !state.faulted && state.source_armed &&
           !state.source_suspended;
  }
  void run(std::stop_token stop) noexcept {
    std::unique_ptr<camera::CameraSurfaceLease> lease;
    std::unique_ptr<g::GpuCompositor> gpu;
    std::deque<Pending> pending;
    camera::LocalPresentationClock clock;
    Generation active = 0;
    std::uint64_t next_token = 0;
    auto retire = [&] {
      // Window/imports/fences belong to this owner. Retire them before allowing
      // the exclusive camera owner to acquire the same surface lease.
      gpu.reset();
      pending.clear();
      if (lease) lease->release();
      if (options.native_page_control) options.native_page_control->renderer_release();
      clock = {};
      std::lock_guard lock(mutex);
      state.retired = std::max(state.retired, active);
      state.window = 0;
      state.window_ready = false;
      state.touch_ready = false;
      touches.cancel(touch_generation);
      active = 0;
    };
    try {
      if (!options.surface_lease_file.empty())
        lease = std::make_unique<camera::CameraSurfaceLease>(options.surface_lease_file);
      while (!stop.stop_requested()) {
        Generation desired;
        bool show;
        {
          std::lock_guard lock(mutex);
          desired = state.requested;
          show = visible;
        }
        if (active != desired || !show) {
          if (active || gpu) retire();
          if (!show) {
            std::lock_guard lock(mutex);
            // A hidden generation has no surface, even if never constructed.
            if (state.requested == desired) state.retired = std::max(state.retired, desired);
          } else if (options.native_page_control ? options.native_page_control->renderer_acquire()
                                                 : (!lease || lease->acquire_shared())) {
            active = desired;
            gpu = std::make_unique<g::GpuCompositor>(options.width, options.height, options.window);
            // Construction can block. Never publish an ACK for a request that
            // was retired while the display connection was being established.
            if (!current(active)) {
              retire();
              continue;
            }
            (void)gpu->take_touches(false);
            std::lock_guard lock(mutex);
            if (state.requested == active && visible) {
              state.window = active;
              state.window_ready = true;
            }
          }
        }
        if (gpu && current(active)) {
          if (lease) check(lease->holds(), "Standard GPU lost its held surface lease");
          if (options.native_page_control)
            check(options.native_page_control->renderer_holds(),
                  "Standard GPU lost its native page surface lease");
          // Poll even without new video. This processes receipts and physical
          // input for a static home screen without calling the control owner.
          for (unsigned count = 0; count < 2; ++count) {
            const auto feedback = gpu->poll_presentation();
            if (!feedback) break;
            check(!pending.empty() && feedback->token == pending.front().token,
                  "Standard GPU receipt does not match submitted source");
            auto receipt = std::move(pending.front());
            pending.pop_front();
            const auto presented = clock.accept(*feedback);
            std::lock_guard lock(mutex);
            if (receipt.generation != state.requested || !visible || !state.source_armed) continue;
            if (presented) {
              ++state.presented;
              state.presented_arrival_running_ns = receipt.source.arrival_running_ns;
              state.touch_ready = true;
            } else {
              ++state.discarded;
              // A static first frame may be discarded. Retry that SAME source
              // lease without changing its arrival time or counting a fresh
              // producer frame, unless a newer image is already waiting.
              if (!latest && pending.empty()) {
                check(receipt.retries < 3, "Standard GPU source repeatedly discarded");
                latest = WorkFrame{std::move(receipt.source), receipt.retries + 1};
                ++state.retried;
              }
            }
          }
          bool input_enabled;
          {
            std::lock_guard lock(mutex);
            input_enabled = state.requested == active && visible && state.touch_ready;
          }
          const auto reports = gpu->take_touches(input_enabled);
          {
            std::lock_guard lock(mutex);
            if (state.requested == active && visible && state.touch_ready)
              touches.push(touch_generation, reports);
          }
          std::optional<WorkFrame> frame;
          if (gpu->presentation_slot_available()) {
            std::lock_guard lock(mutex);
            if (state.requested == active && visible && state.source_armed) {
              frame = std::move(latest);
              latest.reset();
            }
          }
          if (frame && current_source(active)) {
            const auto root = scene(frame->source, options.width, options.height);
            gpu->compose_scene(root);
            // A hide may arrive during composition. It cancels touch at once;
            // no stale source is submitted knowingly or accepted on return.
            if (current_source(active)) {
              if (lease) check(lease->holds(), "Standard GPU surface lease changed during drawing");
              if (options.native_page_control)
                check(options.native_page_control->renderer_holds(),
                      "Standard native page surface lease changed during drawing");
              check(next_token < std::numeric_limits<std::uint64_t>::max(),
                    "Standard GPU token exhausted");
              const auto token = ++next_token;
              check(pending.size() < options.window.max_pending_presentations,
                    "Standard GPU pending bound exceeded");
              pending.push_back({token, active, std::move(frame->source), root, frame->retries});
              gpu->submit(token);
              std::lock_guard lock(mutex);
              ++state.submitted;
            }
          }
        }
        const bool can_consume = gpu && gpu->presentation_slot_available();
        std::unique_lock lock(mutex);
        state.observed_monotonic_ns = presentation_monotonic_ns();
        // Fixed polling bound also services the Wayland socket while a pending
        // receipt prevents consuming the latest-frame mailbox.
        wake.wait_for(lock, std::chrono::milliseconds(5), [&] {
          return stop.stop_requested() || state.requested != desired ||
                 (can_consume && latest.has_value());
        });
      }
      retire();
    } catch (...) {
      // Do not leave a faulted phone window holding the shared camera lease.
      // Error text is deliberately stable rather than a dependency exception
      // that could contain private display paths or protocol payloads.
      retire();
      std::lock_guard lock(mutex);
      latest.reset();
      state.source_armed = false;
      state.faulted = true;
      state.error = "Standard GPU presenter retired after output failure";
      touches.retire();
    }
  }
};
StandardPreviewWorker::StandardPreviewWorker(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
StandardPreviewWorker::~StandardPreviewWorker() = default;
StandardPreviewWorker::Generation StandardPreviewWorker::request_window(bool visible) {
  auto& p = *impl_;
  std::lock_guard lock(p.mutex);
  check(!p.state.faulted, "Standard GPU presenter is faulted");
  check(p.state.requested < std::numeric_limits<Generation>::max(),
        "Standard GPU generation exhausted");
  ++p.state.requested;
  p.visible = visible;
  p.latest.reset();
  p.floor.reset();
  p.last_arrival.reset();
  p.state.window_ready = false;
  p.state.source_armed = false;
  p.state.touch_ready = false;
  p.state.source_suspended = false;
  p.state.presented_arrival_running_ns.reset();
  p.touches.retire();
  p.touch_generation = p.touches.begin();
  p.wake.notify_all();
  return p.state.requested;
}
bool StandardPreviewWorker::suspend_source(Generation generation) {
  auto& p = *impl_;
  std::lock_guard lock(p.mutex);
  if (p.state.faulted || p.state.requested != generation) return false;
  p.state.source_armed = false;
  p.state.source_suspended = true;
  p.state.touch_ready = false;
  p.state.presented_arrival_running_ns.reset();
  p.latest.reset();
  p.touches.retire();
  p.touch_generation = p.touches.begin();
  p.wake.notify_all();
  return true;
}
StandardPreviewWorker::Status StandardPreviewWorker::status() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}
bool StandardPreviewWorker::arm_source(Generation generation, std::optional<std::uint64_t> floor) {
  auto& p = *impl_;
  std::lock_guard lock(p.mutex);
  if (p.state.faulted || !p.visible || p.state.requested != generation ||
      p.state.window != generation || !p.state.window_ready || p.state.source_armed ||
      p.state.source_suspended)
    return false;
  check(floor.has_value() || generation == 1, "Standard return requires original arrival boundary");
  p.floor = floor;
  p.state.source_armed = true;
  p.wake.notify_all();
  return true;
}
bool StandardPreviewWorker::offer(Generation generation, GstPreviewFrames::Frame frame) {
  auto& p = *impl_;
  std::lock_guard lock(p.mutex);
  if (p.state.faulted || !p.visible || p.state.requested != generation || !p.state.source_armed ||
      (p.floor && frame.arrival_running_ns <= *p.floor) ||
      (p.last_arrival && frame.arrival_running_ns <= *p.last_arrival)) {
    ++p.state.rejected;
    return false;
  }
  check(frame.image && frame.image->width == p.options.width &&
            frame.image->height == p.options.height &&
            frame.image->pixels.size() == std::size_t(p.options.width) * p.options.height,
        "Standard GPU source dimensions or storage mismatch");
  p.last_arrival = frame.arrival_running_ns;
  ++p.state.accepted;
  if (p.latest) ++p.state.coalesced;
  p.latest = Impl::WorkFrame{std::move(frame), 0};
  p.wake.notify_all();
  return true;
}
int StandardPreviewWorker::input_fd() const noexcept {
  return impl_->touches.wake_fd();
}
unsigned StandardPreviewWorker::drain_touches(const std::function<bool(TouchInput)>& sender) {
  check(bool(sender), "Standard touch sender is missing");
  return impl_->touches.drain(sender);
}
}  // namespace headunit
