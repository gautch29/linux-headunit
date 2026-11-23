// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/airplay_clock.hpp>
#include <headunit/airplay_timing.hpp>

#include <algorithm>
#include <stdexcept>

namespace headunit {
namespace {
constexpr std::uint64_t second = 1000000000;
constexpr std::uint64_t quantization_ns = 2000;
constexpr std::uint64_t phase_tolerance_ns = 20000000;
// Conservative host policy for free-running clock drift, not a phone SLA.
constexpr std::uint64_t drift_ppm = 500;

std::uint64_t read64(std::span<const std::uint8_t> bytes, unsigned at) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value = (value << 8) | bytes[at + i];
  return value;
}
std::uint64_t ticks(std::uint64_t ns) {
  return ((ns / second) << 32) + ((ns % second) << 32) / second;
}
std::uint64_t nanoseconds(std::uint64_t ticks) {
  return (ticks >> 32) * second + (((ticks & 0xffffffffULL) * second + (1ULL << 31)) >> 32);
}
std::uint64_t distance(std::uint64_t a, std::uint64_t b) {
  const auto delta = a - b;
  return nanoseconds(std::min(delta, std::uint64_t{0} - delta));
}
std::uint64_t drift(std::uint64_t ns) {
  return (ns * drift_ppm + 999999) / 1000000;
}
}  // namespace

void AirplayClock::sent(std::uint64_t origin, std::uint64_t before) {
  if (!origin ||
      (last_request_ && (before <= last_request_->before_ns || origin == last_request_->origin)))
    throw std::invalid_argument("invalid timing request correlation");
  // Continue normal timing traffic even after an Ultra clock fault. It cannot
  // recover this timeline; the connection owner must create a new session.
  ++diagnostics_.sent;
  last_request_ = Request{origin, before};
  pending_ = failed_ ? std::nullopt : last_request_;
}

bool AirplayClock::receive(std::span<const std::uint8_t> packet, std::uint64_t now) {
  if (failed_ || !pending_ || !AirplayTiming::valid_reply(packet, pending_->origin)) {
    ++diagnostics_.unmatched;
    return false;
  }
  const auto request = *pending_;
  pending_.reset();  // A duplicate cannot refresh the clock, even if this is bad.
  if (now < request.before_ns || now - request.before_ns > max_exchange_ns) {
    ++diagnostics_.invalid_exchange;
    return false;
  }
  const auto elapsed = now - request.before_ns;
  const auto received = read64(packet, 16), transmitted = read64(packet, 24);
  const auto processing_ticks = transmitted - received;  // NTP-era wrap is valid.
  if (processing_ticks > ticks(max_exchange_ns)) {
    ++diagnostics_.invalid_exchange;
    return false;
  }
  const auto processing = nanoseconds(processing_ticks);
  if (processing > elapsed + quantization_ns) {
    ++diagnostics_.invalid_exchange;
    return false;
  }
  const auto delay = elapsed > processing ? elapsed - processing : 0;
  diagnostics_.last_delay_ns = delay;
  diagnostics_.max_delay_ns = std::max(diagnostics_.max_delay_ns, delay);
  if (delay > max_network_delay_ns) {
    ++diagnostics_.delayed;
    return false;
  }
  const AirplayClockSample next{transmitted + ticks(delay / 2), now, now,
                                (delay + 1) / 2 + quantization_ns};

  if (last_observed_) {
    const auto& previous = *last_observed_;
    if (now <= previous.monotonic_ns) return false;
    const auto age = now - previous.monotonic_ns;
    const auto phase_delta = distance(next.timing_ntp, previous.timing_ntp + ticks(age));
    const auto phase_bound =
        phase_tolerance_ns + previous.uncertainty_ns + next.uncertainty_ns + drift(age);
    if (age > max_relock_gap_ns || phase_delta > phase_bound) {
      diagnostics_.fault = {
          age > max_relock_gap_ns ? FaultReason::relock_gap : FaultReason::phase_jump,
          phase_delta,
          phase_bound,
          age,
          previous.uncertainty_ns,
          next.uncertainty_ns,
          elapsed,
          processing};
      failed_ = true;
      qualified_ = false;
      samples_.clear();
      return false;
    }
    if (age > max_sample_age_ns) {
      qualified_ = false;
      samples_.clear();
    }
  }
  diagnostics_.last_accepted_ns = now;
  last_observed_ = next;
  samples_.push_back(next);
  while (samples_.size() > 8 || now - samples_.front().monotonic_ns > max_sample_age_ns)
    samples_.pop_front();
  if (samples_.size() >= 3) qualified_ = true;
  ++accepted_;
  return true;
}

std::uint64_t AirplayClock::probe_interval_ns(std::uint64_t now) const {
  if (failed_) return second;
  // Acquire the three distinct initial measurements without quarter-second
  // gaps. Keep this burst bounded; delayed/lost replies return to normal
  // acquisition cadence after eight sends. Reacquisition is unchanged.
  if (accepted_ < 3 && diagnostics_.sent < 8) return second / 20;
  if (!qualified_ || !last_observed_ || now < last_observed_->monotonic_ns ||
      now - last_observed_->monotonic_ns >= 2 * second)
    return second / 4;
  return second;
}

std::optional<AirplayClockSample> AirplayClock::sample(std::uint64_t now) const {
  if (failed_ || !qualified_ || !last_observed_ || now < last_observed_->monotonic_ns) return {};
  const auto& candidate = *last_observed_;
  const auto age = now - candidate.monotonic_ns;
  if (age > max_sample_age_ns) return {};
  // Publish each newly accepted measurement in order. Selecting an older
  // low-delay measurement can hide bounded clock corrections, then expose
  // their accumulated phase as one discontinuity when that sample expires.
  // Rejected exchanges still cannot renew synchronization or qualification.
  return AirplayClockSample{candidate.timing_ntp + ticks(age), now, candidate.last_sync_ns,
                            candidate.uncertainty_ns + drift(age)};
}
}  // namespace headunit
