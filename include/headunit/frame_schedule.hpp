// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace headunit {
inline constexpr std::uint64_t ui_frame_period_ns = 16666667;
// First scheduled deadline strictly after now; skip missed slots in O(1).
// The deadline's phase is preserved. No render completion delay is added and
// no debt accumulates for a stalled owner to replay as a burst of frames.
inline std::uint64_t next_frame_deadline(std::uint64_t deadline, std::uint64_t now,
                                         std::uint64_t period) {
  if (!period) throw std::invalid_argument("frame period must be positive");
  if (deadline > now) return deadline;
  const auto remaining = period - (now - deadline) % period;
  if (now > std::numeric_limits<std::uint64_t>::max() - remaining)
    throw std::overflow_error("frame deadline overflow");
  return now + remaining;
}

// Pace a serial owner from its actual turn start. Over-budget work may resume
// immediately, instead of idling until another fixed-phase slot. Every new
// turn still starts at least one period after the last start; there is no
// catch-up debt. These are local scheduler dates, never media/presentation PTS.
inline std::uint64_t next_owner_frame_deadline(std::uint64_t started, std::uint64_t finished,
                                               std::uint64_t period) {
  if (!period || finished < started) throw std::invalid_argument("invalid owner frame interval");
  if (started > std::numeric_limits<std::uint64_t>::max() - period)
    throw std::overflow_error("owner frame deadline overflow");
  const auto earliest = started + period;
  return finished > earliest ? finished : earliest;
}
}  // namespace headunit
