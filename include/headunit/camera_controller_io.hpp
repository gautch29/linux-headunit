// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace headunit::camera {
// Single control-owner APIs. Parent directories must be real, owned and 0700;
// endpoint files must be owned single-link 0600 regular data. Constructors do
// not create directories. Locks are nonblocking and remain pinned to an inode.
class RequestPublisher {
 public:
  RequestPublisher(std::string request_path, std::string owner_path);
  ~RequestPublisher();
  RequestPublisher(const RequestPublisher&) = delete;
  RequestPublisher& operator=(const RequestPublisher&) = delete;
  // Writes off on construction, and attempts off before releasing ownership.
  // ON requires a positive explicit CLOCK_MONOTONIC deadline; the activation
  // reader separately enforces its expiry and maximum future lease interval.
  void publish(bool on, std::uint64_t deadline_ns = 0);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
enum class ControlPhase { idle, waiting, active, fault, stopped };
enum class Availability;
// Source eligibility for the existing owner heartbeat, not a presentation
// receipt. A closed source requires explicit Off -> On recovery. Invalid
// source clocks report fault; missing/stale frames remain transient waiting.
ControlPhase source_control_phase(bool requested, Availability);
struct ControlStatus {
  std::uint64_t instance_started_ns = 0, sequence = 0, observed_ns = 0;
  ControlPhase phase = ControlPhase::idle;
  bool operator==(const ControlStatus&) const = default;
};
class StatusPublisher {
 public:
  explicit StatusPublisher(std::string status_path);
  ~StatusPublisher();
  StatusPublisher(const StatusPublisher&) = delete;
  StatusPublisher& operator=(const StatusPublisher&) = delete;
  // Owns status_path+".owner". One instance per publisher, positive increasing
  // sequence, and nondecreasing genuine local observation dates. No fsync:
  // these files are ephemeral heartbeats, not durable camera preferences.
  void publish(const ControlStatus&);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class StatusReader {
 public:
  explicit StatusReader(std::string status_path);
  ~StatusReader();
  StatusReader(const StatusReader&) = delete;
  StatusReader& operator=(const StatusReader&) = delete;
  // Reads original observation time, never file mtime or this poll's time as
  // a replacement heartbeat. Repeated identical current data is allowed only
  // while still fresh. After loss/expiry it needs a strictly newer snapshot;
  // rollback/replay/conflicting equal sequence fails closed. A reversed input
  // clock retires this reader; construct a new reader for a new clock domain.
  // Control status is not camera acquisition or a presentation receipt.
  std::optional<ControlStatus> poll(std::uint64_t now_ns, unsigned maximum_age_ms) noexcept;
  // Production polling samples genuine local CLOCK_MONOTONIC time after the
  // bounded read, so a normal concurrent update does not appear future-dated.
  // The explicit-clock overload above remains available for clock fixtures.
  std::optional<ControlStatus> poll(unsigned maximum_age_ms) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::camera
