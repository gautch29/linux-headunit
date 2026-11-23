// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/frame_schedule.hpp>
#include <initializer_list>
#include <stdexcept>
using namespace headunit;
void check(bool v) {
  if (!v) throw std::runtime_error("frame schedule check failed");
}
int main() {
  constexpr std::uint64_t ms = 1000000, p = ui_frame_period_ns;
  auto deadline = next_frame_deadline(0, 10 * ms, p);
  check(deadline == p);  // Work consumes the budget.
  deadline = next_frame_deadline(deadline, p + 10 * ms, p);
  check(deadline == 2 * p);                              // No completion-time drift.
  check(next_frame_deadline(0, 25 * ms, p) == 2 * p);    // Over-budget work skips a slot.
  check(next_frame_deadline(2 * p, 2 * p, p) == 3 * p);  // Exact boundary advances once.
  check(next_frame_deadline(2 * p, 2 * p - 1, p) ==
        2 * p);  // Early wakeup does not move the phase.
  const auto after_stall = next_frame_deadline(2 * p, 5000000000ULL, p);
  check(after_stall > 5000000000ULL && after_stall <= 5000000000ULL + p && after_stall % p == 0);
  check(next_frame_deadline(after_stall, 5000000000ULL, p) == after_stall);  // No catch-up debt.
  // The panel can be polled rapidly while one receipt is pending. Repeated
  // polls do not move the draw slot; actual presentation remains a separate gate.
  const auto panel = next_frame_deadline(0, 123 * ms, p);
  for (auto now = 123 * ms; now < panel; now += 1000)
    check(next_frame_deadline(panel, now, p) == panel);
  bool bad = false;
  try {
    (void)next_frame_deadline(0, 0, 0);
  } catch (const std::invalid_argument&) {
    bad = true;
  }
  check(bad);
  bad = false;
  try {
    (void)next_frame_deadline(0, UINT64_MAX, 1);
  } catch (const std::overflow_error&) {
    bad = true;
  }
  check(bad);
  // Two panels taking 22 ms share one worker. Fixed-phase pacing adds 11.3 ms
  // of avoidable idle on every turn, limiting it to 30 Hz. Owner pacing uses
  // the actual available capacity; it does not invent extra source frames.
  std::uint64_t owner_start = 0, phased_start = 0;
  for (unsigned turn = 0; turn < 300; ++turn) {
    owner_start = next_owner_frame_deadline(owner_start, owner_start + 22 * ms, p);
    phased_start = next_frame_deadline(phased_start, phased_start + 22 * ms, p);
  }
  check(owner_start == 6600 * ms);
  check(phased_start == 600 * p);
  check(next_owner_frame_deadline(0, 10 * ms, p) == p);
  check(next_owner_frame_deadline(p, 2 * p, p) == 2 * p);
  check(next_owner_frame_deadline(0, 5 * 1000000000ULL, p) == 5 * 1000000000ULL);
  // After a stall or a late wakeup, a cheap next turn cannot replay old slots
  // back-to-back. A feedback-only turn does not move this frame deadline.
  check(next_owner_frame_deadline(5 * 1000000000ULL, 5 * 1000000000ULL + ms, p) ==
        5 * 1000000000ULL + p);
  check(next_owner_frame_deadline(2 * p + ms, 2 * p + 2 * ms, p) == 3 * p + ms);
  for (const auto invalid : {0U, 1U, 2U}) {
    bad = false;
    try {
      if (invalid == 0)
        (void)next_owner_frame_deadline(0, 0, 0);
      else if (invalid == 1)
        (void)next_owner_frame_deadline(2, 1, p);
      else
        (void)next_owner_frame_deadline(UINT64_MAX, UINT64_MAX, p);
    } catch (const std::invalid_argument&) {
      bad = invalid != 2;
    } catch (const std::overflow_error&) {
      bad = invalid == 2;
    }
    check(bad);
  }
}
