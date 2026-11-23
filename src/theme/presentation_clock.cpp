// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/presentation_clock.hpp>
#include <limits>
#include <stdexcept>

namespace headunit::theme::video {
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::invalid_argument(why);
}
constexpr std::uint64_t second = 1000000000;
std::uint64_t delta(std::uint64_t a, std::uint64_t b) {
  return a > b ? a - b : b - a;
}
std::uint64_t midpoint(const ClockCorrelation& c) {
  return c.before_ns + (c.after_ns - c.before_ns) / 2;
}
std::uint64_t halfspan(const ClockCorrelation& c) {
  return (c.after_ns - c.before_ns + 1) / 2;
}
std::uint64_t drift(std::uint64_t ns, std::uint32_t ppm) {
  // Inputs are bounded to seconds; round up rather than underreport error.
  return (ns * ppm + 999999) / 1000000;
}
}  // namespace
PresentationClock::PresentationClock(PresentationLimits limits) : limits_(limits) {
  check(limits.max_sample_span_ns > 0 && limits.max_sample_span_ns <= second &&
            limits.max_feedback_age_ns > 0 && limits.max_feedback_age_ns <= 60 * second &&
            limits.max_uncertainty_ns > 0 && limits.max_uncertainty_ns <= second &&
            limits.max_drift_ppm > 0 && limits.max_drift_ppm <= 10000,
        "invalid presentation clock policy");
}
bool presentation_time_reached(const PresentationFeedback& f, const ClockCorrelation& c) {
  check(f.presented && c.clock_id == f.clock_id && f.nanoseconds < second &&
            f.seconds <= (std::numeric_limits<std::uint64_t>::max() - f.nanoseconds) / second,
        "invalid presentation correlation request");
  const auto presented = f.seconds * second + f.nanoseconds;
  if (presented <= c.source_ns) return true;
  check(presented - c.source_ns <= 2000000, "presentation receipt exceeds early-arrival bound");
  return false;
}
std::optional<PresentationTime> PresentationClock::accept(const PresentationFeedback& f,
                                                          const MediaClock& media) {
  check(!failed_, "presentation clock is faulted");
  try {
    check(f.token > last_token_ && f.received_ns >= last_received_,
          "out-of-order presentation feedback");
    check(!clock_id_ || *clock_id_ == f.clock_id, "presentation clock domain changed");
    clock_id_ = f.clock_id;
    last_token_ = f.token;
    last_received_ = f.received_ns;
    if (!f.presented) return {};  // No conversion or inferred presentation on discard.
    check(f.nanoseconds < second && !(f.flags & ~15u) &&
              f.seconds <= (std::numeric_limits<std::uint64_t>::max() - f.nanoseconds) / second,
          "invalid presentation timestamp");
    check(f.correlation.has_value(), "missing presentation clock correlation");
    const auto& c = *f.correlation;
    check(c.clock_id == f.clock_id && c.after_ns >= c.before_ns && c.after_ns <= f.received_ns &&
              c.after_ns - c.before_ns <= limits_.max_sample_span_ns,
          "invalid presentation clock sample");
    const auto presented = f.seconds * second + f.nanoseconds;
    // Correlation is sampled after receiving feedback, so future source times
    // indicate inconsistent clock domains or a malformed receipt.
    if (presented > c.source_ns || c.source_ns - presented > limits_.max_feedback_age_ns ||
        f.received_ns - c.before_ns > limits_.max_feedback_age_ns)
      throw std::invalid_argument(
          "stale or future presentation clock sample: presented=" + std::to_string(presented) +
          " sampled=" + std::to_string(c.source_ns) + " before=" + std::to_string(c.before_ns) +
          " received=" + std::to_string(f.received_ns));
    const auto mid = midpoint(c), distance = c.source_ns - presented;
    check(distance <= mid, "presentation precedes monotonic clock origin");
    if (previous_) {
      check(c.before_ns >= previous_->after_ns && c.source_ns >= previous_->source_ns,
            "presentation clock sample regressed");
      const auto elapsed = mid - midpoint(*previous_);
      check(elapsed <= limits_.max_feedback_age_ns, "presentation clock continuity expired");
      check(delta(c.source_ns - previous_->source_ns, elapsed) <=
                halfspan(c) + halfspan(*previous_) + drift(elapsed, limits_.max_drift_ppm),
            "presentation clock correlation jumped");
    }
    const auto monotonic = mid - distance;
    check(monotonic <= f.received_ns && f.received_ns - monotonic <= limits_.max_feedback_age_ns,
          "presentation feedback arrived too late");
    const auto value = media.presentation_at(monotonic, f.received_ns);
    const auto uncertainty =
        halfspan(c) + drift(distance, limits_.max_drift_ppm) + value.uncertainty_ns;
    check(uncertainty <= limits_.max_uncertainty_ns, "presentation time is too uncertain");
    check(!last_ || monotonic >= last_->monotonic_ns, "presentation monotonic timeline regressed");
    check(!last_ || value.ntp >= last_->media_ntp,
          "presentation media timeline regressed or wrapped");
    previous_ = c;
    last_ = PresentationTime{f.token, monotonic, value.ntp, uncertainty, value.pts_seconds};
    return last_;
  } catch (...) {
    failed_ = true;
    throw;
  }
}
void PresentationClock::resume_after_idle() {
  check(!failed_, "presentation clock is faulted");
  previous_.reset();
}
}  // namespace headunit::theme::video
