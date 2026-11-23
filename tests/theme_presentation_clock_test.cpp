// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/presentation_clock.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace headunit::theme::video;
constexpr std::uint64_t second = 1000000000, epoch = std::uint64_t{2208988800} << 32;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
}
std::uint64_t ntp(unsigned s, unsigned ns = 0) {
  return (std::uint64_t(s) << 32) + (std::uint64_t(ns) << 32) / second;
}
ClockSample media_sample(std::uint64_t time, std::uint64_t mono) {
  return {true, time + epoch, mono, mono, mono, 1000};
}
PresentationFeedback feedback(std::uint64_t token, std::uint64_t mono,
                              std::uint64_t delay = 1000000) {
  // Fixture presentation clock has an unrelated +200 second epoch.
  PresentationFeedback f;
  f.token = token;
  f.clock_id = 4;
  f.presented = true;
  const auto raw = mono + 200 * second;
  f.seconds = raw / second;
  f.nanoseconds = raw % second;
  f.received_ns = mono + delay + 100;
  f.correlation = ClockCorrelation{4, raw + delay, mono + delay - 100, mono + delay + 100};
  return f;
}
void mapping() {
  MediaClock media;
  media.update(media_sample(ntp(900), 10 * second));
  PresentationClock clock;
  auto value = clock.accept(feedback(1, 10 * second + second / 4), media);
  CHECK(value && value->token == 1 && value->monotonic_ns == 10 * second + second / 4);
  CHECK(value->media_ntp == ntp(900, 250000000) && value->pts_seconds == 900.25 &&
        value->uncertainty_ns == 3100);
  auto discarded = feedback(2, 10 * second + second / 3);
  discarded.presented = false;
  discarded.correlation.reset();
  CHECK(!clock.accept(discarded, media));
  value = clock.accept(feedback(3, 10 * second + second / 2), media);
  CHECK(value && value->pts_seconds == 900.5);
  rejects([&] { clock.accept(feedback(3, 10 * second + second / 2), media); });
  CHECK(clock.failed());
  rejects([&] { clock.accept(feedback(4, 10 * second + second / 2), media); });
}
void delayed_feedback() {
  MediaClock media;
  media.update(media_sample(ntp(900), 10 * second));
  media.update(media_sample(ntp(901, 1000000), 11 * second));
  // Feedback for a frame before the latest anchor must keep its original map.
  CHECK(media.presentation_at(10 * second + second / 2, 11 * second + 1000000).pts_seconds ==
        900.5);
  CHECK(std::abs(
            media.presentation_at(11 * second + second / 4, 11 * second + second / 2).pts_seconds -
            901.251) < 1e-8);
  PresentationClock clock;
  auto f = feedback(1, 10 * second + second / 2, second / 2 + 1000000);
  CHECK(clock.accept(f, media)->pts_seconds == 900.5);
  rejects([&] { media.presentation_at(9 * second, 11 * second); });
  rejects([&] { media.presentation_at(12 * second, 11 * second); });
  rejects([&] { media.presentation_at(10 * second, 15 * second); });
  rejects([&] { media.update({}); });  // Lock loss invalidates history too.
  rejects([&] { media.presentation_at(10 * second + second / 2, 11 * second + 1000000); });
  MediaClock bounded;
  for (unsigned i = 0; i < 40; ++i)
    bounded.update(media_sample(ntp(900, i * 1000000), 10 * second + i * 1000000));
  rejects([&] { bounded.presentation_at(10 * second + 1000000, 10 * second + 40000000); });
}
void invalid() {
  MediaClock media;
  media.update(media_sample(ntp(900), 10 * second));
  const auto invalid = [&](auto edit) {
    auto f = feedback(1, 10 * second + second / 4);
    edit(f);
    PresentationClock clock;
    rejects([&] { clock.accept(f, media); });
    CHECK(clock.failed());
  };
  invalid([](auto& f) { f.token = 0; });
  invalid([](auto& f) { f.nanoseconds = 1000000000; });
  invalid([](auto& f) { f.seconds = std::numeric_limits<std::uint64_t>::max(); });
  invalid([](auto& f) { f.flags = 16; });
  invalid([](auto& f) { f.correlation.reset(); });
  invalid([](auto& f) { f.correlation->clock_id = 1; });
  invalid([](auto& f) { f.correlation->after_ns = f.correlation->before_ns - 1; });
  invalid([](auto& f) { f.correlation->before_ns -= 3000000; });
  invalid([](auto& f) { f.received_ns = f.correlation->after_ns - 1; });
  invalid([](auto& f) { f.correlation->source_ns -= 2000000; });
  invalid([](auto& f) { f.correlation->source_ns += second; });
  invalid([](auto& f) { f.received_ns += second; });
  PresentationClock jumped;
  CHECK(jumped.accept(feedback(1, 10 * second + second / 4), media));
  auto f = feedback(2, 10 * second + second / 2);
  f.correlation->source_ns += 1000000;
  rejects([&] { jumped.accept(f, media); });
  CHECK(jumped.failed());
  PresentationClock changed;
  CHECK(changed.accept(feedback(1, 10 * second + second / 4), media));
  f = feedback(2, 10 * second + second / 2);
  f.clock_id = 1;
  f.correlation->clock_id = 1;
  rejects([&] { changed.accept(f, media); });
  PresentationClock reverse;
  CHECK(reverse.accept(feedback(1, 10 * second + second / 2), media));
  rejects([&] { reverse.accept(feedback(2, 10 * second + second / 4), media); });
  PresentationClock retroactive;
  CHECK(retroactive.accept(feedback(1, 10 * second + second / 2), media));
  rejects([&] { retroactive.accept(feedback(2, 10 * second + 400000000, 200000000), media); });
  PresentationClock unsynchronized;
  MediaClock empty;
  rejects([&] { unsynchronized.accept(feedback(1, 10 * second + second / 4), empty); });
  PresentationClock rate;
  CHECK(rate.accept(feedback(1, 10 * second + second / 4), media));
  f = feedback(2, 10 * second + second / 2);
  f.correlation->source_ns += 100000;  // 400 ppm, inside declared allowance.
  CHECK(rate.accept(f, media)->pts_seconds < 900.5);
  PresentationLimits limits;
  limits.max_uncertainty_ns = 3000;
  PresentationClock uncertain(limits);
  rejects([&] { uncertain.accept(feedback(1, 10 * second + second / 4), media); });
  limits.max_drift_ppm = std::numeric_limits<std::uint32_t>::max();
  rejects([&] { PresentationClock bad(limits); });
}
void precision_and_wrap() {
  MediaClock media;
  const auto high = std::numeric_limits<std::uint64_t>::max() - 300 * second;
  media.update(media_sample(ntp(0xffffffffU), high));
  PresentationClock clock;
  const auto one = clock.accept(feedback(1, high + 10), media);
  CHECK(one && one->monotonic_ns == high + 10 && one->media_ntp == ntp(0xffffffffU) + 42);
  MediaClock wrapped;
  wrapped.update(media_sample(ntp(0xffffffffU, 750000000), 10 * second));
  PresentationClock wrap;
  CHECK(wrap.accept(feedback(1, 10 * second + 100000000), wrapped));
  rejects([&] { wrap.accept(feedback(2, 10 * second + 500000000), wrapped); });
}
void idle_preview() {
  MediaClock media;
  media.update(media_sample(ntp(900), 10 * second));
  PresentationClock clock;
  const auto first = clock.accept(feedback(1, 10 * second + second / 4), media);
  CHECK(first);
  clock.resume_after_idle();
  // Pausing a display does not pause the independent paired-clock updates.
  for (unsigned i = 1; i <= 10; ++i)
    media.update(media_sample(ntp(900 + i), (10 + i) * second));
  const auto next = clock.accept(feedback(2, 20 * second + second / 4), media);
  CHECK(next && next->pts_seconds == 910.25);
  clock.resume_after_idle();
  rejects([&] {
    clock.accept(feedback(1, 20 * second + second / 2), media);
  });                                           // Token guards survive.
  rejects([&] { clock.resume_after_idle(); });  // A fault cannot be cleared this way.
  PresentationClock stale;
  stale.resume_after_idle();
  rejects([&] { stale.accept(feedback(1, 15 * second, 5 * second), media); });
  PresentationClock domain;
  CHECK(domain.accept(feedback(1, 20 * second + second / 4), media));
  domain.resume_after_idle();
  auto changed = feedback(2, 20 * second + second / 2);
  changed.clock_id = 1;
  changed.correlation->clock_id = 1;
  rejects([&] { domain.accept(changed, media); });
}
void early_receipt() {
  MediaClock media;
  media.update(media_sample(ntp(900), 10 * second));
  auto f = feedback(1, 10 * second + second / 4);
  auto early = *f.correlation;
  early.source_ns -= 1134259;
  CHECK(!presentation_time_reached(f, early));
  // Strict mapping still rejects an early correlation; the caller must wait.
  auto premature = f;
  premature.correlation = early;
  PresentationClock strict;
  rejects([&] { strict.accept(premature, media); });
  CHECK(presentation_time_reached(f, *f.correlation));
  PresentationClock clock;
  CHECK(clock.accept(f, media)->pts_seconds == 900.25);
  early.source_ns -= 2000000;
  rejects([&] { presentation_time_reached(f, early); });
  early = *f.correlation;
  early.clock_id = 1;
  rejects([&] { presentation_time_reached(f, early); });
}
int main() {
  try {
    mapping();
    invalid();
    precision_and_wrap();
    delayed_feedback();
    idle_preview();
    early_receipt();
    std::cout << "Presentation clock: domain conversion, delayed feedback, uncertainty, "
                 "stale/jumping clocks and discard passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
