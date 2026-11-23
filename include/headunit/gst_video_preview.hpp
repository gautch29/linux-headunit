// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/airplay_video_stream.hpp"
#include "headunit/h264_annexb.hpp"
#include "headunit/touch_input.hpp"
#include "headunit/main_preview_options.hpp"
#include "headunit/presentation_status.hpp"

#include <string>
#include <chrono>
#include <optional>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <functional>

typedef struct _GstElement GstElement;
typedef struct _GstEvent GstEvent;

namespace headunit {
class RouteGuidance;
class GstPreviewOutput;
#ifdef HEADUNIT_HAVE_STANDARD_GPU
class StandardPreview;
#endif
#ifdef HEADUNIT_HAVE_WAYLAND_PREVIEW
class WaylandPreview;
#endif

// Optional Linux bench renderer. The protocol receiver remains independent of
// this display backend; production KMS output can replace it later.
class GstVideoPreview {
 public:
  explicit GstVideoPreview(const std::string& sink, unsigned width = 1280, unsigned height = 720,
                           const std::string& decoder = "v4l2h264dec",
                           const std::string& wayland_display = {},
                           MainPreviewOptions main_options = {});
  ~GstVideoPreview();
  GstVideoPreview(const GstVideoPreview&) = delete;
  GstVideoPreview& operator=(const GstVideoPreview&) = delete;
  void submit(const AirplayVideoPacket& packet);
  std::size_t finish(unsigned timeout_ms = 5000);
  std::size_t decoded_frames() const { return decoded_frames_.load(); }
  // -1 for preview backends without a pollable input connection.
  int input_fd() const noexcept;
  void set_navigation(std::shared_ptr<RouteGuidance> source);
  // Legacy backends only. The owned route must retain rejected reports.
  std::vector<TouchInput> take_touches();
  // Owned Standard route only; sender executes on the phone control owner.
  unsigned drain_touches(const std::function<bool(TouchInput)>& sender);
  // Stop optional rendering without losing a backpressured contact release.
  // The connection retains this owner until delivery or session teardown.
  void retire() noexcept;
  // Control owner only. Only the owned Standard route can prove presentation.
  // Hidden Home retains eligibility only while its original decoder is healthy.
  MainPresentationStatus main_presentation_status();

 private:
  H264AnnexB converter_;
  GstElement* pipeline_ = nullptr;
  GstElement* source_ = nullptr;
  std::unique_ptr<GstPreviewOutput> output_;
#ifdef HEADUNIT_HAVE_STANDARD_GPU
  std::unique_ptr<StandardPreview> standard_;
  // Private bench trace only. Its lifetime budget never resets on Home cycles.
  bool standard_diagnostics_ = false, standard_diagnostic_home_seen_ = false;
  unsigned standard_diagnostic_reports_ = 0;
  std::uint64_t standard_diagnostic_flags_ = 0, standard_diagnostic_window_ = 0;
  std::uint64_t standard_diagnostic_requested_ = 0, standard_diagnostic_next_ns_ = 0;
  std::uint64_t standard_encoded_ingress_ = 0;
  std::optional<std::uint64_t> standard_last_ingress_ns_, standard_last_ingress_running_ns_;
  void trace_standard_return(bool epoch_ready, bool healthy, bool visible) noexcept;
#endif
  bool retired_ = false;
  bool pipeline_failed_ = false;
  bool eos_seen_ = false;
  [[maybe_unused]] bool main_presented_ = false;  // Used only with the Standard GPU presenter.
  [[maybe_unused]] bool main_hidden_ = false;     // Standard GPU presenter withdrawn by Home.
  [[maybe_unused]] std::uint64_t main_returns_ = 0;
  std::atomic<std::size_t> decoded_frames_{0};
  void check_pipeline_error();
  void navigation(GstEvent* event);
  std::mutex touch_mutex_;
  std::vector<TouchInput> pending_touches_;
  bool pointer_down_ = false;
  unsigned width_;
  unsigned height_;
  std::vector<std::uint8_t> configuration_;
  std::optional<std::chrono::steady_clock::time_point> first_frame_;
#ifdef HEADUNIT_HAVE_WAYLAND_PREVIEW
  std::unique_ptr<WaylandPreview> wayland_;
#endif
};

}  // namespace headunit
