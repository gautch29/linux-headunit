// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/media_clock.hpp>

namespace headunit::theme::video {
// Sample the presentation clock between two CLOCK_MONOTONIC reads. The latter
// is the same Linux clock used by the paired timing transport and steady_clock.
struct ClockCorrelation {
  std::uint32_t clock_id = 0;
  std::uint64_t source_ns = 0, before_ns = 0, after_ns = 0;
};
struct PresentationFeedback {
  std::uint64_t token = 0, seconds = 0, sequence = 0;
  std::uint32_t nanoseconds = 0, refresh_ns = 0, flags = 0, clock_id = 0;
  bool presented = false;  // False is a compositor's explicit discarded event.
  std::optional<ClockCorrelation> correlation;
  std::uint64_t received_ns = 0;  // CLOCK_MONOTONIC, after sampling correlation.
};
struct PresentationLimits {
  // Host policy, not protocol constants. Drift allowance covers the short
  // difference between sampled clocks; it is included in output uncertainty.
  std::uint64_t max_sample_span_ns = 2000000, max_feedback_age_ns = 1000000000;
  std::uint64_t max_uncertainty_ns = 25000000;
  std::uint32_t max_drift_ppm = 2000;
};
// A DRM receipt can arrive fractionally before its reported timestamp. Defer
// consumption and resample the real clock on a later turn (at most 2 ms ahead).
// This never changes the receipt timestamp or relaxes PresentationClock::accept.
bool presentation_time_reached(const PresentationFeedback&, const ClockCorrelation&);
struct PresentationTime {
  std::uint64_t token, monotonic_ns, media_ntp, uncertainty_ns;
  double pts_seconds;
};
// One display/connection, on its owner thread. Only actual presented feedback
// advances the timeline. A discarded frame produces no PTS. Flags are preserved
// by the receipt: a headless compositor does not prove physical scanout.
// Any invalid mapping faults this instance; reset the owning session to relock.
class PresentationClock {
 public:
  explicit PresentationClock(PresentationLimits = {});
  std::optional<PresentationTime> accept(const PresentationFeedback&, const MediaClock&);
  // After an explicitly observed idle/hidden preview, use a fresh real clock
  // correlation. Retains token, domain and last-presented monotonic/PTS guards;
  // it neither supplies a PTS nor revives an already-faulted clock.
  void resume_after_idle();
  bool failed() const { return failed_; }

 private:
  PresentationLimits limits_;
  std::uint64_t last_token_ = 0, last_received_ = 0;
  std::optional<std::uint32_t> clock_id_;
  std::optional<ClockCorrelation> previous_;
  std::optional<PresentationTime> last_;
  bool failed_ = false;
};
}  // namespace headunit::theme::video
