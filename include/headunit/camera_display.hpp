// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/camera.hpp>
#include <headunit/camera_manual_client.hpp>
#include <headunit/theme/gpu_compositor.hpp>
#include <headunit/theme/video_stream.hpp>
#include <array>
#include <functional>

namespace headunit::camera {
// Fit the complete oriented capture inside the configured rectangle. Rotation
// is clockwise; horizontal mirroring applies in oriented output coordinates.
theme::graphics::Transform capture_transform(const Configuration&);
struct CameraScene {
  std::shared_ptr<const theme::graphics::Layer> root;
  // Native video is staged only as a GPU texture source. Its local PTS never
  // goes through a phone MediaClock. Advance it only on the real receipt.
  std::shared_ptr<theme::video::Stream> native_stream;
  std::optional<std::uint64_t> native_token;
};
class SceneBuilder {
 public:
  explicit SceneBuilder(Configuration, bool manual_controls = false);
  CameraScene build(const std::shared_ptr<const Frame>&, Availability,
                    std::optional<ManualCloseState> manual_close = std::nullopt) const;
  // Output-space native overlay for Ultra or other renderers. No script
  // mutation. Inactive returns null; unavailable states cover only the camera
  // rectangle and identify the fault, while ready simulation gets a badge.
  std::shared_ptr<const theme::graphics::Layer> status_overlay(bool active, Availability,
                                                               bool simulated,
                                                               theme::graphics::Size output) const;

 private:
  Configuration configuration_;
  std::shared_ptr<theme::graphics::Image> simulated_, live_, missing_, stale_, clock_reversed_,
      closed_;
  std::array<std::shared_ptr<theme::graphics::Image>, 4> close_labels_;
};
struct LocalPresentation {
  std::uint64_t monotonic_ns = 0, uncertainty_ns = 0;
};
// Validates actual Wayland feedback and its real cross-clock sample directly
// against local monotonic time. No fabricated AirPlay/media synchronization.
class LocalPresentationClock {
 public:
  std::optional<LocalPresentation> accept(const theme::video::PresentationFeedback&);
  bool failed() const { return failed_; }

 private:
  std::uint64_t last_token_ = 0, last_received_ = 0;
  std::optional<std::uint32_t> clock_id_;
  std::optional<theme::video::ClockCorrelation> previous_;
  std::optional<LocalPresentation> last_;
  bool failed_ = false;
};
struct DisplayReceipt {
  std::uint64_t token = 0;
  std::optional<std::uint64_t> capture_monotonic_ns, presentation_monotonic_ns, capture_age_ns;
  std::uint32_t flags = 0, refresh_ns = 0;
  bool presented = false, simulated = false, fresh_camera = false;
  Availability source = Availability::missing;
  std::optional<CaptureReference> capture_reference = std::nullopt;
};
struct DisplayOptions {
  theme::graphics::WaylandOptions window;
  // The first path uses synchronous GPU fences and TWO pending Wayland
  // receipts. Deferred scene fences are rejected, so frame release is explicit.
  std::uint64_t gpu_completion_timeout_ns = 100000000;
  std::function<void(const DisplayReceipt&)> receipt;
  // Optional bounded local snapshot only. No socket I/O belongs in this call.
  // Absence preserves the existing camera-only presentation.
  std::function<ManualCloseState()> manual_close_state;
};
struct DisplayStatistics {
  std::uint64_t submissions = 0, presentations = 0, fresh_camera_presentations = 0;
  std::uint64_t missing_presentations = 0, stale_presentations = 0, max_capture_age_ns = 0;
  unsigned pending = 0, maximum_pending = 0;
  bool faulted = false;
};
// Linux GPU owner. Queue must outlive this display and have only one stage/
// completion owner. The library never starts or changes a system service.
class NativeDisplay {
 public:
  NativeDisplay(Configuration, Queue&, DisplayOptions = {});
  ~NativeDisplay();
  NativeDisplay(const NativeDisplay&) = delete;
  NativeDisplay& operator=(const NativeDisplay&) = delete;
  // Poll receipts nonblocking, then compose at most one new frame/status scene.
  // The source and availability use the genuine local monotonic clock.
  void turn();
  DisplayStatistics statistics() const;
  std::vector<headunit::TouchInput> take_touches();
  std::string renderer() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::camera
