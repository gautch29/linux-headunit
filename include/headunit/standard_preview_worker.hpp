// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/gst_preview_frames.hpp>
#include <headunit/theme/gpu_compositor.hpp>
#include <headunit/main_surface_control.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace headunit {
// Owned Standard video presenter. GPU/Wayland work never runs on the phone's
// control owner. Only immutable copied pixels cross the bounded mailbox; no
// GStreamer sample, buffer, element or fabricated sender clock crosses it.
class StandardPreviewWorker {
 public:
  using Generation = std::uint64_t;
  struct Options {
    unsigned width = 1280, height = 720;
    theme::graphics::WaylandOptions window = [] {
      theme::graphics::WaylandOptions value;
      value.title = "CarPlay";
      value.app_id = "org.headunit.carplay.main";
      return value;
    }();
    std::string surface_lease_file;
    std::shared_ptr<MainSurfaceControl> native_page_control;
    bool initially_visible = true;
  };
  struct Status {
    Generation requested = 1, window = 0, retired = 0;
    bool window_ready = false, source_armed = false, source_suspended = false, touch_ready = false,
         faulted = false;
    std::uint64_t accepted = 0, coalesced = 0, rejected = 0, submitted = 0, presented = 0,
                  discarded = 0, retried = 0;
    // Actual owner-turn heartbeat, independent of source arrival/presentation
    // timestamps. Control readers cannot renew a stalled worker's observation.
    std::uint64_t observed_monotonic_ns = 0;
    std::optional<std::uint64_t> presented_arrival_running_ns;
    std::string error;  // Bounded; no image, phone identifier or private path.
  };
  explicit StandardPreviewWorker(Options);
  ~StandardPreviewWorker();
  StandardPreviewWorker(const StandardPreviewWorker&) = delete;
  StandardPreviewWorker& operator=(const StandardPreviewWorker&) = delete;

  // Every explicit request advances the generation, even if the final state
  // matches the old state. Rapid hide/show cannot coalesce away teardown.
  // Immediately retire touch, including a release for a delivered contact.
  // Withdrawal is acknowledged only after GPU destruction and lease release.
  Generation request_window(bool visible);
  // Retire source/input before decoder-output drain while keeping the window
  // lease held. Sticky for this window generation; only a new request can arm
  // it again. In-flight drawing cannot submit after observing suspension.
  bool suspend_source(Generation);
  Status status() const;
  // Call only after the matching window-ready ACK and decoder-output restore.
  // The original running-time floor is mandatory after generation 1. nullopt
  // is permitted only for the initial visible startup, which makes no camera-
  // return claim. Only a strictly newer accepted image can qualify touch.
  bool arm_source(Generation, std::optional<std::uint64_t> arrival_floor);
  bool offer(Generation, GstPreviewFrames::Frame);
  // Nonblocking input readiness; the caller owns all actual phone/HID writes.
  int input_fd() const noexcept;
  // Bounded nonblocking sender on the control owner. False retains a report,
  // including a cancellation release under phone backpressure. Retry on the
  // regular owner turn even without new input. Do not call another worker
  // method from this callback; it runs under the short input-mailbox lock.
  unsigned drain_touches(const std::function<bool(TouchInput)>& sender);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
