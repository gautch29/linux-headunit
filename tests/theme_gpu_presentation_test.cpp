// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_compositor.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <time.h>

using namespace headunit::theme::graphics;
using Clock = std::chrono::steady_clock;
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <class F>
void rejects(F&& action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "invalid presentation operation was accepted");
}
PresentationReceipt collect(GpuCompositor& output) {
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  for (;;) {
    if (auto receipt = output.poll_presentation()) return *receipt;
    check(Clock::now() < deadline, "test receipt deadline exceeded");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
int main() {
  try {
    GpuCompositor first(64, 64, WaylandOptions{"Headunit asynchronous presentation A"});
    GpuCompositor second(64, 64, WaylandOptions{"Headunit asynchronous presentation B"});
    const Image red{64, 64, std::vector<std::uint32_t>(4096, 0xffff0000)},
        blue{64, 64, std::vector<std::uint32_t>(4096, 0xff0000ff)};
    check(!first.presentation_pending() && !first.poll_presentation(), "idle window has a receipt");
    first.compose_reference(red);
    Image verification;
    first.submit(1, &verification);
    check(verification.pixels == red.pixels && first.presentation_pending(),
          "first submission lost its frame");
    // The other display can submit while A's receipt is still outstanding.
    second.compose_reference(blue);
    second.submit(7, &verification);
    check(verification.pixels == blue.pixels && second.presentation_pending(),
          "second submission lost its frame");
    first.compose_reference(blue);
    rejects([&] { first.submit(2); });  // Does not consume the newly composed frame.
    const auto b = collect(second), a = collect(first);
    check(a.token == 1 && b.token == 7 && a.presented && b.presented && a.correlation &&
              b.correlation,
          "actual presentation feedback was lost or mixed across displays");
    if (std::getenv("HEADUNIT_THEME_TEST_EXPECT_MONOTONIC"))
      check(a.clock_id == CLOCK_MONOTONIC && b.clock_id == CLOCK_MONOTONIC,
            "native-clock fixture did not provide CLOCK_MONOTONIC receipts");
    // CLOCK_MONOTONIC receipts already use the host's monotonic domain. A
    // three-read midpoint estimate introduces an artificial offset that can
    // reverse two receipts for the same compositor presentation timestamp.
    for (const auto& receipt : {a, b}) {
      if (receipt.clock_id == CLOCK_MONOTONIC) {
        const auto& sample = *receipt.correlation;
        check(sample.before_ns == sample.source_ns && sample.source_ns == sample.after_ns,
              "native monotonic presentation clock acquired a synthetic conversion offset");
      }
    }
    check(!first.presentation_pending() && !second.presentation_pending(),
          "consumed receipt remains pending");
    first.submit(2, &verification);
    check(verification.pixels == blue.pixels, "busy submission overwrote the next frame");
    const auto next = collect(first);
    check(next.token == 2 && next.presented && next.received_ns >= a.received_ns,
          "next actual receipt is invalid");
    const auto began = Clock::now();
    for (int i = 0; i < 100; ++i)
      check(!first.poll_presentation() && !second.poll_presentation(), "receipt delivered twice");
    check(Clock::now() - began < std::chrono::milliseconds(300),
          "idle presentation polling waited for an event");
    if (std::getenv("HEADUNIT_THEME_TEST_TWO_PENDING")) {
      WaylandOptions options{"Headunit bounded two-frame presentation"};
      options.max_pending_presentations = 2;
      GpuCompositor pipelined(64, 64, std::move(options));
      pipelined.compose_reference(red);
      pipelined.submit(1);
      pipelined.compose_reference(blue);
      pipelined.submit(2);
      check(!pipelined.presentation_slot_available(), "two-frame queue exceeded its bound");
      pipelined.compose_reference(red);
      rejects([&] { pipelined.submit(3); });
      const auto oldest = collect(pipelined), newest = collect(pipelined);
      check(oldest.token == 1 && newest.token == 2 && newest.presented &&
                !pipelined.presentation_pending() && pipelined.presentation_slot_available(),
            "two-frame receipts were lost, reordered or fabricated");
      pipelined.submit(3);
      check(collect(pipelined).token == 3, "two-frame queue did not recover");
      std::cout << "twoFrameOldestPresented=" << oldest.presented << '\n';
    }
    // Existing synchronous probe callers remain usable on the same connection.
    second.compose_reference(red);
    check(second.present(8).token == 8, "blocking compatibility failed");
    // Deadline belongs to the submission, even if the owner stops polling.
    // A queued old event must not become a fresh receipt after an arbitrary delay.
    first.compose_reference(red);
    first.submit(3);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    rejects([&] { first.poll_presentation(); });
    rejects([&] { first.compose_reference(red); });
    std::cout << "Asynchronous presentation: independent windows, exact blits, once-only actual "
                 "receipts, busy recovery and absolute deadline passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
