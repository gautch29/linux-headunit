// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_hud.hpp>
#include <array>
#include <charconv>
#include <cmath>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace headunit {
namespace {
constexpr auto maximum_age = std::chrono::hours(24);
bool source_identifier(std::string_view value) {
  if (value.empty() || value.size() > 64) return false;
  for (unsigned char c : value)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-' || c == '.'))
      return false;
  return true;
}
template <class T>
bool number(std::string_view text, T& value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return !text.empty() && error == std::errc{} && end == text.data() + text.size();
}
}  // namespace
bool fresh(const HudSpeedLimitObservation& value, Clock::time_point now) noexcept {
  const auto& t = value.timing;
  return std::isfinite(value.kmh) && value.kmh >= 1 && value.kmh <= 300 &&
         source_identifier(value.source) && valid_hud_country(value.country) &&
         (value.unit == HudSpeedUnit::kilometres_per_hour ||
          value.unit == HudSpeedUnit::miles_per_hour) &&
         t.availability == VehicleAvailability::available &&
         (t.origin == VehicleObservationOrigin::measured ||
          t.origin == VehicleObservationOrigin::calculated ||
          t.origin == VehicleObservationOrigin::simulation) &&
         t.acquired_at && *t.acquired_at >= Clock::time_point{} &&
         t.maximum_age > Clock::duration::zero() && t.maximum_age <= maximum_age &&
         now >= *t.acquired_at && now - *t.acquired_at < t.maximum_age;
}
std::optional<HudSpeedLimitObservation> parse_hud_speed_limit(std::string_view record) {
  if (record.empty() || record.size() > 512) return {};
  if (record.back() == '\n') record.remove_suffix(1);
  if (record.empty()) return {};
  for (unsigned char c : record)
    if (c < 32 || c > 126) return {};
  const bool regional = record.starts_with("hud-speed-limit-v2 ");
  const std::size_t count = regional ? 8 : 7;
  std::array<std::string_view, 8> fields{};
  for (std::size_t i = 0; i < count; ++i) {
    const auto end = record.find(' ');
    fields[i] = record.substr(0, end);
    if (i + 1 == count) {
      if (end != record.npos) return {};
    } else {
      if (end == record.npos) return {};
      record.remove_prefix(end + 1);
    }
  }
  if (fields[0] != (regional ? "hud-speed-limit-v2" : "hud-speed-limit-v1")) return {};
  constexpr std::array<std::string_view, 7> keys{
      "value=", "unit=", "acquired_ns=", "maximum_age_ns=", "origin=", "source=", "country="};
  for (std::size_t i = 0; i < count - 1; ++i) {
    if (!fields[i + 1].starts_with(keys[i])) return {};
    fields[i + 1].remove_prefix(keys[i].size());
  }
  HudSpeedLimitObservation value;
  if (regional) {
    if (fields[7] != "unknown") value.country = fields[7];
    if ((fields[7] != "unknown" && value.country.empty()) || !valid_hud_country(value.country))
      return {};
  }
  if (fields[2] == "mph" && regional)
    value.unit = HudSpeedUnit::miles_per_hour;
  else if (fields[2] != "km/h")
    return {};
  std::uint64_t acquired = 0, age = 0;
  if (!number(fields[1], value.kmh) || !std::isfinite(value.kmh) || value.kmh < 1 ||
      value.kmh > 300 || !number(fields[3], acquired) || !number(fields[4], age) ||
      acquired > std::uint64_t(std::numeric_limits<std::int64_t>::max()) || age == 0 ||
      age > std::uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(maximum_age).count()) ||
      !source_identifier(fields[6]))
    return {};
  if (value.unit == HudSpeedUnit::miles_per_hour) value.kmh *= 1.609344;
  if (value.kmh > 300) return {};
  if (fields[5] == "measured")
    value.timing.origin = VehicleObservationOrigin::measured;
  else if (fields[5] == "calculated")
    value.timing.origin = VehicleObservationOrigin::calculated;
  else if (fields[5] == "simulation")
    value.timing.origin = VehicleObservationOrigin::simulation;
  else
    return {};
  value.timing.acquired_at = Clock::time_point{
      std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(acquired))};
  value.timing.maximum_age =
      std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(age));
  value.timing.availability = VehicleAvailability::available;
  value.source = fields[6];
  return value;
}
std::optional<HudSpeedLimitObservation> read_hud_speed_limit(const std::string& path,
                                                             Clock::time_point now) {
  if (path.empty() || path.size() > 4096) return {};
  const std::filesystem::path file(path), parent = file.parent_path();
  std::error_code error;
  if (!file.is_absolute() || std::filesystem::canonical(file, error) != file || error) return {};
  struct stat directory{};
  if (lstat(parent.c_str(), &directory) || !S_ISDIR(directory.st_mode) ||
      directory.st_uid != geteuid() || (directory.st_mode & 07777) != 0700)
    return {};
  const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return {};
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } close_fd{fd};
  struct stat before{}, after{}, parent_after{};
  if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
      (before.st_mode & 07777) != 0600 || before.st_nlink != 1 || before.st_size <= 0 ||
      before.st_size > 512)
    return {};
  std::array<char, 512> bytes{};
  std::size_t offset = 0;
  unsigned interrupted = 0;
  while (offset < std::size_t(before.st_size)) {
    const auto count = read(fd, bytes.data() + offset, std::size_t(before.st_size) - offset);
    if (count < 0 && errno == EINTR && ++interrupted <= 3) continue;
    if (count <= 0) return {};
    offset += std::size_t(count);
  }
  if (fstat(fd, &after) || lstat(parent.c_str(), &parent_after) || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size || !S_ISREG(after.st_mode) ||
      after.st_uid != before.st_uid || after.st_mode != before.st_mode || after.st_nlink > 1 ||
      directory.st_dev != parent_after.st_dev || directory.st_ino != parent_after.st_ino ||
      directory.st_uid != parent_after.st_uid || directory.st_mode != parent_after.st_mode)
    return {};
#ifdef __APPLE__
  if (before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
      before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
      before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
      before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec)
    return {};
#else
  if (before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
      before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    return {};
#endif
  // Atomic unlink or any in-place change during acquisition may change ctime.
  // Reject that whole observation rather than splice value and acquisition date.
  // The next poll may pin the new complete inode; never reopen mid-observation.
  auto value = parse_hud_speed_limit({bytes.data(), offset});
  return value && fresh(*value, now) ? value : std::nullopt;
}
}  // namespace headunit
