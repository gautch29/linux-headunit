// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "../src/theme/wayland_window.hpp"
#include <headunit/presentation_feedback_timeout.hpp>
#include <headunit/theme/display_video_input.hpp>
#include <time.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace headunit::theme::graphics;
namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("Wayland timeout classification check failed");
}
}  // namespace
int main() try {
  // Existing bounded wl_shm submission on the production window. No GPU/VM,
  // signed pack, phone clock/video or hardware-renderer policy bypass.
  WaylandWindow window(64, 64, "Production pending feedback timeout");
  const Image red{64, 64, std::vector<std::uint32_t>(4096, 0xffff0000)};
  const auto sampled = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
  headunit::theme::video::MediaClock media;
  media.update({true, std::uint64_t{2208989800} << 32, sampled, sampled, sampled, 1000});
  headunit::theme::video::PresentationClock mapper;
  window.submit_image(17, red, true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool presented = false;
  while (!presented) {
    if (auto receipt = window.poll_receipt()) {
      check(receipt->token == 17 && receipt->presented && receipt->correlation);
      const auto mapped = mapper.accept(*receipt, media);
      check(mapped && mapped->token == 17 && mapped->monotonic_ns <= receipt->received_ns);
      const auto& c = *receipt->correlation;
      if (receipt->clock_id == CLOCK_MONOTONIC) {
        check(c.before_ns == c.source_ns && c.after_ns == c.source_ns);
        check(mapped->monotonic_ns == receipt->seconds * 1000000000 + receipt->nanoseconds);
      }
      const headunit::theme::video::InputPresentationReceipt copied{
          media.suspension_epoch(), media.suspension_epoch(), sampled, *receipt, *mapped};
      check(copied.feedback.token == mapped->token &&
            copied.feedback.received_ns == receipt->received_ns);
      bool replayed = false;
      try {
        (void)mapper.accept(copied.feedback, media);
      } catch (const std::exception&) {
        replayed = true;
      }
      check(replayed && mapper.failed());
      presented = true;
    }
    check(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(!window.pending() && !window.poll_receipt());
  window.submit_image(18, red, true);
  // The submission owns its deadline even when the caller does not service
  // feedback. A queued old event must not turn into a fresh accepted receipt.
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  bool unresolved_timeout = false;
  try {
    (void)window.poll_receipt();
  } catch (const headunit::PresentationFeedbackTimeout&) {
    unresolved_timeout = true;
  }
  check(unresolved_timeout);
  bool invalid_configuration = false;
  try {
    WaylandWindow invalid(0, 64, "Invalid configuration never grants retry");
  } catch (const headunit::PresentationFeedbackTimeout&) {
    check(false);
  } catch (const std::exception&) {
    invalid_configuration = true;
  }
  check(invalid_configuration);
  std::cout << "Production wl_shm window: real receipt, once-only delivery, absolute pending "
               "deadline and permanent configuration fault classification passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
