// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace headunit::camera {
enum class ManualCloseState { idle, pending, accepted, failed };
struct ManualCloseResult {
  ManualCloseState state = ManualCloseState::idle;
  std::uint64_t request_sequence = 0;
  std::string message;
};
// Optional native Close request to the dedicated manual-camera command owner.
// Configured clients have one owned worker; unconfigured clients allocate no
// thread or descriptors. No default socket, environment lookup or constructor
// connection. Request/status do no socket I/O, and pending requests coalesce.
// "accepted" means a complete positive command-owner reply, never proof that the
// camera disappeared or its GPU/surface ownership has been returned.
class ManualCloseClient {
 public:
  explicit ManualCloseClient(std::optional<std::string> socket_path = std::nullopt,
                             unsigned timeout_ms = 500);
  ~ManualCloseClient();
  ManualCloseClient(const ManualCloseClient&) = delete;
  ManualCloseClient& operator=(const ManualCloseClient&) = delete;
  bool configured() const noexcept;
  bool request_close() noexcept;
  ManualCloseResult status() const;
  // Asynchronous cancellation; wakes both idle and I/O waits. The destructor
  // joins this one bounded worker outside the rendering turn.
  void request_stop() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::camera
