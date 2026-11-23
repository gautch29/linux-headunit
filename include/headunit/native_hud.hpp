// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/backup_cluster.hpp>
#include <headunit/route_guidance.hpp>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace headunit {
struct HudPoint {
  double x = 0, y = 0;
};
struct HudCalibration {
  bool reflect_horizontal = true, reflect_vertical = false;
  unsigned quarter_turns = 0;                    // Clockwise, 0..3.
  double scale = 1, offset_x = 0, offset_y = 0;  // Offsets are fractions of output size.
  // Measured destination corners, normalized TL,TR,BR,BL. Absent is identity.
  std::optional<std::array<HudPoint, 4>> corners;
};
// Rotation, reflection, scale/offset, then projective corner calibration.
// Defaults are a design starting point, not qualification of physical optics.
bool valid_hud_calibration(const HudCalibration&) noexcept;
void validate_hud_calibration(const HudCalibration&);

enum class HudSpeedUnit { kilometres_per_hour, miles_per_hour };
enum class HudSpeedSign { neutral, red_ring, yellow_red_ring, us, canada, australia };
struct HudRoadLocale {
  HudSpeedUnit unit = HudSpeedUnit::kilometres_per_hour;
  HudSpeedSign sign = HudSpeedSign::neutral;
};
// Empty is automatic/unknown; otherwise canonical two-letter country code.
bool valid_hud_country(std::string_view) noexcept;
HudRoadLocale hud_road_locale(std::string_view country) noexcept;
double hud_speed_value(double kmh, HudSpeedUnit) noexcept;
// Display conversion only: retain the original route distance in metres.
std::string hud_distance_text(std::uint32_t metres, HudSpeedUnit);

struct HudSpeedLimitObservation {
  double kmh = 0;
  VehicleSignalTiming timing;
  std::string source;
  // Country from the same dated road observation, never the phone's locale.
  std::string country{};
  HudSpeedUnit unit = HudSpeedUnit::kilometres_per_hour;
};
bool fresh(const HudSpeedLimitObservation&, Clock::time_point) noexcept;
// Explicit record (ASCII, <=512 bytes, provider source <=64 identifier bytes):
// hud-speed-limit-v1 value=50 unit=km/h acquired_ns=123 maximum_age_ns=1000000000
// origin=measured source=provider  (all fields on one line, in this order).
// v2 also accepts unit=mph and requires a final country=FR (or country=unknown).
// No route/speed-derived limit. Missing, invalid or expired records are absent.
std::optional<HudSpeedLimitObservation> parse_hud_speed_limit(std::string_view);
std::optional<HudSpeedLimitObservation> read_hud_speed_limit(const std::string& path,
                                                             Clock::time_point now);

struct NativeHudContent {
  VehicleSample vehicle;
  // Obtain from RouteGuidance::snapshot(now) in this live session. Its
  // change-driven freshness/clear policy remains owned by RouteGuidance.
  NavigationSnapshot navigation;
  std::optional<HudSpeedLimitObservation> speed_limit;
  std::string country{};  // Configured fallback; fresh provider country wins.
};
struct NativeHudModel {
  std::optional<double> speed_kmh;
  Clock::time_point speed_sampled_at{};  // Original legacy acquisition date.
  std::optional<VehicleSignalTiming> speed_timing;
  bool simulated = false;
  NavigationSnapshot navigation;
  std::optional<HudSpeedLimitObservation> speed_limit;
  HudRoadLocale locale;
};
NativeHudModel native_hud_model(const NativeHudContent&, Clock::time_point);
// Opaque black ARGB8888, native-endian, caller-owned buffer. No file I/O.
void draw_native_hud(const NativeHudContent&, Clock::time_point, std::uint32_t* pixels,
                     unsigned width, unsigned height, std::size_t stride_bytes,
                     const HudCalibration& = {});
Image render_native_hud(const NativeHudContent&, Clock::time_point, unsigned width = 960,
                        unsigned height = 360, const HudCalibration& = {});
// Static measured-optics setup image; no vehicle/navigation claims.
Image render_hud_calibration_grid(unsigned width, unsigned height, const HudCalibration& = {});
}  // namespace headunit
