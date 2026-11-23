// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/airplay_clock.hpp>
#include <headunit/theme/media_clock.hpp>

namespace headunit::theme::video {

// Display-owner bridge from the independent receiver's projected phone clock.
// Refresh on regular event-loop turns, including turns with no video packets.
// No input or a scheduling pause suspends PTS; a fresh accepted exchange is
// required to resume the same bounded timeline. Never reports presentation.
class ReceiverClock {
 public:
  void update(const std::optional<AirplayClockSample>& timing) {
    if (failed_) throw std::invalid_argument("receiver clock bridge is faulted");
    try {
      if (timing && timing->monotonic_ns < last_query_)
        throw std::invalid_argument("receiver clock query went backwards");
      if (!timing || (clock_.acquired() && !clock_.ready(timing->monotonic_ns))) clock_.suspend();
      // Preserve a useful history for delayed presentation feedback. Updating
      // at every frame would consume the 32-anchor history in a fraction of a
      // second when both video streams are busy. New network anchors bypass
      // the 100 ms refresh interval.
      if (timing && timing->monotonic_ns > last_sample_ &&
          (timing->last_sync_ns != last_sync_ ||
           timing->monotonic_ns - last_sample_ >= 100000000)) {
        clock_.update({true, timing->timing_ntp, timing->monotonic_ns, timing->monotonic_ns,
                       timing->last_sync_ns, timing->uncertainty_ns});
        last_sample_ = timing->monotonic_ns;
        last_sync_ = timing->last_sync_ns;
      }
      if (timing) last_query_ = timing->monotonic_ns;
    } catch (...) {
      failed_ = true;
      if (!clock_.failed()) clock_.suspend();
      throw;
    }
  }
  const MediaClock& clock() const { return clock_; }
  bool failed() const { return failed_; }

 private:
  MediaClock clock_;
  std::uint64_t last_sample_ = 0, last_sync_ = 0, last_query_ = 0;
  bool failed_ = false;
};
}  // namespace headunit::theme::video
