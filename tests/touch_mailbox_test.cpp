// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/touch_mailbox.hpp>
#include <headunit/preview_touch.hpp>
#include <array>
#include <stdexcept>
#include <thread>
#include <vector>
#include <poll.h>
#include <latch>
using namespace headunit;
void check(bool v) {
  if (!v) throw std::runtime_error("touch mailbox check failed");
}
int main() {
  TouchMailbox box;
  auto generation = box.begin();
  std::vector<TouchInput> sent;
  auto accept = [&](const TouchInput& t) {
    sent.push_back(t);
    return true;
  };
  const std::array press{TouchInput{true, 960, 360}};
  box.push(generation, press);
  check(box.drain([](auto) { return false; }) == 0);  // Socket backpressure retains the press.
  check(box.drain(accept) == 1 && sent.back().pressed);
  box.cancel(generation);
  check(box.drain([](auto) { return false; }) == 0);
  box.cancel(generation);  // Retry cancellation must retain an undelivered release.
  check(box.drain(accept) == 1 && !sent.back().pressed && sent.back().x == 960);
  box.push(generation, press);
  box.drain(accept);
  const auto next = box.begin();
  box.push(generation, press);
  box.cancel(generation);  // Stale worker cannot affect successor.
  check(box.drain(accept) == 1 && !sent.back().pressed);
  box.push(next, press);
  box.drain(accept);
  std::array<TouchInput, 100> motions{};
  for (auto& t : motions)
    t = {true, 100, 200};
  box.push(next, motions);
  check(box.drain(accept) == 1 && !sent.back().pressed);
  box.push(next, press);
  check(box.drain(accept) == 0);  // Ignore rest of overloaded gesture.
  const std::array release{TouchInput{false, 100, 200}};
  box.push(next, release);
  box.push(next, press);
  check(box.drain(accept) == 1 && sent.back().pressed);
  box.retire();
  box.push(next, press);
  check(box.drain(accept) == 1 && !sent.back().pressed);
  // Mapping for the signed pack's 1920x720 scene on the 1280x720 CM5 output.
  PreviewTouch touch(1920, 720);
  touch.resize(1280, 720);
  const auto r = touch.rectangle();
  check(r.x == 0 && r.y == 120 && r.width == 1280 && r.height == 480);
  generation = box.begin();
  touch.down(1, 640, 100);
  touch.up(1);
  box.push(generation, touch.take());
  check(box.drain(accept) == 0);
  touch.down(1, 640, 360);
  box.push(generation, touch.take());
  box.drain(accept);
  check(sent.back().pressed && sent.back().x == 960 && sent.back().y == 360);
  touch.resize(960, 360);
  box.push(generation, touch.take());
  box.drain(accept);
  check(!sent.back().pressed);
  touch.down(1, 480, 180);
  touch.up(1);
  box.push(generation, touch.take());
  check(box.drain(accept) == 2);
  check(sent[sent.size() - 2].x == 960 &&
        sent[sent.size() - 2].y == 360);  // scale=2 logical coordinates.
  // Concurrent worker teardown and receiver drain cannot resurrect a gesture.
  generation = box.begin();
  std::thread producer([&] {
    for (unsigned i = 0; i < 1000; ++i) {
      box.push(generation, press);
      box.push(generation, release);
    }
    box.cancel(generation);
  });
  for (unsigned i = 0; i < 1000; ++i)
    box.drain(accept);
  producer.join();
  box.drain(accept);
  check(!sent.back().pressed);

  // A real receiver wait is released by another thread's new report, without
  // a network packet or timer. Rejecting delivery consumes readiness without
  // losing the gesture or spinning the poll loop under socket backpressure.
  TouchMailbox wake_box(true);
  const auto wake_generation = wake_box.begin();
  check((fcntl(wake_box.wake_fd(), F_GETFL) & O_NONBLOCK) != 0);
  check((fcntl(wake_box.wake_fd(), F_GETFD) & FD_CLOEXEC) != 0);
  for (unsigned i = 0; i < 128; ++i) {
    pollfd fd{wake_box.wake_fd(), POLLIN, 0};
    check(poll(&fd, 1, 0) == 0);
    std::latch ready(1);
    std::thread worker([&] {
      ready.wait();
      wake_box.push(wake_generation, press);
    });
    ready.count_down();
    const auto result = poll(&fd, 1, 1000);
    worker.join();
    check(result == 1 && (fd.revents & POLLIN));
    check(wake_box.drain([](auto) { return false; }) == 0);
    fd.revents = 0;
    check(poll(&fd, 1, 0) == 0);
    check(wake_box.drain(accept) == 1 && sent.back().pressed);
    wake_box.cancel(wake_generation);
    fd.revents = 0;
    check(poll(&fd, 1, 0) == 1);
    check(wake_box.drain(accept) == 1 && !sent.back().pressed);
  }
  const auto timing = wake_box.take_timing();
  check(timing.reports == 256 && timing.max_ns > 0 && timing.total_ns >= timing.max_ns);
  check(timing.percentile_upper_ns(50) > 0 &&
        timing.percentile_upper_ns(95) >= timing.percentile_upper_ns(50));
  check(wake_box.take_timing().reports == 0 && box.take_timing().reports == 0);
  wake_box.push(wake_generation, press);
  wake_box.drain(accept);
  wake_box.push(wake_generation, press);
  wake_box.drain([](auto) { return false; });
  wake_box.cancel(wake_generation);
  pollfd cancellation{wake_box.wake_fd(), POLLIN, 0};
  check(poll(&cancellation, 1, 0) == 1);
  check(wake_box.drain(accept) == 1 && !sent.back().pressed);
  wake_box.retire();
  wake_box.push(wake_generation, press);
  pollfd stale{wake_box.wake_fd(), POLLIN, 0};
  check(poll(&stale, 1, 0) == 0);
  int closed_fd = -1;
  {
    TouchMailbox temporary;
    closed_fd = temporary.wake_fd();
  }
  check(fcntl(closed_fd, F_GETFD) == -1 && errno == EBADF);
}
