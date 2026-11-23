// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster_configuration.hpp>
#include <charconv>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit {
bool valid_native_cluster_configuration(const NativeClusterConfiguration& config) noexcept {
  return (config.temperature_precision == NativeTemperaturePrecision::whole ||
          config.temperature_precision == NativeTemperaturePrecision::one_decimal) &&
         (config.pressure_unit == NativePressureUnit::kpa ||
          config.pressure_unit == NativePressureUnit::bar ||
          config.pressure_unit == NativePressureUnit::psi) &&
         (config.speed_precision == NativeSpeedPrecision::whole ||
          config.speed_precision == NativeSpeedPrecision::one_decimal) &&
         (config.fuel_consumption_unit == NativeFuelConsumptionUnit::liters_per_100km ||
          config.fuel_consumption_unit == NativeFuelConsumptionUnit::us_mpg ||
          config.fuel_consumption_unit == NativeFuelConsumptionUnit::imperial_mpg) &&
         (config.temperature_unit == NativeTemperatureUnit::celsius ||
          config.temperature_unit == NativeTemperatureUnit::fahrenheit) &&
         (config.distance_unit == NativeDistanceUnit::kilometers ||
          config.distance_unit == NativeDistanceUnit::miles) &&
         std::isfinite(config.speed_maximum_kmh) && config.speed_maximum_kmh >= 10 &&
         config.speed_maximum_kmh <= 800 && std::isfinite(config.rpm_maximum) &&
         config.rpm_maximum >= 100 && config.rpm_maximum <= 30000 &&
         std::isfinite(config.rpm_redline) && config.rpm_redline >= 0 &&
         config.rpm_redline <= config.rpm_maximum;
}
NativeClusterConfiguration parse_native_cluster_configuration(std::string_view text) {
  if (text.empty() || text.size() > 4096 || text.find_first_of("\r\0", 0, 2) != text.npos)
    throw std::invalid_argument("invalid native cluster configuration size or characters");
  std::map<std::string, std::string> fields;
  while (!text.empty()) {
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text = end == text.npos ? std::string_view{} : text.substr(end + 1);
    if (line.empty()) continue;
    const auto equal = line.find('=');
    if (equal == line.npos || equal == 0 || equal + 1 == line.size() ||
        !fields.emplace(line.substr(0, equal), line.substr(equal + 1)).second)
      throw std::invalid_argument("invalid or duplicate native cluster field");
    if (fields.size() > 13) throw std::invalid_argument("too many native cluster fields");
  }
  auto take = [&](const char* key) {
    const auto found = fields.find(key);
    if (found == fields.end()) throw std::invalid_argument("missing native cluster field");
    const auto value = found->second;
    fields.erase(found);
    return value;
  };
  if (take("version") != "1") throw std::invalid_argument("unknown native cluster version");
  const auto unit = take("speed_unit");
  if (unit != "kmh" && unit != "mph") throw std::invalid_argument("unknown native speed unit");
  auto real = [&](const char* key) {
    const auto value = take(key);
    double number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(number))
      throw std::invalid_argument("invalid native cluster number");
    return number;
  };
  NativeClusterConfiguration config;
  config.miles_per_hour = unit == "mph";
  if (const auto found = fields.find("speed_precision"); found != fields.end()) {
    if (found->second == "whole")
      config.speed_precision = NativeSpeedPrecision::whole;
    else if (found->second == "one_decimal")
      config.speed_precision = NativeSpeedPrecision::one_decimal;
    else
      throw std::invalid_argument("unknown native speed precision");
    fields.erase(found);
  }
  config.speed_maximum_kmh = real("speed_maximum_kmh");
  config.rpm_maximum = real("rpm_maximum");
  config.rpm_redline = real("rpm_redline");
  const auto shown = take("show_rpm");
  if (shown != "true" && shown != "false")
    throw std::invalid_argument("invalid native RPM visibility");
  config.show_rpm = shown == "true";
  if (const auto found = fields.find("temperature_unit"); found != fields.end()) {
    if (found->second == "celsius")
      config.temperature_unit = NativeTemperatureUnit::celsius;
    else if (found->second == "fahrenheit")
      config.temperature_unit = NativeTemperatureUnit::fahrenheit;
    else
      throw std::invalid_argument("unknown native temperature unit");
    fields.erase(found);
  }
  if (const auto found = fields.find("distance_unit"); found != fields.end()) {
    if (found->second == "kilometers")
      config.distance_unit = NativeDistanceUnit::kilometers;
    else if (found->second == "miles")
      config.distance_unit = NativeDistanceUnit::miles;
    else
      throw std::invalid_argument("unknown native distance unit");
    fields.erase(found);
  }
  if (const auto found = fields.find("fuel_consumption_unit"); found != fields.end()) {
    if (found->second == "liters_per_100km")
      config.fuel_consumption_unit = NativeFuelConsumptionUnit::liters_per_100km;
    else if (found->second == "us_mpg")
      config.fuel_consumption_unit = NativeFuelConsumptionUnit::us_mpg;
    else if (found->second == "imperial_mpg")
      config.fuel_consumption_unit = NativeFuelConsumptionUnit::imperial_mpg;
    else
      throw std::invalid_argument("unknown native fuel consumption unit");
    fields.erase(found);
  }
  if (const auto found = fields.find("pressure_unit"); found != fields.end()) {
    if (found->second == "kpa")
      config.pressure_unit = NativePressureUnit::kpa;
    else if (found->second == "bar")
      config.pressure_unit = NativePressureUnit::bar;
    else if (found->second == "psi")
      config.pressure_unit = NativePressureUnit::psi;
    else
      throw std::invalid_argument("unknown native pressure unit");
    fields.erase(found);
  }
  if (const auto found = fields.find("temperature_precision"); found != fields.end()) {
    if (found->second == "whole")
      config.temperature_precision = NativeTemperaturePrecision::whole;
    else if (found->second == "one_decimal")
      config.temperature_precision = NativeTemperaturePrecision::one_decimal;
    else
      throw std::invalid_argument("unknown native temperature precision");
    fields.erase(found);
  }
  if (const auto found = fields.find("drive_mode_profiles_file"); found != fields.end()) {
    config.drive_mode_profiles_file = found->second;
    fields.erase(found);
    if (!std::filesystem::path(config.drive_mode_profiles_file).is_absolute() ||
        config.drive_mode_profiles_file.size() > 4000)
      throw std::invalid_argument("invalid drive mode profiles path");
  }
  if (!fields.empty() || !valid_native_cluster_configuration(config))
    throw std::invalid_argument("invalid native cluster configuration");
  return config;
}
NativeClusterConfiguration load_native_cluster_configuration(const std::string& path) {
  // Configuration is read before presentation begins. A pipe or device must
  // not hold the appliance's only native renderer in an unbounded startup read.
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) throw std::runtime_error("cannot open native cluster configuration");
  struct Close {
    int descriptor;
    ~Close() { close(descriptor); }
  } owner{fd};
  struct stat before{}, after{}, named{};
  if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
      before.st_size > 4096)
    throw std::invalid_argument("native cluster configuration must be a bounded regular file");
  std::string text(std::size_t(before.st_size), '\0');
  std::size_t offset = 0;
  unsigned interrupted = 0;
  while (offset < text.size()) {
    const auto count = read(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR && ++interrupted <= 3) continue;
    if (count <= 0) throw std::runtime_error("cannot read native cluster configuration");
    offset += std::size_t(count);
  }
  const auto same = [](const struct stat& a, const struct stat& b) {
    const bool identity = a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
#ifdef __APPLE__
    return identity && a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
           a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
           a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
           a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
    return identity && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
  };
  if (fstat(fd, &after) != 0 || lstat(path.c_str(), &named) != 0 || !S_ISREG(named.st_mode) ||
      !same(before, after) || !same(after, named))
    throw std::runtime_error("native cluster configuration changed during acquisition");
  return parse_native_cluster_configuration(text);
}
}  // namespace headunit
