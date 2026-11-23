// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/vehicle_configuration.hpp>
#include <mutex>

namespace headunit::theme::vehicle {
// One connection's camera owner publishes only copied state and genuine local
// dates. Neither phone services nor script/GPU objects cross this boundary.
// This is fresh-source eligibility, not a physical presentation receipt.
class CameraStateSnapshot {
 public:
  enum class OffStatus { pending, applied, timed_out, retired };
  struct OffRequest {
    std::uint64_t generation = 0, sequence = 0, requested_ns = 0, deadline_ns = 0;
  };
  struct OffResult {
    OffStatus status = OffStatus::pending;
    std::uint64_t withdrawn_ns = 0, presented_ns = 0;
  };
  // Local policy, not an Apple protocol limit. One request/result, no queue.
  static constexpr std::uint64_t maximum_off_wait_ns = 5'000'000'000;
  std::uint64_t begin(std::uint64_t now_ns);
  bool observe(std::uint64_t generation, bool visible, std::uint64_t capture_ns,
               std::uint64_t observed_ns);
  void retire(std::uint64_t generation, std::uint64_t now_ns) noexcept;
  void close(std::uint64_t now_ns) noexcept;
  Sample read(std::uint64_t now_ms, std::uint64_t maximum_age_ms);
  std::optional<OffRequest> request_off(std::uint64_t now_ns, std::uint64_t deadline_ns);
  std::optional<OffRequest> pending_off(std::uint64_t generation, std::uint64_t now_ns);
  bool owner_off(std::uint64_t generation, std::uint64_t sequence, std::uint64_t now_ns);
  void cancel_off(std::uint64_t generation) noexcept;
  void cancel_off(std::uint64_t generation, std::uint64_t sequence) noexcept;
  std::uint64_t off_for_composition(std::uint64_t generation, std::uint64_t now_ns);
  // Only the existing display owner calls this after accepting real feedback
  // for a newly composed scene with no camera source token. Eligibility OFF,
  // a discarded commit or an unrelated presentation cannot call this seam.
  bool presented_off(std::uint64_t generation, std::uint64_t sequence,
                     std::uint64_t submission_epoch, std::uint64_t current_epoch,
                     std::uint64_t draw, std::uint64_t submitted_ns, std::uint64_t presented_ns,
                     std::uint64_t observed_ns);
  std::optional<OffResult> off_result(std::uint64_t generation, std::uint64_t sequence,
                                      std::uint64_t now_ns);

 private:
  mutable std::mutex mutex_;
  std::uint64_t generation_ = 0, observed_ns_ = 0, capture_ns_ = 0, off_ns_ = 0;
  bool active_ = false, visible_ = false, closed_ = false;
  std::optional<OffRequest> off_request_;
  OffResult off_result_;
  std::uint64_t next_off_sequence_ = 0;
  bool pending_off_locked(std::uint64_t generation, std::uint64_t now_ns);
  void retire_off_locked() noexcept;
};
// The two phone On characteristics alias the same owner observation. Other
// Climate/settings bindings retain their explicit provider; camera writes are
// rejected by default. Optional genuine owner time enables only the bounded
// C++ Off completion seam; parsed phone capabilities remain read-only.
std::shared_ptr<Provider> camera_state_provider(std::shared_ptr<Provider>, const CameraCapability&,
                                                std::shared_ptr<CameraStateSnapshot>,
                                                std::function<std::uint64_t()> owner_now_ns = {});
}  // namespace headunit::theme::vehicle
