// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/media_clock.hpp>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>

namespace headunit::theme::video {
namespace {
void check(bool ok, const char* reason) {
  if (!ok) throw std::invalid_argument(reason);
}
constexpr std::uint64_t second = 1000000000;
std::uint64_t ntp_delta(std::uint64_t ns) {
  return ((ns / second) << 32) + ((ns % second) << 32) / second;
}
std::uint64_t magnitude(std::int64_t n) {
  return n < 0 ? std::uint64_t(-(n + 1)) + 1 : std::uint64_t(n);
}
// Signed nearest distance on the 64-bit NTP circle, rounded to nanoseconds.
// Split multiplication prevents overflow and avoids floating-point cancellation.
std::int64_t distance(std::uint64_t a, std::uint64_t b) {
  const auto delta = a - b;
  check(delta != (std::uint64_t{1} << 63), "ambiguous media clock distance");
  const bool negative = delta > (std::uint64_t{1} << 63);
  const auto ticks = negative ? std::uint64_t{0} - delta : delta;
  const auto ns = (ticks >> 32) * second +
                  (((ticks & 0xffffffffULL) * second + (std::uint64_t{1} << 31)) >> 32);
  return negative ? -std::int64_t(ns) : std::int64_t(ns);
}
std::uint64_t offset(std::uint64_t base, std::int64_t delta) {
  if (delta < 0) {
    check(magnitude(delta) <= base, "media deadline precedes host clock origin");
    return base - magnitude(delta);
  }
  check(std::uint64_t(delta) <= std::numeric_limits<std::uint64_t>::max() - base,
        "media deadline overflow");
  return base + std::uint64_t(delta);
}
}  // namespace
MediaClock::MediaClock(ClockLimits limits) : limits_(limits) {
  for (const auto n :
       {limits.max_sample_span_ns, limits.max_uncertainty_ns, limits.max_anchor_age_ns,
        limits.max_sync_age_ns, limits.max_phase_adjustment_ns, limits.max_frame_distance_ns,
        limits.max_relock_gap_ns})
    check(n > 0 && n <= 60 * second, "invalid media clock policy bound");
  check(limits.max_sample_span_ns <= limits.max_anchor_age_ns,
        "sample window exceeds anchor lifetime");
  check(limits.max_relock_gap_ns >= limits.max_anchor_age_ns,
        "relock window is shorter than anchor lifetime");
}
void MediaClock::suspend() {
  check(!failed_, "media clock is faulted");
  if (!anchor_ || suspended_) return;
  check(suspension_epoch_ < std::numeric_limits<std::uint64_t>::max(),
        "media clock suspension sequence exhausted");
  suspended_ = true;
  ++suspension_epoch_;
  history_.clear();
}
void MediaClock::update(ClockSample sample) {
  check(!failed_, "media clock is faulted");
  try {
    if (!sample.locked) {
      check(!anchor_, "media clock lost synchronization");
      return;  // No clock has been acquired yet. Zero is never a fallback PTS.
    }
    check(sample.after_ns >= sample.before_ns, "reversed media clock sample window");
    const auto span = sample.after_ns - sample.before_ns;
    check(span <= limits_.max_sample_span_ns, "media clock sample window too wide");
    check(sample.last_sync_ns <= sample.after_ns &&
              sample.after_ns - sample.last_sync_ns <= limits_.max_sync_age_ns,
          "media clock synchronization is stale");
    const auto half = (span + 1) / 2;
    check(half <= limits_.max_uncertainty_ns &&
              sample.synchronization_uncertainty_ns <= limits_.max_uncertainty_ns - half,
          "media clock sample too uncertain");
    Anchor next{sample, sample.before_ns + span / 2, timing_to_media_ntp(sample.timing_wire_ntp),
                half + sample.synchronization_uncertainty_ns};
    if (observed_)
      check(next.midpoint_ns > observed_->midpoint_ns &&
                sample.last_sync_ns >= observed_->sample.last_sync_ns,
            "media clock sample went backwards");
    if (anchor_) {
      const auto elapsed = next.midpoint_ns - anchor_->midpoint_ns;
      check(elapsed <= (suspended_ ? limits_.max_relock_gap_ns : limits_.max_anchor_age_ns),
            "media clock sample continuity expired");
      const auto predicted = anchor_->media_ntp + ntp_delta(elapsed);
      // Each correlated measurement is an interval, not an exact offset.
      // Compare phase beyond the two measured uncertainty intervals against
      // the unchanged phase policy, matching the network clock's admission.
      const auto phase = magnitude(distance(next.media_ntp, predicted));
      const auto bound =
          limits_.max_phase_adjustment_ns + anchor_->uncertainty_ns + next.uncertainty_ns;
      if (phase > bound)
        throw std::invalid_argument(
            "media clock discontinuity requires a new session timeline: phase_ns=" +
            std::to_string(phase) + " bound_ns=" + std::to_string(bound));
      // Querying the old synchronization again cannot end an outage. Keep the
      // last accepted anchor for continuity checks until a real new sync arrives.
      if (suspended_ && sample.last_sync_ns == anchor_->sample.last_sync_ns) {
        observed_ = next;
        return;
      }
    }
    observed_ = next;
    suspended_ = false;
    anchor_ = next;
    history_.push_back(next);
    while (history_.size() > 32 ||
           (history_.size() > 1 &&
            next.midpoint_ns - history_.front().midpoint_ns > limits_.max_anchor_age_ns))
      history_.pop_front();
  } catch (...) {
    failed_ = true;
    anchor_.reset();
    observed_.reset();
    history_.clear();
    throw;
  }
}
bool MediaClock::ready(std::uint64_t now) const {
  return !failed_ && !suspended_ && anchor_ && now >= anchor_->sample.after_ns &&
         now - anchor_->midpoint_ns <= limits_.max_anchor_age_ns &&
         now >= anchor_->sample.last_sync_ns &&
         now - anchor_->sample.last_sync_ns <= limits_.max_sync_age_ns;
}
std::uint64_t MediaClock::media_ntp_at(std::uint64_t now) const {
  check(ready(now), "media clock is not ready for this host timestamp");
  return anchor_->media_ntp + ntp_delta(now - anchor_->midpoint_ns);
}
MediaTime MediaClock::presentation_at(std::uint64_t presented, std::uint64_t received) const {
  check(ready(received), "media clock is not synchronized at presentation feedback");
  check(presented <= received && received - presented <= limits_.max_anchor_age_ns,
        "presentation feedback is stale or in the future");
  for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
    if (presented < it->sample.after_ns) continue;
    check(presented - it->midpoint_ns <= limits_.max_anchor_age_ns &&
              presented >= it->sample.last_sync_ns &&
              presented - it->sample.last_sync_ns <= limits_.max_sync_age_ns,
          "historical presentation clock is stale");
    const auto ntp = it->media_ntp + ntp_delta(presented - it->midpoint_ns);
    return {ntp, double(ntp >> 32) + double(ntp & 0xffffffffULL) / 4294967296.0,
            it->uncertainty_ns};
  }
  throw std::invalid_argument("presentation predates retained media clock coverage");
}
FrameTiming MediaClock::schedule(std::uint64_t sender, std::uint64_t now) const {
  const auto media_now = media_ntp_at(now);
  const auto lead = distance(sender, media_now);
  if (magnitude(lead) > limits_.max_frame_distance_ns) throw FrameOutsideWindow(lead);
  // Preserve the asset's absolute media timeline. Era rollover cannot silently
  // become an increasing double timeline: Stream independently rejects it and
  // the owning session must be replaced. Deadline arithmetic remains wrap-safe.
  const double pts = double(sender >> 32) + double(sender & 0xffffffffULL) / 4294967296.0;
  return {sender, pts, offset(now, lead), lead, anchor_->uncertainty_ns};
}
MediaTime MediaClock::capture_at(std::uint64_t captured, std::uint64_t observed) const {
  return presentation_at(captured, observed);
}
}  // namespace headunit::theme::video
