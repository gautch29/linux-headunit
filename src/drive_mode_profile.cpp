// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/drive_mode_profile.hpp>
#include <charconv>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

namespace headunit {
namespace {
bool token(std::string_view value, std::size_t maximum = 64) {
  return !value.empty() && value.size() <= maximum &&
         value.find_first_not_of(
             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:-") == value.npos;
}
std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> fields;
  while (true) {
    const auto end = text.find(separator);
    fields.push_back(text.substr(0, end));
    if (end == text.npos) return fields;
    text.remove_prefix(end + 1);
  }
}
class SnapshotChanged : public std::runtime_error {
 public:
  SnapshotChanged() : std::runtime_error("drive mode profiles changed during read") {}
};
void require(bool valid) {
  if (!valid) throw std::invalid_argument("invalid drive mode profiles");
}
bool uuid(std::string_view value) {
  if (value.empty()) return true;
  if (value.size() != 36) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if ((i == 8 || i == 13 || i == 18 || i == 23)
            ? c != '-'
            : !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F'))
      return false;
  }
  return true;
}
DriveModeProfile profile(const std::vector<std::string_view>& row) {
  require(row.size() == 9 && parse_drive_mode_identifier(row[1]).has_value());
  require(!row[2].empty() && row[2].size() <= 64);
  for (const char c : row[2])
    require(c >= 32 && c <= 126);
  require(row[4] == "minimal" || row[4] == "sport" || row[4] == "gauges" ||
          row[4] == "maps_media" || row[4] == "map" || row[4] == "media" || row[4] == "vehicle" ||
          row[4] == "trip");
  require(uuid(row[3]) && (row[5].empty() || (token(row[5]) && row[5].find(':') == row[5].npos)));
  require(row[6].size() == 6 && row[6].find_first_not_of("0123456789ABCDEF") == row[6].npos);
  require(row[7] == "phone" || row[7] == "black" || row[7] == "blue" || row[7] == "red");
  require(row[8] == "true" || row[8] == "false");
  return {std::string(row[1]), std::string(row[2]), std::string(row[3]), std::string(row[4]),
          std::string(row[5]), std::string(row[6]), std::string(row[7]), row[8] == "true"};
}
std::string read(const std::string& path) {
  const auto parent = std::filesystem::path(path).parent_path();
  struct stat directory{}, named_directory{};
  require(!lstat(parent.c_str(), &directory) && S_ISDIR(directory.st_mode) &&
          directory.st_uid == getuid() && (directory.st_mode & 0777) == 0700 &&
          std::filesystem::canonical(parent) == parent);
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) throw std::invalid_argument("drive mode profiles unavailable");
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } owner{fd};
  struct stat before{}, after{}, named{};
  require(!fstat(fd, &before));
  if (before.st_nlink == 0) throw SnapshotChanged();
  require(S_ISREG(before.st_mode) && before.st_uid == getuid() && (before.st_mode & 0777) == 0600 &&
          before.st_nlink == 1 && before.st_size > 0 && before.st_size <= 32768);
  std::string text(std::size_t(before.st_size), '\0');
  require(::read(fd, text.data(), text.size()) == before.st_size);
  const auto same = [](const struct stat& a, const struct stat& b) {
    const bool identity = a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
                          a.st_mode == b.st_mode && a.st_uid == b.st_uid &&
                          a.st_nlink == b.st_nlink;
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
  if (fstat(fd, &after) || lstat(path.c_str(), &named) || !same(before, after) ||
      !same(before, named))
    throw SnapshotChanged();
  require(!lstat(parent.c_str(), &named_directory) && directory.st_dev == named_directory.st_dev &&
          directory.st_ino == named_directory.st_ino &&
          directory.st_uid == named_directory.st_uid &&
          directory.st_mode == named_directory.st_mode);
  return text;
}
}  // namespace
DriveModeProfiles parse_drive_mode_profiles(std::string_view text) {
  require(!text.empty() && text.size() <= 32768 && text.back() == '\n' &&
          text.find_first_of("\r\0", 0, 2) == text.npos);
  auto lines = split(text.substr(0, text.size() - 1), '\n');
  require(lines.size() >= 3 && lines.size() <= 65);
  const auto header = split(lines[0], '\t');
  require(header.size() == 4 && header[0] == "HUDP1" && token(header[1], 160));
  DriveModeProfiles result;
  result.scope = header[1];
  const auto [end, error] =
      std::from_chars(header[2].data(), header[2].data() + header[2].size(), result.revision);
  require(error == std::errc{} && end == header[2].data() + header[2].size() &&
          result.revision > 0 && header[2].front() != '0');
  if (header[3] != "-") {
    require(parse_drive_mode_identifier(header[3]).has_value());
    result.selected = header[3];
  }
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const auto row = split(lines[i], '\t');
    require(!row.empty() && (row[0] == "mode" || row[0] == "base"));
    auto value = profile(row);
    auto& target = row[0] == "mode" ? result.profiles : result.defaults;
    for (const auto& previous : target)
      require(previous.id != value.id);
    target.push_back(std::move(value));
  }
  require(!result.profiles.empty() && result.profiles.size() <= 32 &&
          result.profiles.size() == result.defaults.size());
  bool selected = result.selected.empty();
  for (std::size_t i = 0; i < result.profiles.size(); ++i) {
    require(result.profiles[i].id == result.defaults[i].id &&
            result.profiles[i].name == result.defaults[i].name);
    selected |= result.profiles[i].id == result.selected;
  }
  require(selected);
  return result;
}
DriveModePresentation resolve_drive_mode(const DriveModeProfiles& profiles,
                                         const VehicleSample& raw, Clock::time_point now) {
  DriveModePresentation result;
  result.revision = profiles.revision;
  const auto sample = current_vehicle_sample(raw, now);
  std::string id;
  const bool simulated_mode =
      sample.observation_timing
          ? sample.observation_timing->drive_mode.origin == VehicleObservationOrigin::simulation
          : sample.simulated;
  if (sample.drive_mode && (!simulated_mode || profiles.selected.empty())) {
    id = drive_mode_identifier_key(*sample.drive_mode);
    result.source = simulated_mode ? "simulation" : "vehicle";
    if (sample.observation_timing) {
      result.timing = sample.observation_timing->drive_mode;
      result.vehicle_generation = sample.observation_timing->generation;
    } else {
      VehicleSignalTiming timing;
      timing.acquired_at = sample.sampled_at;
      timing.maximum_age = std::chrono::milliseconds(500);
      timing.origin = sample.simulated ? VehicleObservationOrigin::simulation
                                       : VehicleObservationOrigin::measured;
      timing.availability = VehicleAvailability::available;
      result.timing = timing;
    }
  } else if (!profiles.selected.empty()) {
    id = profiles.selected;
    result.source = "user";
  }
  for (const auto& entry : profiles.profiles)
    if (entry.id == id) result.profile = entry;
  // A fresh unmapped observation must not fall back to a conflicting user mode.
  if (!id.empty() && !result.profile) result.source = "unmapped";
  return result;
}
DriveModeProfilesFile::DriveModeProfilesFile(std::string path, std::string scope)
    : path_(std::move(path)), scope_(std::move(scope)) {
  require(path_.size() <= 4000 && path_.find('\0') == path_.npos &&
          std::filesystem::path(path_).is_absolute());
}
DriveModePresentation DriveModeProfilesFile::poll(const VehicleSample& sample,
                                                  Clock::time_point now) {
  if (now >= next_read_) {
    next_read_ = now + std::chrono::milliseconds(250);
    try {
      std::string text;
      try {
        text = read(path_);
      } catch (const SnapshotChanged&) {
        text =
            read(path_);  // One bounded retry admits an atomic replacement without a blank frame.
      }
      auto candidate = parse_drive_mode_profiles(text);
      require(scope_.empty() || candidate.scope == scope_);
      scope_ = candidate.scope;
      profiles_ = std::move(candidate);
      error_.clear();
    } catch (const SnapshotChanged& e) {
      // Two concurrent atomic replacements can supersede both opened inodes.
      // Retain the last complete configuration until the next bounded poll;
      // independently acquired vehicle observations still expire normally.
      error_ = e.what();
    } catch (const std::exception& e) {
      profiles_.reset();
      error_ = e.what();
    }
  }
  return profiles_ ? resolve_drive_mode(*profiles_, sample, now) : DriveModePresentation{};
}
void publish_drive_mode_status(const std::string& path, std::string_view scope,
                               const DriveModePresentation& state, std::string_view phase,
                               std::string_view error, Clock::time_point now,
                               std::string_view nonce, std::uint64_t connection_generation) {
  const auto quote = [](std::string_view value) {
    std::string result = "\"";
    for (const char c : value) {
      if (c == '\\' || c == '"') result += '\\';
      if (c >= 32 && c <= 126) result += c;
    }
    return result + "\"";
  };
  const auto ns = [](Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  };
  timespec publication{};
  if (clock_gettime(CLOCK_MONOTONIC, &publication) != 0) return;
  const auto publication_ns = std::int64_t(publication.tv_sec) * 1000000000 + publication.tv_nsec;
  // Record real clock observations, not an offset between different epochs.
  // Acquisition remains in the original C++ steady-clock domain. The age at
  // this conservative (earlier) publication bound permits independent expiry
  // in Python even where libc++ steady_clock uses a different monotonic clock.
  const auto sampled = Clock::now();
  std::ostringstream out;
  out << "{\"version\":1,\"scope\":" << quote(scope) << ",\"revision\":" << state.revision
      << ",\"source\":" << quote(state.source)
      << ",\"mode\":" << (state.profile ? quote(state.profile->id) : "null")
      << ",\"phase\":" << quote(phase) << ",\"error\":" << quote(error.substr(0, 256))
      << ",\"updatedMonotonicNs\":" << publication_ns
      << ",\"vehicleGeneration\":" << state.vehicle_generation << ",\"observation\":";
  if (state.timing && state.timing->acquired_at) {
    const auto& timing = *state.timing;
    const auto origin = timing.origin == VehicleObservationOrigin::simulation   ? "simulation"
                        : timing.origin == VehicleObservationOrigin::calculated ? "calculated"
                                                                                : "measured";
    out << "{\"acquiredSteadyNs\":" << ns(timing.acquired_at->time_since_epoch())
        << ",\"maximumAgeNs\":" << ns(timing.maximum_age)
        << ",\"ageAtPublicationNs\":" << ns(sampled - *timing.acquired_at)
        << ",\"clockDomain\":\"host-steady\",\"origin\":" << quote(origin) << '}';
  } else {
    out << "null";
  }
  out << ",\"nonce\":" << quote(nonce) << ",\"connectionGeneration\":\"" << connection_generation
      << "\""
      << ",\"sportScreenAvailable\":"
      << (state.profile && state.profile->sport_screen_available &&
                  (phase == "native-profile" || phase == "local-profile" || phase == "confirmed" ||
                   phase == "partial")
              ? "true"
              : "false")
      << ",\"signedWallpaperSupported\":false}\n";
  const auto text = out.str();
  const auto parent = std::filesystem::path(path).parent_path();
  struct stat directory{}, named{};
  if (lstat(parent.c_str(), &directory) || !S_ISDIR(directory.st_mode) ||
      directory.st_uid != getuid() || (directory.st_mode & 0777) != 0700)
    return;
  const int folder = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (folder < 0) return;
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } directory_owner{folder};
  if (fstat(folder, &named) || directory.st_dev != named.st_dev || directory.st_ino != named.st_ino)
    return;
  // A pinned directory prevents a replacement parent from redirecting status.
  const auto temporary = ".drive-mode-status-" + std::to_string(getpid()) + "-" +
                         std::to_string(ns(now.time_since_epoch()));
  const int fd =
      openat(folder, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return;
  const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  close(fd);
  const bool same_parent = !lstat(parent.c_str(), &named) && directory.st_dev == named.st_dev &&
                           directory.st_ino == named.st_ino && directory.st_uid == named.st_uid &&
                           directory.st_mode == named.st_mode;
  if (written && same_parent)
    (void)renameat(folder, temporary.c_str(), folder,
                   std::filesystem::path(path).filename().c_str());
  unlinkat(folder, temporary.c_str(), 0);
}
}  // namespace headunit
