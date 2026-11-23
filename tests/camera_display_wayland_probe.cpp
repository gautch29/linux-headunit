// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_display.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace c = headunit::camera;
using Clock = std::chrono::steady_clock;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
std::uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}
int main() try {
  c::Configuration config;
  config.enabled = true;
  config.manual_activation = true;
  config.width = 320;
  config.height = 180;
  config.canvas_width = 640;
  config.canvas_height = 360;
  config.destination = {80, 36, 480, 288};
  config.frame_rate = 30;
  c::Queue queue(config);
  c::SyntheticProvider provider(config);
  c::DisplayOptions options;
  options.window.title = "Headunit isolated camera probe";
  options.gpu_completion_timeout_ns = 2000000000;
  options.receipt = [](const c::DisplayReceipt& event) {
    std::cout << "camera_receipt token=" << event.token << " actualPresented=" << event.presented
              << " source=" << int(event.source) << " simulated=" << event.simulated
              << " freshCamera=" << event.fresh_camera << " flags=" << event.flags;
    if (event.capture_age_ns) std::cout << " captureAgeNs=" << *event.capture_age_ns;
    std::cout << '\n';
  };
  c::NativeDisplay display(config, queue, std::move(options));
  std::cout << "camera_renderer=" << display.renderer() << '\n';
  const auto run = [&](bool produce, const auto& done) {
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (!done(display.statistics())) {
      check(Clock::now() < deadline, "isolated camera phase exceeded deadline");
      if (produce) provider.poll(now(), queue);
      display.turn();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  };
  run(false, [](const auto& stats) { return stats.missing_presentations > 0; });
  run(true, [](const auto& stats) { return stats.fresh_camera_presentations >= 3; });
  run(false, [](const auto& stats) { return stats.stale_presentations > 0; });
  const auto before_recovery = display.statistics().fresh_camera_presentations;
  run(true,
      [&](const auto& stats) { return stats.fresh_camera_presentations >= before_recovery + 3; });
  const auto until = Clock::now() + std::chrono::seconds(2);
  while (display.statistics().pending && Clock::now() < until) {
    display.turn();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto stats = display.statistics();
  check(stats.missing_presentations > 0 && stats.stale_presentations > 0 &&
            stats.fresh_camera_presentations > before_recovery && stats.maximum_pending <= 2 &&
            !stats.faulted && queue.statistics().maximum_outstanding_leases <= 4,
        "native camera recovery or bounded ownership failed");
  std::cout << "Isolated actual Wayland camera/missing/stale/recovery receipts passed; no physical "
               "scanout claim\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
