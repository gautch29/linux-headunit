// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace headunit::audio_observation {
// Values are numeric, private observations, never PCM or phone identities.
// Dates are acquired at the named boundary in host CLOCK_MONOTONIC. PTS and
// RTP sample time retain their own domains; no clock conversion is implied.
constexpr std::uint64_t unavailable = UINT64_MAX;
enum class Kind : std::uint64_t {
  stream_start = 1,
  stream_stop,
  decoder_start,
  decoder_stop,
  route_start,
  route_stop,
  flush_requested,
  flush_begin,
  flush_applied,
  flush_skipped,
  resource,
  focus,
  receive_turn,
  datagram,
  authenticated,
  rejected,
  truncated,
  pre_resume,
  decoder_submit,
  decoder_reject,
  jitter_enter,
  jitter_release,
  decoded_buffer,
  decoded_pull,
  pcm_pull,
  jitter_counts,
  decoder_counts,
  supply,
  supply_blocked,
  channel_pop,
  channel_detach,
  output_accept,
  output_exhausted,
  resource_requested,
  focus_requested
};
struct Event {
  std::uint64_t date_ns = 0;
  Kind kind{};
  std::uint64_t source = 0, route = 0, epoch = 0;
  std::uint64_t a = unavailable, b = unavailable, c = unavailable, d = unavailable;
};
struct Counters {
  std::uint64_t recorded = 0, contention = 0, overflow = 0, budget = 0, lifecycle_overflow = 0;
};
class Capture {
 public:
  static constexpr std::size_t capacity = 8192, lifecycle_capacity = 128;
  static constexpr std::uint64_t maximum_budget = 262144;
  explicit Capture(std::uint64_t budget = maximum_budget);
  std::uint64_t identity() noexcept;
  // Nonblocking for every owner. Lifecycle uses a separate fixed reservoir,
  // so budget exhaustion preserves exits. Contention/overflow remains explicit.
  void record(Event, bool lifecycle = false) noexcept;
  std::size_t take(std::span<Event> destination);
  Counters counters() const;
  void stop() noexcept;

 private:
  mutable std::mutex mutex_;
  std::array<Event, capacity> events_{};
  std::array<Event, lifecycle_capacity> lifecycle_{};
  std::size_t head_ = 0, count_ = 0, lifecycle_head_ = 0, lifecycle_count_ = 0;
  std::uint64_t remaining_;
  Counters counts_;
  std::atomic<std::uint64_t> identities_{0}, contention_{0};
  std::atomic<bool> stopped_{false};
};
struct Point {
  std::shared_ptr<Capture> capture;
  std::uint64_t source = 0, route = 0, epoch = 0;
  void observe(Kind, std::uint64_t a = unavailable, std::uint64_t b = unavailable,
               std::uint64_t c = unavailable, std::uint64_t d = unavailable,
               bool lifecycle = false) const noexcept;
};
// One supervised storage process owns ALL private-file I/O and encoding. The
// parent/control owner copies <=8x64 numeric events per poll to fixed-buffer
// nonblocking Unix datagrams. A stalled collector drops diagnostics, not PCM.
class File {
 public:
  enum class State { starting, running, finished, failed, cancelled };
  struct Export {
    std::uint64_t written_events = 0, partial_bytes = 0, error = 0;
    std::uint64_t encoding_ns = 0, write_ns = 0, write_calls = 0, written_bytes = 0;
  };
  struct Status {
    State state = State::starting;
    int collector_pid = -1;
    bool reaped = false;
    std::uint64_t transport_dropped_events = 0, transport_dropped_batches = 0;
    std::uint64_t transport_error = 0;
    std::optional<Export> exporter;
  };
  File(const std::string& path, const std::string& executable);
  ~File();
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  std::shared_ptr<Capture> capture() const;
  bool poll() noexcept;
  // Shutdown only: seal, attempt finite drain, allow 100ms graceful completion,
  // then SIGKILL and observe waitpid for <=100ms. Never call on an audio owner.
  bool finish() noexcept;
  Status status() const noexcept;
  // Same-executable private collector entry, before ordinary receiver startup.
  static std::optional<int> collector_main(int argc, char** argv) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::audio_observation
