// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <string>
#include <string_view>

namespace headunit {
enum class NativeTemperaturePrecision { whole, one_decimal };
enum class NativePressureUnit { kpa, bar, psi };
enum class NativeSpeedPrecision { whole, one_decimal };
enum class NativeFuelConsumptionUnit { liters_per_100km, us_mpg, imperial_mpg };
enum class NativeDistanceUnit { kilometers, miles };
// Legacy bench defaults preserve existing callers. A configured vehicle must
// declare its gauge parameters explicitly; these are not acquired readings.
enum class NativeTemperatureUnit { celsius, fahrenheit };
struct NativeClusterConfiguration {
  bool miles_per_hour = false;
  double speed_maximum_kmh = 260;
  double rpm_maximum = 8000;
  double rpm_redline = 6500;
  bool show_rpm = true;
  std::string drive_mode_profiles_file{};
  NativeTemperatureUnit temperature_unit = NativeTemperatureUnit::celsius;
  NativeDistanceUnit distance_unit = NativeDistanceUnit::kilometers;
  NativeFuelConsumptionUnit fuel_consumption_unit = NativeFuelConsumptionUnit::liters_per_100km;
  NativeSpeedPrecision speed_precision = NativeSpeedPrecision::whole;
  NativeTemperaturePrecision temperature_precision = NativeTemperaturePrecision::whole;
  NativePressureUnit pressure_unit = NativePressureUnit::kpa;
};
bool valid_native_cluster_configuration(const NativeClusterConfiguration&) noexcept;
NativeClusterConfiguration parse_native_cluster_configuration(std::string_view);
NativeClusterConfiguration load_native_cluster_configuration(const std::string& path);
}  // namespace headunit
