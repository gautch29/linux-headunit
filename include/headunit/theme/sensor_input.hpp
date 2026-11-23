// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/vehicle_service.hpp>
#include <memory>
#include <string>

namespace headunit::theme::vehicle {
// Local, receiver-owned input from a sensor process. The descriptor must be an
// inherited AF_UNIX SOCK_SEQPACKET socket, never a phone/network channel. Each
// bounded OPACK packet is one {iid, sampledMs, value} or {iid, sampledMs,
// invalid:true} record. Both processes use CLOCK_MONOTONIC milliseconds.
class SensorInput {
 public:
  SensorInput(int fd, std::shared_ptr<ReadOnlyService>);
  ~SensorInput();
  SensorInput(const SensorInput&) = delete;
  SensorInput& operator=(const SensorInput&) = delete;
  int fd() const { return fd_; }
  bool active() const { return fd_ >= 0; }
  const std::string& failure() const { return failure_; }
  // Process at most 32 records per call on the vehicle service's owner thread.
  // Provider EOF, malformed input or a rejected sample clears timed readings
  // and disables this generation; no partial/ambiguous sample is published.
  std::size_t pump(std::uint64_t now_ms);

 private:
  int fd_ = -1;
  std::shared_ptr<ReadOnlyService> source_;
  std::string failure_;
  void fail(std::string reason, std::uint64_t now_ms);
};
}  // namespace headunit::theme::vehicle
