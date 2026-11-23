// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/standard_preview.hpp>
#include <headunit/gst_preview_output.hpp>
#include <stdexcept>

namespace headunit {
namespace {
void require(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
}  // namespace
struct StandardPreview::Impl {
  GstPreviewOutput& output;
  Options options;
  std::unique_ptr<GstPreviewFrames> frames;
  std::unique_ptr<StandardPreviewWorker> worker;
  Status state;

  Impl(GstPreviewOutput& value, Options configuration)
      : output(value), options(std::move(configuration)) {
    require(bool(options.camera_control) == !options.presenter.surface_lease_file.empty(),
            "Standard camera needs both activation and surface lease");
    require(!options.presenter.native_page_control || !options.camera_control,
            "Standard native page and camera ownership cannot be combined");
    require(!(options.camera_control || options.presenter.native_page_control) ||
                !options.presenter.window.output_name.empty(),
            "Standard handoff needs a named main output");
    const bool hidden = active();
    options.presenter.initially_visible = !hidden;
    if (hidden)
      state.stage = Stage::hidden;
    else {
      frames = adapter();
      output.start(frames->release_output());
    }
    worker = std::make_unique<StandardPreviewWorker>(options.presenter);
  }
  bool active() noexcept {
    state.camera_active = options.camera_control && options.camera_control->poll();
    state.native_page_active = options.presenter.native_page_control &&
                               options.presenter.native_page_control->native_requested();
    return state.camera_active || state.native_page_active;
  }
  std::unique_ptr<GstPreviewFrames> adapter() const {
    return std::make_unique<GstPreviewFrames>(options.presenter.width, options.presenter.height);
  }
  void retire() noexcept {
    state.stage = Stage::faulted;
    // Preserve the touch owner after withdrawal, including a release whose
    // phone queue is temporarily full. Never destroy it inside this path.
    if (worker) {
      try {
        (void)worker->suspend_source(state.window_generation);
      } catch (...) {
      }
    }
    try {
      if (!output.stats().draining) output.drain();
    } catch (...) {
    }
    frames.reset();
    if (worker) {
      try {
        state.window_generation = worker->request_window(false);
      } catch (...) {
      }
    }
  }
  void hide() {
    require(worker->suspend_source(state.window_generation), "Standard source could not suspend");
    // No sample/strong appsink reference crosses this call. A throwing drain
    // still withdraws the GPU through pump's terminal cleanup.
    if (!output.stats().draining) output.drain();
    frames.reset();
    state.window_generation = worker->request_window(false);
    state.stage = Stage::hidden;
  }
  void pump() {
    require(state.stage != Stage::faulted, "Standard optional presenter is retired");
    try {
      require(!worker->status().faulted, "Standard GPU presenter faulted");
      if (active()) {
        if (state.stage != Stage::hidden) hide();
        return;
      }
      if (state.stage == Stage::hidden) {
        state.window_generation = worker->request_window(true);
        state.stage = Stage::returning;
      }
      // Observe actual epoch identity even when no new encoded frame arrives.
      const bool restorable = output.restore_ready();
      require(!output.stats().decoder_epoch_retired, "Standard original decoder epoch retired");
      if (state.stage == Stage::initial || state.stage == Stage::returning) {
        const auto status = worker->status();
        if (!status.window_ready || status.window != state.window_generation ||
            status.requested != state.window_generation)
          return;
        // Recheck the manual request after consuming the async ACK, before
        // installing a source branch or rearming a returned window.
        if (active()) {
          hide();
          return;
        }
        if (state.stage == Stage::initial) {
          require(worker->arm_source(state.window_generation, {}),
                  "Standard startup source could not arm");
        } else {
          if (!restorable) return;
          const auto previous = output.stats().generation;
          auto next = adapter();
          output.restore(next->release_output());
          const auto installed = output.stats();
          require(!installed.draining && !installed.decoder_epoch_retired &&
                      !installed.barrier_retired && installed.generation == previous + 1 &&
                      installed.arrival_floor.has_value(),
                  "Standard output did not publish matching restore boundary");
          require(worker->arm_source(state.window_generation, installed.arrival_floor),
                  "Standard returned source could not arm");
          frames = std::move(next);
        }
        state.stage = Stage::visible;
      }
      if (state.stage != Stage::visible) return;
      require(frames && !frames->closed(), "Standard decoded source disappeared");
      // Keep the control turn bounded even when the decoder produces a burst.
      for (unsigned count = 0; count < 3; ++count) {
        auto frame = frames->pull();
        if (!frame) break;
        if (active()) {
          hide();
          return;
        }
        (void)worker->offer(state.window_generation, std::move(*frame));
      }
      require(!frames->eos(), "Standard decoded source ended");
    } catch (...) {
      retire();
      throw;
    }
  }
};
StandardPreview::StandardPreview(GstPreviewOutput& output, Options options)
    : impl_(std::make_unique<Impl>(output, std::move(options))) {}
StandardPreview::~StandardPreview() = default;
void StandardPreview::pump() {
  impl_->pump();
}
void StandardPreview::retire() noexcept {
  impl_->retire();
}
int StandardPreview::input_fd() const noexcept {
  return impl_->worker->input_fd();
}
unsigned StandardPreview::drain_touches(const std::function<bool(TouchInput)>& sender) {
  if (impl_->state.stage != Stage::faulted) impl_->pump();
  return impl_->worker->drain_touches(sender);
}
StandardPreview::Status StandardPreview::status() const {
  auto result = impl_->state;
  result.presenter = impl_->worker->status();
  return result;
}
}  // namespace headunit
