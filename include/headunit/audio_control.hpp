// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace headunit::audio_control {

struct Level {
  unsigned volume_percent = 100;
  bool muted = false;
  bool operator==(const Level&) const = default;
};
struct Command {
  std::uint64_t sequence = 0;
  Level level{};
  bool operator==(const Command&) const = default;
};
enum class RequestResult { accepted, stale, unavailable };
// Output-owner observations, not a speaker/device presentation receipt. Dates
// use CLOCK_MONOTONIC; receiver heartbeats never refresh this observation.
struct OutputTiming {
  // Accepted PCM horizon and time between output-owner turns, in nanoseconds.
  // These measure scheduling, not device xruns or phone/video synchronization.
  std::uint64_t scheduled_ahead_ns = 0, maximum_owner_gap_ns = 0;
  std::uint64_t last_underrun_owner_gap_ns = 0, last_underrun_late_ns = 0;
  // Actual appsrc occupancy, including its value at the last exhaustion.
  std::uint64_t queued_bytes = 0, last_underrun_queued_bytes = 0;
  bool operator==(const OutputTiming&) const = default;
};
struct OutputProgress {
  std::uint64_t sampled_ns = 0, accepted_blocks = 0, discontinuities = 0, underruns = 0;
  bool starved = false;
  std::optional<OutputTiming> timing = std::nullopt;
  bool operator==(const OutputProgress&) const = default;
};
struct Snapshot {
  bool available = false;
  Command requested{}, applied{};
  std::optional<OutputProgress> output = std::nullopt;
  bool operator==(const Snapshot&) const = default;
};

// One fixed, latest-request mailbox for the head unit's local audio mixer.
// request/snapshot are thread-safe; all mixer-owner operations belong to the
// thread that first opens it. This object performs no mixer or device I/O.
class Mailbox {
 public:
  explicit Mailbox(Level initial = {});
  Mailbox(const Mailbox&) = delete;
  Mailbox& operator=(const Mailbox&) = delete;
  // A request is not an applied mixer reading. Positive increasing sequence
  // numbers replace only the pending latest request; no command is retried.
  // Invalid inputs throw without changing history, even while unavailable.
  RequestResult request(Command);
  Snapshot snapshot() const;
  // Call only after the owner has actually configured the initial mixer level.
  // Claims the owner exactly once. Reopening, including after close, is invalid.
  void open();
  // Lease at most one latest pending command. While a lease is outstanding,
  // returns no additional work even if a newer request arrives.
  std::optional<Command> next();
  // Confirm only after applying this exact leased command. A newer concurrent
  // request remains pending; confirming the old lease never publishes it.
  void applied(const Command&);
  // Owner-only and allocation-free. Regressing dates/counters and changed
  // observations with the same date are rejected without changing state.
  void observe_output(OutputProgress);
  // Terminal, owner-only and idempotent after open. Retires any outstanding
  // lease and availability, retaining requested/applied history for diagnosis.
  void close();

 private:
  mutable std::mutex mutex_;
  std::optional<std::thread::id> owner_{};
  Snapshot state_{};
  std::optional<Command> lease_{};
  bool closed_ = false;
  void check_owner() const;  // Caller holds mutex_.
};

// CLOCK_MONOTONIC nanoseconds, matching Python clock_gettime_ns(CLOCK_MONOTONIC).
// Do not substitute a platform-specific steady_clock origin for the wire.
std::uint64_t monotonic_time_ns();

}  // namespace headunit::audio_control
