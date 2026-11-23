// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/drive_mode_profile.hpp>
#include <headunit/theme/layout_control.hpp>

namespace headunit::theme::script {
// Owned by the existing cluster VM. A profile is one combined UI-Sync request;
// applied presentation is reported only after ready/idle layout+palette match.
class DriveModeControl {
 public:
  DriveModeControl(std::string path, std::string verified_scope,
                   LayoutControl::Publisher publisher = {});
  void poll(const VehicleSample&, Clock::time_point, const LayoutControl::ClusterSelection&);

 private:
  DriveModeProfilesFile profiles_;
  std::optional<DriveModeProfile> target_;
  Clock::time_point next_poll_{}, requested_at_{};
  std::string phase_, error_;
  LayoutControl::Publisher publisher_;
};
}  // namespace headunit::theme::script
