// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/airplay_clock.hpp>
#include <headunit/airplay_timing.hpp>
#include <headunit/theme/media_clock.hpp>
#include <headunit/theme/receiver_clock.hpp>

#include <iostream>
#include <algorithm>
#include <limits>
#include <source_location>
#include <stdexcept>

namespace {
constexpr std::uint64_t second = 1000000000;
constexpr std::uint64_t epoch = std::uint64_t{2208988800} << 32;
std::uint64_t ticks(std::uint64_t ns) {
  return ((ns / second) << 32) + ((ns % second) << 32) / second;
}
void check(bool value, std::source_location at = std::source_location::current()) {
  if (!value)
    throw std::runtime_error("phone clock check failed at line " + std::to_string(at.line()));
}
using Clock = headunit::AirplayClock;
using Wire = headunit::AirplayTiming;

// Independent fixture timeline: remote media starts at 900 seconds while the
// host monotonic clock starts at 10 seconds. Wire NTP adds the transport epoch.
std::uint64_t phone(std::uint64_t host) {
  return epoch + ticks(host + 890 * second);
}
Wire::Packet exchange(Clock& clock, std::uint64_t sent, std::uint64_t forward = 1000000,
                      std::uint64_t processing = 200000, std::uint64_t back = 1000000,
                      std::uint64_t phase = 0) {
  const auto origin = ticks(sent) + 123;  // Correlation token, deliberately not host wall time.
  clock.sent(origin, sent);
  const auto reply = Wire::reply(Wire::request(origin), phone(sent + forward) + phase,
                                 phone(sent + forward + processing) + phase);
  check(clock.receive(reply, sent + forward + processing + back));
  return reply;
}
void qualify(Clock& clock, std::uint64_t start = 10 * second) {
  for (unsigned i = 0; i < 3; ++i) {
    exchange(clock, start + i * second);
    check(clock.sample(start + i * second + 3000000).has_value() == (i == 2));
  }
}
void mapping() {
  Clock clock;
  check(!clock.sample(0));
  qualify(clock);
  const auto now = 12 * second + 3000000;
  const auto sample = clock.sample(now);
  check(sample && sample->monotonic_ns == now && sample->last_sync_ns == 12 * second + 2200000);
  const auto error = sample->timing_ntp > phone(now) ? sample->timing_ntp - phone(now)
                                                     : phone(now) - sample->timing_ntp;
  check(error <= 4);  // Only fixed-point conversion rounding.
  check(sample->uncertainty_ns >= 1000000 && sample->uncertainty_ns < 1100000);
  headunit::theme::video::MediaClock media;
  media.update({true, sample->timing_ntp, now, now, sample->last_sync_ns, sample->uncertainty_ns});
  const auto frame = media.schedule(ticks(902 * second + 10000000), now);
  check(frame.deadline_ns >= 12 * second + 9999998 && frame.deadline_ns <= 12 * second + 10000002);
  check(frame.media_pts_seconds > 902.009 && frame.media_pts_seconds < 902.011);
  const auto later = clock.sample(now + second);
  check(later && later->last_sync_ns == sample->last_sync_ns &&
        later->uncertainty_ns > sample->uncertainty_ns);
  check(clock.sample(22 * second + 2200000).has_value());
  check(!clock.sample(22 * second + 2200001));
  check(!clock.sample(12 * second));
}
void correlation_and_bounds() {
  Clock clock;
  check(!clock.receive({}, 1));
  const auto reply = exchange(clock, 10 * second);
  check(!clock.receive(reply, 10 * second + 3000000) && clock.accepted() == 1);
  clock.sent(99, 11 * second);
  check(!clock.receive(std::span(reply).first(31), 11 * second + 1000000));
  check(!clock.receive(reply, 11 * second + 3000000));
  auto wrong = Wire::reply(Wire::request(100), phone(11 * second), phone(11 * second));
  check(!clock.receive(wrong, 11 * second + 3000000));
  auto delayed = Wire::reply(Wire::request(99), phone(11 * second), phone(11 * second));
  check(!clock.receive(delayed, 11 * second + 21000000));
  check(!clock.receive(delayed, 11 * second + 1000000));  // Rejected matching reply consumed.
  clock.sent(101, 12 * second);
  auto backwards = Wire::reply(Wire::request(101), phone(12 * second + 1), phone(12 * second));
  check(!clock.receive(backwards, 12 * second + 2000000));
  clock.sent(102, 13 * second);
  auto impossible =
      Wire::reply(Wire::request(102), phone(13 * second), phone(13 * second + 10000000));
  check(!clock.receive(impossible, 13 * second + 1000000));
  clock.sent(103, 14 * second);
  auto stale = Wire::reply(Wire::request(103), phone(14 * second), phone(14 * second));
  check(!clock.receive(stale, 15 * second + 1));
  check(clock.accepted() == 1 && !clock.sample(15 * second));
  const auto diagnostics = clock.diagnostics();
  check(diagnostics.sent == 5 && diagnostics.unmatched == 6 && diagnostics.invalid_exchange == 3);
  check(diagnostics.delayed == 1 && diagnostics.last_delay_ns == 21000000 &&
        diagnostics.max_delay_ns == 21000000);
  check(diagnostics.last_accepted_ns == 10 * second + 2200000);
  check(diagnostics.fault.reason == Clock::FaultReason::none);
  bool rejected = false;
  try {
    clock.sent(103, 16 * second);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
}
void query_stability() {
  Clock clock;
  qualify(clock);
  std::uint64_t previous_sync = 0;
  // Sub-microsecond asymmetry and closely spaced query times exercise fixed
  // point rounding at the delay filter's boundaries.
  for (unsigned i = 0; i < 20; ++i) {
    const auto at = (13 + i) * second;
    exchange(clock, at, 1000001 + i * 11, 200003, 1000000 + i * 7);
    for (unsigned offset = 0; offset < 100; ++offset) {
      const auto sample = clock.sample(at + 3000000 + offset);
      check(sample && sample->last_sync_ns >= previous_sync);
      previous_sync = sample->last_sync_ns;
    }
  }
}
void bounded_delay_and_relock() {
  Clock clock;
  qualify(clock);
  const auto previous = clock.sample(12 * second + 3000000);
  exchange(clock, 13 * second, 8000000, 200000, 1000000);
  const auto filtered = clock.sample(13 * second + 10000000);
  check(filtered && filtered->last_sync_ns > previous->last_sync_ns &&
        filtered->uncertainty_ns > previous->uncertainty_ns);
  check(!clock.sample(27 * second));
  exchange(clock, 28 * second);
  check(!clock.sample(28 * second + 3000000));
  exchange(clock, 29 * second);
  check(!clock.sample(29 * second + 3000000));
  exchange(clock, 30 * second);
  check(clock.sample(30 * second + 3000000).has_value());

  clock.sent(500, 31 * second);
  const auto shifted = Wire::reply(Wire::request(500), phone(31 * second) + ticks(second),
                                   phone(31 * second) + ticks(second));
  check(!clock.receive(shifted, 31 * second + 2000000) && clock.failed());
  check(!clock.sample(31 * second + 3000000));
  clock.sent(501, 32 * second);
  const auto fresh = Wire::reply(Wire::request(501), phone(32 * second), phone(32 * second));
  check(!clock.receive(fresh, 32 * second + 2000000));
}
void eras_and_long_processing() {
  Clock clock;
  const auto base = std::numeric_limits<std::uint64_t>::max() - ticks(second / 2);
  for (unsigned i = 0; i < 3; ++i) {
    const auto at = 10 * second + i * second;
    clock.sent(i + 1, at);
    const auto remote = base + ticks(i * second + 1000000);
    const auto reply = Wire::reply(Wire::request(i + 1), remote, remote + ticks(500000000));
    check(clock.receive(reply, at + 502000000));
  }
  const auto sample = clock.sample(13 * second);
  check(sample.has_value());
  const auto delta = sample->timing_ntp - (base + ticks(3 * second));
  check(std::min(delta, std::uint64_t{0} - delta) <= 4);
  Clock stale;
  qualify(stale);
  stale.sent(999, 50 * second);
  const auto reply = Wire::reply(Wire::request(999), phone(50 * second), phone(50 * second));
  check(!stale.receive(reply, 50 * second + 2000000) && stale.failed());
  const auto fault = stale.diagnostics().fault;
  check(fault.reason == Clock::FaultReason::relock_gap);
  check(fault.accepted_age_ns == 38 * second - 200000);
  check(fault.phase_delta_ns < fault.phase_bound_ns);
  check(fault.exchange_ns == 2000000 && fault.processing_ns == 0);
}
void phase_fault_diagnostics() {
  // Both signs of a real source step report its magnitude, exact policy bound
  // and measurement age. The rejected exchange must not refresh the anchor.
  for (bool backwards : {false, true}) {
    Clock clock;
    qualify(clock);
    const auto last_accepted = clock.diagnostics().last_accepted_ns;
    clock.sent(999, 13 * second);
    const auto phase = backwards ? std::uint64_t{0} - ticks(30000000) : ticks(30000000);
    const auto reply = Wire::reply(Wire::request(999), phone(13 * second + 1000000) + phase,
                                   phone(13 * second + 1200000) + phase);
    check(!clock.receive(reply, 13 * second + 2200000) && clock.failed());
    const auto fault = clock.diagnostics().fault;
    check(fault.reason == Clock::FaultReason::phase_jump);
    check(fault.phase_delta_ns >= 29999999 && fault.phase_delta_ns <= 30000001);
    check(fault.phase_bound_ns == 22504000 && fault.accepted_age_ns == second);
    check(fault.previous_uncertainty_ns == 1002000 && fault.next_uncertainty_ns == 1002000);
    check(fault.exchange_ns == 2200000 && fault.processing_ns == 200000);
    check(clock.accepted() == 3 && clock.diagnostics().last_accepted_ns == last_accepted);
    check(!clock.sample(13 * second + 3000000));
    // Later traffic cannot replace the terminal fault or recover this timeline.
    clock.sent(1000, 14 * second);
    const auto fresh = Wire::reply(Wire::request(1000), phone(14 * second), phone(14 * second));
    check(!clock.receive(fresh, 14 * second + 2000000));
    check(clock.diagnostics().fault.phase_delta_ns == fault.phase_delta_ns &&
          clock.diagnostics().fault.accepted_age_ns == fault.accepted_age_ns);
  }
}
void proactive_probes() {
  Clock source;
  check(source.probe_interval_ns(10 * second) == second / 20);
  qualify(source);
  const auto accepted = 12 * second + 2200000;
  check(source.probe_interval_ns(accepted) == second);
  check(source.probe_interval_ns(accepted + 2 * second - 1) == second);
  check(source.probe_interval_ns(accepted + 2 * second) == second / 4);
  // Slow replies do not refresh the anchor or return to the slow cadence.
  source.sent(999, 15 * second);
  const auto slow = Wire::reply(Wire::request(999), phone(15 * second), phone(15 * second));
  check(!source.receive(slow, 15 * second + 21000000));
  check(source.probe_interval_ns(16 * second) == second / 4);
  check(source.sample(16 * second)->last_sync_ns == accepted);
  exchange(source, 17 * second);
  check(source.probe_interval_ns(17 * second + 3000000) == second);
  check(!source.sample(28 * second));  // Original expiration is unchanged.
  check(source.probe_interval_ns(28 * second) == second / 4);
}
void initial_probe_burst() {
  Clock source;
  headunit::theme::video::ReceiverClock bridge;
  for (unsigned i = 0; i < 3; ++i) {
    const auto at = 10 * second + i * 50000000ULL;
    exchange(source, at);
    bridge.update(source.sample(at + 3000000));
    check(bridge.clock().ready(at + 3000000) == (i == 2));
  }
  check(source.probe_interval_ns(10 * second + 103000000) == second);
  Clock missing;
  for (unsigned i = 0; i < 8; ++i) {
    const auto at = 10 * second + i * 50000000ULL;
    check(missing.probe_interval_ns(at) == second / 20);
    missing.sent(i + 1, at);
    check(!missing.sample(at));
  }
  check(missing.probe_interval_ns(10 * second + 400000000) == second / 4);
}
void idle_receiver_bridge() {
  Clock source;
  qualify(source);
  headunit::theme::video::ReceiverClock bridge;
  // No decoded frames are involved. Keep refreshing from timing traffic for
  // eight seconds, then map an actual delayed presentation timestamp.
  for (unsigned i = 0; i < 800; ++i) {
    const auto now = 12 * second + 3000000 + i * 10000000ULL;
    bridge.update(source.sample(now));
    check(bridge.clock().ready(now));
  }
  const auto feedback = bridge.clock().presentation_at(19 * second, 20 * second);
  check(feedback.pts_seconds > 908.99 && feedback.pts_seconds < 909.01);
  bridge.update(source.sample(23 * second));  // Real synchronization expired.
  check(!bridge.clock().ready(23 * second));
  exchange(source, 24 * second);
  bridge.update(source.sample(24 * second + 3000000));
  check(!bridge.clock().ready(24 * second + 3000000));
  exchange(source, 25 * second);
  exchange(source, 26 * second);
  bridge.update(source.sample(26 * second + 3000000));
  check(bridge.clock().ready(26 * second + 3000000));
  bool rejected = false;
  try {
    bridge.update(source.sample(26 * second + 2999999));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected && bridge.failed() && !bridge.clock().ready(26 * second + 3000000));
}
void gradual_phase_bridge() {
  Clock source;
  qualify(source);
  headunit::theme::video::ReceiverClock bridge;
  bridge.update(source.sample(12 * second + 3000000));
  // Real successive measurements are consistent with the unchanged source
  // phase bound. Keeping an older low-delay anchor conceals their progression
  // and produces a large publication step when that anchor leaves the window.
  for (unsigned i = 1; i <= 12; ++i) {
    const auto at = (12 + i) * second;
    exchange(source, at, 8000000, 200000, 8000000, ticks(i * 12000000));
    for (unsigned frame = 0; frame < 50; ++frame) {
      const auto now = at + 17000000 + frame * 16000000;
      bridge.update(source.sample(now));
      check(!source.failed() && !bridge.failed() && bridge.clock().ready(now));
    }
  }
}
void uncertain_phase_bridge() {
  Clock source;
  qualify(source);
  headunit::theme::video::ReceiverClock bridge;
  bridge.update(source.sample(12 * second + 3000000));
  // The source accepts a bounded offset change with measured network delay.
  // Its downstream media adapter must use the same uncertainty intervals.
  exchange(source, 13 * second, 8000000, 200000, 8000000, ticks(25000000));
  bridge.update(source.sample(23 * second));  // Old low-delay measurements expired.
  check(!source.failed() && !bridge.failed() && bridge.clock().ready(23 * second));
  source.sent(999, 24 * second);
  const auto jumped = Wire::reply(Wire::request(999), phone(24 * second) + ticks(second),
                                  phone(24 * second) + ticks(second));
  check(!source.receive(jumped, 24 * second + 2000000) && source.failed());
  bridge.update(source.sample(24 * second + 3000000));
  check(!bridge.clock().ready(24 * second + 3000000));
}
}  // namespace
int main() {
  mapping();
  correlation_and_bounds();
  query_stability();
  bounded_delay_and_relock();
  eras_and_long_processing();
  phase_fault_diagnostics();
  idle_receiver_bridge();
  proactive_probes();
  uncertain_phase_bridge();
  gradual_phase_bridge();
  initial_probe_burst();
  std::cout
      << "phone clock: correlation, bounds, drift, relock, era wrap and media mapping passed\n";
}
