// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/standard_preview_worker.hpp>
#include <headunit/camera_control.hpp>
#include <memory>

namespace headunit {
class GstPreviewOutput;
// Control-owner adapter. The borrowed output and original decoder pipeline
// outlive this object; pipeline reaches NULL before either owner is destroyed.
// All calls are sequential on the control owner, not a GStreamer callback.
class StandardPreview {
 public:
  struct Options {
    StandardPreviewWorker::Options presenter;
    // Startup-owned private reader, validated before the receiver listens.
    // When present, presenter.surface_lease_file and named output are required.
    std::shared_ptr<const camera::ActivationControl> camera_control;
  };
  enum class Stage { initial, visible, hidden, returning, faulted };
  struct Status {
    std::uint64_t window_generation = 1;
    bool camera_active = false;
    bool native_page_active = false;
    Stage stage = Stage::initial;
    StandardPreviewWorker::Status presenter;
  };
  // Before PLAYING. Initial active camera leaves the permanent decoder drain;
  // otherwise installs a new bounded decoded-image adapter before streaming.
  StandardPreview(GstPreviewOutput&, Options);
  ~StandardPreview();
  StandardPreview(const StandardPreview&) = delete;
  StandardPreview& operator=(const StandardPreview&) = delete;
  // Pump on regular control turns, including while no video/input arrives.
  // Faults retire optional main presentation and throw to its containing owner.
  void pump();
  // Terminal optional-output cleanup. Keep this owner alive to retry its
  // retained HID cancellation until accepted or the phone session closes.
  void retire() noexcept;
  int input_fd() const noexcept;
  unsigned drain_touches(const std::function<bool(TouchInput)>&);
  Status status() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
