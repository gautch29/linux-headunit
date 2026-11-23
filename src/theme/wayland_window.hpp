// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/gpu_compositor.hpp>

namespace headunit::theme::graphics {
class WaylandWindow {
 public:
  WaylandWindow(std::uint32_t width, std::uint32_t height, const std::string& title,
                unsigned buffer_scale = 1, const std::string& display_name = {},
                bool accept_touch = false, unsigned max_pending = 1,
                const std::string& app_id = "org.headunit.ultra.display",
                const std::string& output_name = {}, bool touch_diagnostics = false,
                bool opaque_background = false, bool background_layer = false,
                LocalPresentationDiagnostics* local_diagnostics = nullptr);
  ~WaylandWindow();
  WaylandWindow(const WaylandWindow&) = delete;
  WaylandWindow& operator=(const WaylandWindow&) = delete;
  void* display() const;
  void* window() const;
  // Actual EGL buffer dimensions after the compositor's acknowledged configure.
  std::pair<std::uint32_t, std::uint32_t> buffer_size() const;
  // Stages the region with the caller's next buffer commit, after validating
  // the request. Resize/configure uses this current policy, not the initial one.
  void request(std::uint64_t token, bool opaque_background);
  void observe_egl_swap_return(std::uint64_t token, bool success) noexcept;
  // Bounded desktop preview only. Owns at most three shared-memory buffers;
  // a buffer is never rewritten before the compositor releases it.
  void submit_image(std::uint64_t token, const Image&, bool opaque_background);
  std::vector<headunit::TouchInput> take_touches(bool enabled);
  bool pending() const;
  bool can_submit() const;
  // Change the outstanding-presentation bound (1 or 2) between submissions.
  // Lowering it never cancels a pending request; submission waits until the
  // queue drains below the new bound.
  void set_max_pending(unsigned max_pending);
  // Dispatch available events without waiting. Exactly one ordered receipt per request;
  // the two-second deadline starts at request, not at the next poll.
  std::optional<PresentationReceipt> poll_receipt(bool wait_for_visibility = false);
  bool feedback_delayed() const;
  PresentationReceipt wait();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::graphics
