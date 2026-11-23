// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "../src/theme/wayland_window.hpp"
#include <headunit/presentation_feedback_timeout.hpp>
#include <headunit/theme/local_presentation_diagnostics.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace headunit::theme::graphics;
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
PresentationReceipt collect(WaylandWindow& window) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto receipt = window.poll_receipt()) return *receipt;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("private window did not receive presentation feedback");
}
int main(int argc, char** argv) try {
  check(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--layer"),
        "invalid private fixture mode");
  const bool local = argc == 2;
  const Image image{64, 64, std::vector<std::uint32_t>(4096, 0xffff0000)};
  LocalPresentationDiagnostics observed;
  observed.entered_ns = local_diagnostic_clock();
  {
    WaylandWindow window(64, 64, "Private local diagnostic SHM fixture", 1, {}, false, 2,
                         "headunit.backup-cluster", {}, false, true, local, &observed);
    window.submit_image(1, image, true);
    const auto first = collect(window);
    check(first.token == 1 && first.presented, "first real presentation was lost");
    window.submit_image(2, image, true);
    window.submit_image(3, image, true);
    check(!window.can_submit(), "existing two-request bound changed");
    check(collect(window).token == 2 && collect(window).token == 3,
          "actual bounded receipt order changed");
    check(!window.pending() && !window.poll_receipt(), "consumed feedback reappeared");
    if (local) {
      check(observed.first_request && observed.first_request->token == 1 &&
                observed.first_request->observed_ns && observed.first_feedback &&
                observed.first_feedback->raw.token == 1 && !observed.first_feedback->discarded &&
                observed.first_feedback->observed_ns && observed.first_returned_receipt &&
                observed.first_returned_receipt->token == 1,
            "local original first observations were lost or renewed");
      check(
          *observed.first_request->observed_ns <= *observed.first_feedback->observed_ns &&
              *observed.first_feedback->observed_ns <= observed.first_returned_receipt->received_ns,
          "real original dates were reordered");
      check(!observed.first_swap_return, "SHM submission invented an EGL swap");
    } else {
      check(!observed.first_request && !observed.first_feedback &&
                !observed.first_returned_receipt && !observed.last_dispatch_before_ns &&
                !observed.last_dispatch_after_ns,
            "foreground phone path enabled a local observer");
    }
  }
  LocalPresentationDiagnostics timed;
  timed.entered_ns = local_diagnostic_clock();
  {
    WaylandWindow window(64, 64, "Private absolute deadline fixture", 1, {}, false, 1,
                         "headunit.backup-cluster", {}, false, true, local, &timed);
    window.submit_image(7, image, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    bool rejected = false;
    try {
      (void)window.poll_receipt();
    } catch (const headunit::PresentationFeedbackTimeout&) {
      rejected = true;
    }
    check(rejected, "a queued old event bypassed the absolute deadline before dispatch");
    if (local) {
      check(timed.timeout && timed.timeout->oldest_token == 7 &&
                timed.timeout->pending_count == 1 && !timed.timeout->resolved &&
                timed.timeout->observed_ns && timed.first_request &&
                timed.first_request->observed_ns &&
                *timed.timeout->observed_ns >= *timed.first_request->observed_ns + 2000000000,
            "actual oldest-pending timeout snapshot is incorrect");
      check(!timed.first_feedback && !timed.first_returned_receipt && !timed.first_swap_return,
            "timeout observation drained or manufactured an old event");
    } else
      check(!timed.timeout && !timed.first_request, "shared path collected local timeout data");
  }
  // Both windows/proxies are gone before the bounded records are encoded.
  std::cout << "Private real Wayland/SHM observations; no signed asset, GPU or optical claim\n";
  write_local_presentation_diagnostics(std::cout, observed);
  write_local_presentation_diagnostics(std::cout, timed);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
