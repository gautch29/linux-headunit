// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/media_clock.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace headunit::theme::video;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::exception&) {
    rejected = true;
  }
  CHECK(rejected);
}
constexpr std::uint64_t second = 1000000000, epoch = std::uint64_t{2208988800} << 32;
std::uint64_t ntp(unsigned seconds, unsigned nanoseconds = 0) {
  return (std::uint64_t(seconds) << 32) + (std::uint64_t(nanoseconds) << 32) / second;
}
ClockSample sample(std::uint64_t media, std::uint64_t mono) {
  return {true, media + epoch, mono, mono, mono, 0};
}
void mapping() {
  MediaClock clock;
  CHECK(!clock.ready(0));
  rejects([&] { clock.schedule(0, 0); });
  clock.update({});
  CHECK(!clock.failed() && !clock.ready(0));
  clock.update(sample(ntp(900), 10 * second));
  CHECK(clock.ready(10 * second) && !clock.ready(10 * second - 1));
  CHECK(clock.media_ntp_at(10 * second + second / 4) == ntp(900, 250000000));
  const auto future = clock.schedule(ntp(900, 750000000), 10 * second + second / 4);
  CHECK(future.sender_ntp == ntp(900, 750000000) && future.media_pts_seconds == 900.75);
  CHECK(future.deadline_ns == 10 * second + 750000000 && future.lead_ns == 500000000 &&
        future.uncertainty_ns == 0);
  const auto late = clock.schedule(ntp(900, 125000000), 10 * second + second / 4);
  CHECK(late.lead_ns == -125000000 && late.deadline_ns == 10 * second + 125000000);
  rejects([&] { clock.schedule(ntp(902), 10 * second); });
  rejects([&] { clock.schedule(ntp(898), 10 * second); });
  try {
    clock.schedule(ntp(898), 10 * second);
    CHECK(false);
  } catch (const FrameOutsideWindow& rejected) {
    CHECK(rejected.lead_ns == -2000000000LL);
  }
  CHECK(!clock.failed());  // An outlying frame does not rewrite the clock.
  auto next = sample(ntp(901, 1000000), 11 * second);
  next.last_sync_ns = 10 * second;
  clock.update(next);
  CHECK(clock.media_ntp_at(11 * second) == ntp(901, 1000000));
  CHECK(clock.ready(14 * second) && !clock.ready(14 * second + 1));
  rejects([&] { clock.schedule(ntp(904), 14 * second + 1); });
  rejects([&] { clock.update(sample(ntp(905), 15 * second)); });
  CHECK(clock.failed());
}
void bounds_and_faults() {
  const auto expect_fault = [](ClockSample input) {
    MediaClock clock;
    rejects([&] { clock.update(input); });
    CHECK(clock.failed());
    rejects([&] { clock.update(sample(ntp(1), second)); });
    CHECK(!clock.ready(second));
  };
  auto s = sample(ntp(100), 20 * second);
  s.after_ns = s.before_ns - 1;
  expect_fault(s);
  s = sample(ntp(100), 20 * second);
  s.after_ns += 2000001;
  expect_fault(s);
  s = sample(ntp(100), 20 * second);
  s.last_sync_ns = 9 * second;
  expect_fault(s);
  s = sample(ntp(100), 20 * second);
  s.last_sync_ns += 1;
  expect_fault(s);
  s = sample(ntp(100), 20 * second);
  s.synchronization_uncertainty_ns = 20000001;
  expect_fault(s);
  s = sample(ntp(100), 20 * second);
  s.after_ns += 2;
  s.synchronization_uncertainty_ns = 20000000;
  expect_fault(s);
  MediaClock jump;
  jump.update(sample(ntp(1), second));
  rejects([&] { jump.update(sample(ntp(2, 21000000), 2 * second)); });
  CHECK(jump.failed());
  MediaClock unlock;
  unlock.update(sample(ntp(1), second));
  rejects([&] { unlock.update({}); });
  CHECK(unlock.failed());
  MediaClock reverse;
  reverse.update(sample(ntp(1), second));
  rejects([&] { reverse.update(sample(ntp(1), second)); });
  MediaClock sync_reverse;
  sync_reverse.update(sample(ntp(1), second));
  s = sample(ntp(2), 2 * second);
  s.last_sync_ns = second - 1;
  rejects([&] { sync_reverse.update(s); });
  MediaClock stale;
  s = sample(ntp(1), 20 * second);
  s.last_sync_ns = 11 * second;
  stale.update(s);
  CHECK(stale.ready(21 * second) && !stale.ready(21 * second + 1));
  MediaClock uncertain;
  s = sample(ntp(1), 20 * second);
  s.after_ns += 1001;
  s.synchronization_uncertainty_ns = 10000;
  uncertain.update(s);
  CHECK(!uncertain.ready(s.after_ns - 1));
  const auto mapped = uncertain.schedule(ntp(1), s.after_ns);
  CHECK(mapped.uncertainty_ns == 10501 && mapped.deadline_ns == s.before_ns + 500);
  auto limits = ClockLimits{};
  limits.max_anchor_age_ns = 0;
  rejects([&] { MediaClock invalid(limits); });
  limits = ClockLimits{};
  limits.max_frame_distance_ns = std::numeric_limits<std::uint64_t>::max();
  rejects([&] { MediaClock invalid(limits); });
}
void wrap_and_precision() {
  CHECK(timing_to_media_ntp(0) == std::uint64_t{0} - epoch);
  CHECK(timing_to_media_ntp(epoch + 0x123456789ULL) == 0x123456789ULL);
  MediaClock clock;
  clock.update(sample(ntp(0xffffffffU, 750000000), 10 * second));
  const auto frame = clock.schedule(ntp(0, 125000000), 10 * second + second / 4);
  CHECK(frame.deadline_ns == 10 * second + 375000000 && frame.lead_ns == 125000000 &&
        frame.media_pts_seconds == .125);
  clock.update(sample(ntp(0, 750000000), 11 * second));
  CHECK(!clock.failed());
  MediaClock half;
  half.update(sample(0, second));
  rejects([&] { half.schedule(std::uint64_t{1} << 63, second); });
  MediaClock early;
  early.update(sample(ntp(50), 0));
  rejects([&] { early.schedule(ntp(49, 999999999), 0); });
  MediaClock overflow;
  const auto max = std::numeric_limits<std::uint64_t>::max();
  overflow.update(sample(ntp(1), max - 1));
  rejects([&] { overflow.schedule(ntp(1, 1000), max - 1); });
  // One NTP fraction is less than 1 ns. Absolute double rounding must not be
  // used for deadline subtraction even near the final second of an NTP era.
  MediaClock precise;
  precise.update(sample(ntp(0xffffffffU), second));
  const auto tiny = precise.schedule(ntp(0xffffffffU) + 43, second);
  CHECK(tiny.lead_ns == 10 && tiny.deadline_ns == second + 10);
}
void captured_header() {
  // Numeric timing observation from the connected phone; no payload, key or
  // device identifier. Header observation alone is not frame authentication.
  MediaClock clock;
  constexpr std::uint64_t mono = 260152442538452;
  clock.update({true, 0x83acf5950a4c290dULL, mono - 15398, mono + 15398, mono - 1000000, 1000000});
  const auto mapped = clock.schedule(0x0002771516eeee3bULL, mono + 15398);
  CHECK(mapped.sender_ntp == 0x0002771516eeee3bULL);
  CHECK(mapped.deadline_ns == mono + 49358677);
  CHECK(mapped.uncertainty_ns == 1015398);
  CHECK(std::abs(mapped.media_pts_seconds - 161557.08958329144) < 1e-9);
}
void recovery() {
  MediaClock clock;
  clock.suspend();
  CHECK(clock.suspension_epoch() == 0);
  clock.update(sample(ntp(900), 10 * second));
  clock.suspend();
  clock.suspend();
  CHECK(clock.suspension_epoch() == 1 && !clock.ready(10 * second) && !clock.failed());
  rejects([&] { clock.schedule(ntp(900), 10 * second); });
  auto old_sync = sample(ntp(904), 14 * second);
  old_sync.last_sync_ns = 10 * second;
  clock.update(old_sync);
  CHECK(!clock.ready(14 * second));  // A fresh query is not a fresh sync.
  clock.update(sample(ntp(914), 24 * second));
  CHECK(clock.ready(24 * second));
  CHECK(clock.media_ntp_at(24 * second + second / 2) == ntp(914, 500000000));
  rejects([&] { clock.presentation_at(10 * second, 24 * second); });
  CHECK(clock.presentation_at(24 * second, 24 * second).pts_seconds == 914);
  for (int mode = 0; mode < 5; ++mode) {
    MediaClock invalid;
    invalid.update(sample(ntp(900), 10 * second));
    invalid.suspend();
    auto next = sample(ntp(914), 24 * second);
    if (mode == 0) next.timing_wire_ntp += ntp(0, 21000000);  // Same phase bound after an outage.
    if (mode == 1) next = sample(ntp(931), 41 * second);      // Extrapolation is bounded.
    if (mode == 2) next.synchronization_uncertainty_ns = 20000001;
    if (mode == 3) next.last_sync_ns = 10 * second;  // Stale measurement.
    if (mode == 4) {
      invalid.update(old_sync);
      next = old_sync;
    }  // Even ignored queries cannot regress.
    rejects([&] { invalid.update(next); });
    CHECK(invalid.failed());
    rejects([&] { invalid.suspend(); });
  }
}
void measurement_uncertainty() {
  MediaClock clock;
  auto first = sample(ntp(900), 10 * second);
  first.synchronization_uncertainty_ns = 5000000;
  clock.update(first);
  auto next = sample(ntp(901, 29000000), 11 * second);
  next.synchronization_uncertainty_ns = 5000000;
  clock.update(next);  // 19 ms beyond the two measurement intervals.
  CHECK(clock.ready(11 * second) && !clock.failed());
  const auto mapped = clock.schedule(ntp(901, 39000000), 11 * second);
  CHECK(mapped.lead_ns == 10000000 && mapped.uncertainty_ns == 5000000);
  auto jump = sample(ntp(902, 60000000), 12 * second);
  jump.synchronization_uncertainty_ns = 5000000;
  rejects([&] { clock.update(jump); });  // 21 ms beyond the intervals is terminal.
  CHECK(clock.failed() && !clock.ready(12 * second));
}
int main() {
  try {
    mapping();
    bounds_and_faults();
    wrap_and_precision();
    captured_header();
    recovery();
    measurement_uncertainty();
    std::cout << "media clock mapping, freshness, uncertainty, discontinuity, wrap and captured "
                 "timing checks passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
