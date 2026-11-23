// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <string>
#include <vector>

namespace headunit {
// Display intent only: no actuator state and no synthesized vehicle sample.
struct DriveModeProfile {
  std::string id, name, layout, format, palette, accent, wallpaper;
  bool sport_screen_available = false;
  bool operator==(const DriveModeProfile&) const = default;
};
struct DriveModeProfiles {
  std::string scope, selected;
  std::uint64_t revision = 0;
  std::vector<DriveModeProfile> profiles, defaults;
};
struct DriveModePresentation {
  std::optional<DriveModeProfile> profile;
  std::string source = "unavailable";
  std::optional<VehicleSignalTiming> timing;
  std::uint64_t vehicle_generation = 0, revision = 0;
};
DriveModeProfiles parse_drive_mode_profiles(std::string_view);
DriveModePresentation resolve_drive_mode(const DriveModeProfiles&, const VehicleSample&,
                                         Clock::time_point);
// Private, bounded file reader, no blocking special files. A malformed or missing
// replacement withdraws intent; it never keeps a retired user choice alive.
class DriveModeProfilesFile {
 public:
  explicit DriveModeProfilesFile(std::string path, std::string scope = {});
  DriveModePresentation poll(const VehicleSample&, Clock::time_point);
  const std::string& scope() const noexcept { return scope_; }
  const std::string& path() const noexcept { return path_; }
  const std::string& error() const noexcept { return error_; }

 private:
  std::string path_, scope_, error_;
  std::optional<DriveModeProfiles> profiles_;
  Clock::time_point next_read_{};
};
void publish_drive_mode_status(const std::string& path, std::string_view scope,
                               const DriveModePresentation&, std::string_view phase,
                               std::string_view error, Clock::time_point,
                               std::string_view nonce = {},
                               std::uint64_t connection_generation = 0);
}  // namespace headunit
