// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/preview_touch.hpp"
#include <limits>
#include <stdexcept>
using headunit::PreviewTouch;
void check(bool v) {
  if (!v) throw std::runtime_error("preview touch check failed");
}
int main() {
  PreviewTouch t(1280, 720);
  t.resize(1024, 768);
  const auto r = t.rectangle();
  check(r.x == 0 && r.y == 96 && r.width == 1024 && r.height == 576);
  t.down(0, 512, 20);
  t.motion(0, 512, 384);
  t.up(0);
  check(t.take().empty());  // Bar is not CarPlay.
  t.down(7, 512, 384);
  t.down(8, 10, 110);
  t.motion(8, 100, 110);
  t.up(8);
  auto events = t.take();
  check(events.size() == 1 && events[0].pressed && events[0].x == 640 && events[0].y == 360);
  t.motion(7, 9999, -10);
  t.up(7);
  events = t.take();
  check(events.size() == 2 && events[0].pressed && events[0].x == 1279 && events[0].y == 0 &&
        !events[1].pressed);
  t.down(1, 512, 384);
  t.cancel();
  events = t.take();
  check(events.size() == 2 && !events.back().pressed);
  t.down(1, 512, 384);
  t.take();
  t.resize(720, 1280);
  events = t.take();
  check(events.size() == 1 && !events[0].pressed);
  t.down(1, std::numeric_limits<double>::quiet_NaN(), 0);
  check(t.take().empty());
  t.resize(1280, 720);
  t.down(-1, 0, 0);
  t.motion(-1, 1279, 719);
  t.up(-1);
  events = t.take();
  check(events.size() == 3 && events[0].x == 0 && events[1].x == 1279 && events[1].y == 719 &&
        !events[2].pressed);
  t.down(9, 400, 300);
  t.take();
  for (int i = 0; i < 100; ++i)
    t.motion(9, i, 300);
  events = t.take();
  check(events.size() <= 64 && !events.back().pressed);
  t.down(9, 20, 20);
  t.up(9);
  events = t.take();
  check(events.size() == 2 && events.front().pressed && !events.back().pressed);
  check(!events.front().second_pressed);  // Single touch never reports a second finger.

  // Two-finger digitizer: each change reports both contacts; a third is ignored.
  headunit::set_multitouch(true);
  PreviewTouch m(1280, 720);
  m.down(1, 100, 100);
  m.down(2, 600, 300);
  m.down(3, 700, 400);
  m.motion(2, 640, 360);
  m.up(1);
  m.down(4, 10, 10);  // Reuses the first slot.
  m.up(2);
  m.cancel();
  events = m.take();
  check(events.size() == 7);
  check(events[0].pressed && !events[0].second_pressed && events[0].x == 100);
  check(events[1].pressed && events[1].second_pressed && events[1].second_x == 600 &&
        events[1].second_y == 300);
  check(events[2].second_x == 640 && events[2].second_y == 360 && events[2].x == 100);
  check(!events[3].pressed && events[3].second_pressed);
  check(events[4].pressed && events[4].x == 10 && events[4].second_pressed);
  check(events[5].pressed && !events[5].second_pressed);
  check(!events[6].pressed && !events[6].second_pressed);
  headunit::set_multitouch(false);
}
