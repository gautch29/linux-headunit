// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/local_vehicle_profile.hpp>
#include <headunit/theme/script_runtime.hpp>

namespace headunit::theme::script {
// One worker's local target scene. Authenticated session creation authorizes
// STARTUP; signed cluster completion plus qualified visibility authorizes DRIVE.
// This is UI lifecycle, not an ignition/gear observation or phone readiness.
class SceneLifecycle {
 public:
  enum class Launch { startup, first_pairing, resume };
  SceneLifecycle(const vehicle::LocalProfile&, Launch);
  void pump(std::uint64_t now_ms, Runtime::ScenePhase cluster_phase, bool qualified_visible);
  void retire(std::uint64_t now_ms);
  bool driving() const { return driving_; }

 private:
  std::shared_ptr<vehicle::ReadOnlyService> source_;
  vehicle::LocalProfile::SceneInputs ids_;
  Launch launch_;
  bool saw_logos_ = false, driving_ = false, retired_ = false;
  std::optional<std::uint64_t> last_publication_, animation_started_;
};
}  // namespace headunit::theme::script
