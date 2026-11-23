// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace headunit::theme {
// Passive host-side touch-response timing for one phone video panel. The phone
// sends no frames while its screen is static, so the first frame decoded after
// a finger-down on an idle panel is attributed to that touch. Busy panels are
// skipped rather than guessed. Times are one host monotonic clock (ns).
//
// The measured span starts when the press is forwarded towards the phone and
// ends when the presentation receipt for the response is observed. It includes
// the phone, network, decode, staging and presentation queue; it excludes
// touchscreen sampling and panel scanout, so it is not touch-to-photon latency.
class TouchResponseProbe {
 public:
  static constexpr std::uint64_t idle_ns = 250000000;
  static constexpr std::uint64_t timeout_ns = 1000000000;

  struct Stats {
    std::uint64_t responses = 0, unanswered = 0, skipped_busy = 0;
    std::uint64_t decode_sum_ns = 0, decode_max_ns = 0;
    std::uint64_t present_sum_ns = 0, present_max_ns = 0;
  };

  // A new finger-down was forwarded to the phone.
  void press(std::uint64_t now) {
    expire(now);
    if (active_) return;  // Still waiting for the previous touch's response.
    if (last_decoded_ && now - *last_decoded_ < idle_ns) {
      ++stats_.skipped_busy;
      return;
    }
    active_ = Probe{now, std::nullopt};
  }

  // A phone frame for this panel finished decoding.
  void decoded(std::uint64_t now) {
    expire(now);
    last_decoded_ = now;
    if (active_ && !active_->decoded && now >= active_->pressed) active_->decoded = now;
  }

  // A presentation receipt for a frame decoded after the press was observed.
  void presented(std::uint64_t now) {
    if (!active_ || !active_->decoded || now < *active_->decoded) return;
    const auto decode_ns = *active_->decoded - active_->pressed;
    const auto present_ns = now - active_->pressed;
    ++stats_.responses;
    stats_.decode_sum_ns += decode_ns;
    stats_.decode_max_ns = std::max(stats_.decode_max_ns, decode_ns);
    stats_.present_sum_ns += present_ns;
    stats_.present_max_ns = std::max(stats_.present_max_ns, present_ns);
    active_.reset();
  }

  // Ends a probe whose response never arrived. Called by the other methods.
  void expire(std::uint64_t now) {
    if (active_ && now >= active_->pressed && now - active_->pressed > timeout_ns) {
      ++stats_.unanswered;
      active_.reset();
    }
  }

  const Stats& stats() const { return stats_; }

 private:
  struct Probe {
    std::uint64_t pressed;
    std::optional<std::uint64_t> decoded;
  };
  std::optional<std::uint64_t> last_decoded_;
  std::optional<Probe> active_;
  Stats stats_;
};
}  // namespace headunit::theme
