// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/night_mode.hpp"
#include "headunit/limited_ui.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void check(bool valid, const char* reason) {
  if (!valid) throw std::runtime_error(reason);
}
}  // namespace

int main() try {
  using headunit::NightModePolicy;
  const auto off = headunit::parse_night_mode_policy("off");
  const auto on = headunit::parse_night_mode_policy("on");
  check(off && !headunit::night_mode_at(*off, 0) && !headunit::night_mode_at(*off, 23 * 60),
        "off policy reported night");
  check(on && headunit::night_mode_at(*on, 12 * 60), "on policy reported day");
  const auto schedule = headunit::parse_night_mode_policy("schedule");
  check(schedule && schedule->start_minute == 20 * 60 && schedule->end_minute == 7 * 60,
        "default schedule changed");
  check(headunit::night_mode_at(*schedule, 23 * 60) &&
            headunit::night_mode_at(*schedule, 6 * 60 + 59) &&
            !headunit::night_mode_at(*schedule, 7 * 60) &&
            !headunit::night_mode_at(*schedule, 19 * 60 + 59) &&
            headunit::night_mode_at(*schedule, 20 * 60),
        "wrapping schedule boundaries");
  const auto daytime = headunit::parse_night_mode_policy("schedule:09:30-17:00");
  check(daytime && headunit::night_mode_at(*daytime, 9 * 60 + 30) &&
            !headunit::night_mode_at(*daytime, 17 * 60) &&
            !headunit::night_mode_at(*daytime, 9 * 60),
        "non-wrapping schedule boundaries");
  for (const auto* invalid : {"", "auto", "ON", "schedule:", "schedule:20:00-20:00",
                              "schedule:24:00-07:00", "schedule:20:60-07:00", "schedule:2000-0700",
                              "schedule:20:00_07:00", "schedule:20:00-07:00 "})
    check(!headunit::parse_night_mode_policy(invalid), "invalid night-mode policy accepted");
  const auto lamps = headunit::parse_night_mode_policy("headlights");
  check(lamps && headunit::night_mode_at(*lamps, 12 * 60, true) &&
            !headunit::night_mode_at(*lamps, 23 * 60, false) &&
            headunit::night_mode_at(*lamps, 23 * 60) && !headunit::night_mode_at(*lamps, 12 * 60),
        "headlight policy or its unknown-lamp schedule fallback");
  // Driving restrictions: hysteresis and unknown speed keep the current state.
  check(!headunit::parse_limited_ui_policy("on") && !headunit::parse_limited_ui_policy(""),
        "invalid limited-UI policy accepted");
  const auto moving = *headunit::parse_limited_ui_policy("speed");
  check(!headunit::limited_ui_next(moving, false, 4.9) &&
            headunit::limited_ui_next(moving, false, 5.1),
        "restriction entry threshold");
  check(headunit::limited_ui_next(moving, true, 3) && !headunit::limited_ui_next(moving, true, 1.9),
        "restriction exit threshold");
  check(headunit::limited_ui_next(moving, true, std::nullopt) &&
            !headunit::limited_ui_next(moving, false, std::nullopt) &&
            headunit::limited_ui_next(moving, true, -1.0),
        "unknown or invalid speed changed restrictions");
  check(!headunit::receiver_night_mode_now(), "unconfigured receiver declared night mode");
  headunit::set_receiver_night_mode(on);
  check(headunit::receiver_night_mode_now() == true, "configured receiver policy ignored");
  headunit::set_receiver_night_mode(std::nullopt);
  check(!headunit::receiver_night_mode_now(), "cleared receiver policy retained");
  std::cout << "night-mode policy parsing, schedules and receiver setting passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
