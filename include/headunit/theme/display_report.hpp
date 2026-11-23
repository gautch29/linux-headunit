// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>

namespace headunit::theme::script {
// One panel's existing finite diagnostic series. Owner-thread only. Dates are
// the actual steady-clock samples passed by the reporting owner, never file or
// polling dates. A new renderer owns a new series with no predecessor.
class DisplayReportSeries {
 public:
  using Clock = std::chrono::steady_clock;
  struct Window {
    unsigned sequence = 0;
    Clock::time_point at;
    std::optional<Clock::time_point> previous;
    std::optional<unsigned> previous_sequence;
    // Used only at the existing report cadence. Invalid/unavailable publisher
    // identities remain null, never arbitrary text or generated replacements.
    void write_identity(std::ostream& log, std::uint64_t owner_started_ns, std::string_view nonce,
                        std::uint64_t connection_generation, unsigned panel) const {
      const auto ns = [](auto date) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(date.time_since_epoch())
            .count();
      };
      log << "Ultra display diagnostic panel=" << panel << " mono_ns=" << ns(at)
          << " reportSequence=" << sequence << " previousReportSequence=";
      if (previous_sequence)
        log << *previous_sequence;
      else
        log << "null";
      log << " previousReportMonoNs=";
      if (previous)
        log << ns(*previous);
      else
        log << "null";
      log << " reportOwnerStartedMonoNs=" << owner_started_ns
          << " reportClock=host-steady publisherNoncePrivate=";
      const bool publisher_available =
          nonce.size() == 32 && nonce.find_first_not_of("0123456789abcdef") == nonce.npos;
      if (publisher_available)
        log << nonce;
      else
        log << "null";
      log << " publisherConnectionGeneration=";
      if (publisher_available)
        log << connection_generation;
      else
        log << "null";
      // Role follows the configured panel binding, not a physical receipt.
      log << " outputRole=" << (panel == 0 ? "main" : "cluster") << " frameCorrelation=null";
    }
  };
  std::optional<Window> observe(Clock::time_point at, std::uint64_t scene_presentations,
                                unsigned budget) {
    if (scene_presentations < 5 || sequence_ >= budget || at < next_) return {};
    Window window{++sequence_, at, previous_, {}};
    if (previous_) window.previous_sequence = sequence_ - 1;
    previous_ = at;
    next_ = at + std::chrono::seconds(5);
    return window;
  }

 private:
  unsigned sequence_ = 0;
  Clock::time_point next_{};
  std::optional<Clock::time_point> previous_;
};
}  // namespace headunit::theme::script
