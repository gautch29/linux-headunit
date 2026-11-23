// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_timing.hpp"

#include <stdexcept>
#include <source_location>
#include <string>

namespace {
constexpr std::uint64_t second = 1000000000;
constexpr std::uint64_t epoch = std::uint64_t{2208988800} << 32;
std::uint64_t ticks(std::uint64_t ns) {
  return ((ns / second) << 32) + ((ns % second) << 32) / second;
}
void check(bool value, std::source_location at = std::source_location::current()) {
  if (!value)
    throw std::runtime_error("host clock diagnostic failed at line " + std::to_string(at.line()));
}
using Monitor = headunit::AirplayHostClockMonitor;
Monitor::Reading reading(std::uint64_t at, std::uint64_t wall) {
  // Explicit synthetic host clocks, bracketed around a 200 ns wall read.
  return {epoch + ticks(wall + 100), at, at + 200};
}
void monotonic_timeline() {
  using headunit::AirplayTiming;
  constexpr std::uint64_t second = 1000000000ULL;
  constexpr std::uint64_t anchor = (3970000000ULL << 32) | 0x80000000ULL;
  check(AirplayTiming::ntp_at(anchor, 5 * second, 5 * second) == anchor);
  // 1.5 s later is exactly 1.5 NTP seconds later, whatever the wall clock did.
  check(AirplayTiming::ntp_at(anchor, 5 * second, 6 * second + second / 2) ==
        anchor + (1ULL << 32) + (1ULL << 31));
  // Monotonic and sub-microsecond accurate: 1 ns advances at most one tick.
  const auto a = AirplayTiming::ntp_at(anchor, 0, 123456789);
  const auto b = AirplayTiming::ntp_at(anchor, 0, 123456790);
  check(b >= a && b - a <= 5);
  // NTP-era wrap is carried, not rejected.
  const std::uint64_t late = ~0ULL - (1ULL << 31);
  check(AirplayTiming::ntp_at(late, 0, second) == late + (1ULL << 32));
  bool rejected = false;
  try {
    (void)AirplayTiming::ntp_at(anchor, 2 * second, second);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
}
void host_clock_corrections() {
  for (bool backwards : {false, true}) {
    Monitor monitor;
    check(!monitor.observe(reading(10 * second, 10 * second)));
    check(!monitor.observe(reading(11 * second, 11 * second)));
    const auto wall = backwards ? 12 * second - 500000000 : 12 * second + 500000000;
    const auto correction = monitor.observe(reading(12 * second, wall));
    check(correction.has_value());
    check(correction->delta_ns >= (backwards ? -500000001LL : 499999999LL) &&
          correction->delta_ns <= (backwards ? -499999999LL : 500000001LL));
    check(correction->age_ns == second && correction->previous_read_bracket_ns == 200 &&
          correction->read_bracket_ns == 200);
    // Monitoring adopts the actual next reading without retiring any clock.
    check(!monitor.observe(reading(13 * second, wall + second)));
  }
  Monitor bounded;
  check(!bounded.observe(reading(10 * second, 10 * second)));
  for (unsigned i = 1; i <= 10; ++i) {
    const auto correction = bounded.observe(reading((10 + i) * second, (10 + 2 * i) * second));
    check(correction.has_value() == (i <= 8));
  }
}
void host_clock_invalid_and_gap() {
  Monitor monitor;
  check(!monitor.observe(reading(10 * second, 10 * second)));
  auto slow = reading(11 * second, 11 * second);
  slow.after_ns = slow.before_ns + 1000001;
  check(!monitor.observe(slow));
  check(!monitor.observe(reading(12 * second, 50 * second)));
  check(!monitor.observe(reading(13 * second, 51 * second)));
  check(!monitor.observe({0, 14 * second, 14 * second + 200}));
  check(!monitor.observe(reading(15 * second, 80 * second)));
  check(!monitor.observe(reading(14 * second, 90 * second)));  // Reversed monotonic reading.
  check(!monitor.observe(reading(16 * second, 100 * second)));
  check(!monitor.observe(reading(30 * second, 200 * second)));  // Observation gap, no step claim.
  check(!monitor.observe(reading(31 * second, 201 * second)));
  check(!monitor.observe({epoch, 32 * second + 1, 32 * second}));
  check(!monitor.observe(reading(33 * second, 300 * second)));
}
void host_clock_wrap_and_read_brackets() {
  Monitor wrap;
  const auto start = std::uint64_t{0} - ticks(second / 2);
  check(!wrap.observe({start, 10 * second, 10 * second}));
  check(!wrap.observe({start + ticks(second), 11 * second, 11 * second}));
  Monitor bracket;
  const auto old = reading(10 * second, 10 * second);
  check(!bracket.observe(old));
  // A 20.8 ms relative difference is covered by the actual read brackets and
  // conservative diagnostic drift tolerance; it must not claim a step.
  const auto next =
      Monitor::Reading{epoch + ticks(11 * second + 21300100), 11 * second, 11 * second + 1000000};
  check(!bracket.observe(next));
}
}  // namespace

int main() {
  monotonic_timeline();
  host_clock_corrections();
  host_clock_invalid_and_gap();
  host_clock_wrap_and_read_brackets();
  constexpr std::uint64_t transmit = 0xed00000080000000ULL;
  const auto request = headunit::AirplayTiming::request(transmit);
  if (request[0] != 0x80 || request[1] != 0xd2 || request[24] != 0xed || request[28] != 0x80) {
    throw std::runtime_error("timing request format changed");
  }
  const auto reply = headunit::AirplayTiming::reply(request, transmit + 1, transmit + 2);
  if (!headunit::AirplayTiming::valid_reply(reply, transmit) ||
      headunit::AirplayTiming::valid_reply(reply, transmit + 1) ||
      headunit::AirplayTiming::ntp_now() == 0) {
    throw std::runtime_error("timing reply validation failed");
  }
  return 0;
}
