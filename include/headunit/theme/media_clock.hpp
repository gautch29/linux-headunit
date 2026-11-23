// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>

namespace headunit::theme::video {
// The paired session's timing transport adds this epoch to the screen clock.
// Conversion is modulo 2^64 and preserves all 32 fractional bits.
constexpr std::uint64_t timing_to_media_ntp(std::uint64_t timing_wire_ntp) {
  return timing_wire_ntp - (std::uint64_t{2208988800} << 32);
}
struct ClockSample {
  bool locked = false;
  std::uint64_t timing_wire_ntp = 0;
  // Same host monotonic clock for all three fields. The timestamp read took
  // place between before/after. last_sync is the most recent accepted network
  // synchronization, NOT merely the time syncedNtp() was queried.
  std::uint64_t before_ns = 0, after_ns = 0, last_sync_ns = 0;
  std::uint64_t synchronization_uncertainty_ns = 0;
};
struct ClockLimits {
  // Host scheduling policy, not protocol constants or a claimed Apple SLA.
  std::uint64_t max_sample_span_ns = 2000000;
  std::uint64_t max_uncertainty_ns = 20000000;
  std::uint64_t max_anchor_age_ns = 3000000000;
  std::uint64_t max_sync_age_ns = 10000000000;
  std::uint64_t max_phase_adjustment_ns = 20000000;
  std::uint64_t max_frame_distance_ns = 1000000000;
  std::uint64_t max_relock_gap_ns = 30000000000;
};
struct FrameTiming {
  std::uint64_t sender_ntp;
  // Asset metadata and DPVideoLayer PTS use this media domain. Host monotonic
  // deadlines must never be substituted into CP.PTS or transition metadata.
  double media_pts_seconds;
  std::uint64_t deadline_ns;
  std::int64_t lead_ns;
  std::uint64_t uncertainty_ns;
};
struct MediaTime {
  std::uint64_t ntp;
  double pts_seconds;
  std::uint64_t uncertainty_ns;
};
// An authenticated frame may be too old after decoder startup/backpressure.
// Distinguish that per-frame rejection from a fault in the clock itself so an
// owner can discard the frame without silently accepting it or losing sync.
struct FrameOutsideWindow : std::invalid_argument {
  std::int64_t lead_ns;
  explicit FrameOutsideWindow(std::int64_t lead)
      : std::invalid_argument("video timestamp outside scheduling window"), lead_ns(lead) {}
};
// Scheduling adapter for an already synchronized, paired session. It does not
// authenticate timing packets or estimate the network offset. One owner thread.
// Invalid samples and timeline steps are terminal. An owner that explicitly
// suspends must discard pending frames/receipts and stop reporting PTS. Relock
// requires a new accepted network measurement on the same bounded timeline.
// There is no wall-clock fallback; update(unlocked) remains strict by default.
class MediaClock {
 public:
  explicit MediaClock(ClockLimits = {});
  void update(ClockSample);
  void suspend();
  std::uint64_t suspension_epoch() const { return suspension_epoch_; }
  bool acquired() const { return anchor_.has_value(); }
  bool ready(std::uint64_t now_ns) const;
  bool failed() const { return failed_; }
  FrameTiming schedule(std::uint64_t authenticated_sender_ntp, std::uint64_t now_ns) const;
  // Convert an actual host presentation timestamp only when its clock domain
  // matches the sample. This operation itself asserts no presentation occurred.
  std::uint64_t media_ntp_at(std::uint64_t monotonic_ns) const;
  // Feedback arrives after presentation, possibly after a newer clock sample.
  // Map with the newest retained anchor that already existed at presentation;
  // require the current clock to remain synchronized at feedback receipt.
  // Retains at most 32 anchors. Missing historical coverage is an error.
  MediaTime presentation_at(std::uint64_t presented_ns, std::uint64_t received_ns) const;
  // Map a locally acquired capture using retained accepted anchors. Capture
  // may predate the latest query; this asserts neither phone authentication
  // of the pixels nor presentation. Same readiness/history limits as receipts.
  MediaTime capture_at(std::uint64_t captured_ns, std::uint64_t observed_ns) const;

 private:
  struct Anchor {
    ClockSample sample;
    std::uint64_t midpoint_ns, media_ntp, uncertainty_ns;
  };
  ClockLimits limits_;
  std::optional<Anchor> anchor_;
  std::optional<Anchor> observed_;
  std::deque<Anchor> history_;
  bool failed_ = false;
  bool suspended_ = false;
  std::uint64_t suspension_epoch_ = 0;
};
}  // namespace headunit::theme::video
