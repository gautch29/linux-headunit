// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/touch_mailbox.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <latch>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
// Isolated mailbox delivery experiment. No phone, rendering or panel timing.
void run(bool wakeup) {
  headunit::TouchMailbox box;
  const auto generation = box.begin();
  std::vector<double> delays;
  for (unsigned i = 0; i < 64; ++i) {
    std::latch started(1);
    Clock::time_point enqueued;
    std::thread producer([&] {
      started.wait();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      enqueued = Clock::now();
      const std::array reports{headunit::TouchInput{true, 0, 0}, headunit::TouchInput{false, 0, 0}};
      box.push(generation, reports);
    });
    pollfd descriptor{box.wake_fd(), POLLIN, 0};
    started.count_down();
    const auto result = ::poll(wakeup ? &descriptor : nullptr, wakeup ? 1 : 0, 10);
    producer.join();
    const auto received = Clock::now();
    if (result < 0 || (wakeup && result != 1) || box.drain([](auto) { return true; }) != 2)
      throw std::runtime_error("touch wakeup benchmark lost report");
    delays.push_back(std::chrono::duration<double, std::milli>(received - enqueued).count());
  }
  std::sort(delays.begin(), delays.end());
  std::cout << "{\"mode\":\"" << (wakeup ? "mailbox-wakeup" : "timer-only-10ms")
            << "\",\"trials\":" << delays.size() << ",\"p50Ms\":" << delays[delays.size() / 2]
            << ",\"p95Ms\":" << delays[(delays.size() * 95 + 99) / 100 - 1]
            << ",\"maxMs\":" << delays.back()
            << ",\"physicalTouchLatency\":false,\"phoneConnected\":false}\n";
}
int main() {
  run(false);
  run(true);
}
