// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <span>

namespace headunit {

struct AirplayClockSample {
  std::uint64_t timing_ntp;
  std::uint64_t monotonic_ns;
  std::uint64_t last_sync_ns;
  std::uint64_t uncertainty_ns;
};

// One paired connection, one owner thread. The socket owner must check the
// negotiated peer address/port before receive(). Timing UDP has no cryptographic
// authentication; an echoed origin is correlation, not peer authentication.
// This estimates phone time, never adjusts the host's system clock.
class AirplayClock {
 public:
  enum class FaultReason { none, phase_jump, relock_gap };
  struct Fault {
    FaultReason reason = FaultReason::none;
    // Relative measurements only; never retain wire origins in diagnostics.
    std::uint64_t phase_delta_ns = 0, phase_bound_ns = 0, accepted_age_ns = 0;
    std::uint64_t previous_uncertainty_ns = 0, next_uncertainty_ns = 0;
    std::uint64_t exchange_ns = 0, processing_ns = 0;
  };
  struct Diagnostics {
    std::uint64_t sent = 0, unmatched = 0, invalid_exchange = 0, delayed = 0;
    std::uint64_t last_delay_ns = 0, max_delay_ns = 0, last_accepted_ns = 0;
    Fault fault;
  };
  Diagnostics diagnostics() const { return diagnostics_; }
  static constexpr std::uint64_t max_exchange_ns = 1000000000;
  static constexpr std::uint64_t max_network_delay_ns = 20000000;
  // Matches MediaClock's synchronization lifetime. At 500 ppm, 10 seconds
  // adds at most 5 ms to the <=10.002 ms exchange uncertainty.
  static constexpr std::uint64_t max_sample_age_ns = 10000000000;
  static constexpr std::uint64_t max_relock_gap_ns = 30000000000;

  // Register only a successfully sent request. before_send_ns is sampled
  // immediately before sendto; the origin is the exact NTP value sent on wire.
  void sent(std::uint64_t origin, std::uint64_t before_send_ns);
  // after_receive_ns is sampled immediately after nonblocking recvfrom.
  // A matching reply consumes the outstanding request, including on rejection.
  bool receive(std::span<const std::uint8_t>, std::uint64_t after_receive_ns);
  // Three consistent low-delay exchanges qualify the initial/reacquired clock.
  // Re-querying projects an existing sample; it never refreshes last_sync_ns.
  std::optional<AirplayClockSample> sample(std::uint64_t now_ns) const;
  // Ask more often before aging measurements expire, without widening the
  // acceptance window or treating rejected replies as synchronization.
  std::uint64_t probe_interval_ns(std::uint64_t now_ns) const;
  bool failed() const { return failed_; }
  std::uint64_t accepted() const { return accepted_; }

 private:
  struct Request {
    std::uint64_t origin, before_ns;
  };
  Diagnostics diagnostics_;
  std::optional<Request> pending_;
  std::optional<Request> last_request_;
  std::optional<AirplayClockSample> last_observed_;
  std::deque<AirplayClockSample> samples_;
  std::uint64_t accepted_ = 0;
  bool qualified_ = false, failed_ = false;
};

}  // namespace headunit
