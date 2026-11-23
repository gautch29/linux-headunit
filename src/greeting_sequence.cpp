// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/greeting_sequence.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit {
namespace {
struct File {
  int fd;
  ~File() {
    if (fd >= 0) close(fd);
  }
};
bool stable(const struct stat& a, const struct stat& b) {
  bool same = a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
              a.st_uid == b.st_uid && a.st_mode == b.st_mode && a.st_nlink == b.st_nlink;
#ifdef __APPLE__
  return same && a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
         a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
         a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
         a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
  return same && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
}
std::string read(const std::string& path) {
  if (path.empty()) return {};
  const std::filesystem::path name(path);
  std::error_code path_error;
  if (!name.is_absolute() ||
      std::filesystem::canonical(name.parent_path(), path_error) != name.parent_path() ||
      path_error)
    return {};
  File directory{open(name.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
  struct stat parent{};
  if (directory.fd < 0 || fstat(directory.fd, &parent) || parent.st_uid != getuid() ||
      (parent.st_mode & 0777) != 0700)
    return {};
  File file{openat(directory.fd, name.filename().c_str(),
                   O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
  struct stat before{}, after{}, named{};
  if (file.fd < 0 || fstat(file.fd, &before) || !S_ISREG(before.st_mode) ||
      before.st_uid != getuid() || (before.st_mode & 0777) != 0600 || before.st_nlink != 1 ||
      before.st_size <= 0 || before.st_size > 64)
    return {};
  std::array<char, 65> bytes{};
  const auto count = ::read(file.fd, bytes.data(), bytes.size());
  if (count != before.st_size || fstat(file.fd, &after) ||
      fstatat(directory.fd, name.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
      !stable(before, after) || !stable(after, named))
    return {};
  return {bytes.data(), std::size_t(count)};
}
double ease(double value) {
  value = std::clamp(value, 0., 1.);
  return value * value * (3 - 2 * value);
}
std::uint64_t nanos(Clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}
}  // namespace
std::optional<GreetingVariant> parse_greeting_variant(std::string_view text) {
  if (text.ends_with('\n')) text.remove_suffix(1);
  if (text == "off") return GreetingVariant::off;
  if (text == "brand") return GreetingVariant::brand;
  if (text == "minimal") return GreetingVariant::minimal;
  return {};
}
void GreetingSequence::begin(GreetingPhase phase, GreetingVariant variant, Clock::time_point now) {
  phase_ = variant == GreetingVariant::off ? GreetingPhase::inactive : phase;
  variant_ = variant;
  began_ = now;
  last_ = now;
}
void GreetingSequence::retire() {
  phase_ = GreetingPhase::inactive;
}
GreetingFrame GreetingSequence::frame(Clock::time_point now) {
  if (phase_ == GreetingPhase::inactive) return {};
  if (now < last_) {
    retire();
    return {};
  }
  last_ = now;
  const auto elapsed = std::chrono::duration<double>(now - began_).count();
  const double duration = phase_ == GreetingPhase::startup ? 1.8 : .9;
  if (elapsed >= duration) {
    retire();
    return {};
  }
  const double reveal = phase_ == GreetingPhase::startup ? ease(elapsed / .65) : 1.;
  const double opacity = phase_ == GreetingPhase::startup
                             ? ease(elapsed / .12) * (1 - ease((elapsed - 1.1) / .7))
                             : ease(elapsed / .1) * (1 - ease((elapsed - .25) / .65));
  return {variant_, phase_, opacity, reveal};
}
GreetingSequenceFile::GreetingSequenceFile(std::string path, bool boot)
    : path_(std::move(path)), origin_(Clock::now()), boot_(boot) {}
GreetingFrame GreetingSequenceFile::frame(Clock::time_point now) {
  if (now < origin_) {
    sequence_.retire();
    boot_ = false;
    return {};
  }
  if (now >= next_) {
    next_ = now + std::chrono::milliseconds(50);
    const auto variant = parse_greeting_variant(read(path_));
    variant_ = variant.value_or(GreetingVariant::off);
    if (variant_ == GreetingVariant::off) sequence_.retire();
    if (boot_) {
      boot_ = false;
      sequence_.begin(GreetingPhase::startup, variant_, now);
    }
    const auto event = read(path_ + ".event");
    const auto split = event.find(' ');
    if (split != event.npos && event.ends_with('\n')) {
      const std::string_view kind(event.data(), split);
      const auto date = std::string_view(event).substr(split + 1, event.size() - split - 2);
      std::uint64_t acquired = 0;
      const auto [end, error] = std::from_chars(date.data(), date.data() + date.size(), acquired);
      const auto at = nanos(now);
      if (error == std::errc{} && end == date.data() + date.size() && acquired != seen_) {
        // Witness rejected future dates too: a later redraw cannot turn that
        // same invalid request into an admitted event when its date arrives.
        seen_ = acquired;
        if (acquired >= nanos(origin_) && acquired > consumed_ && acquired <= at &&
            at - acquired <= 900000000 && (kind == "startup" || kind == "goodbye")) {
          consumed_ = acquired;
          sequence_.begin(kind == "startup" ? GreetingPhase::startup : GreetingPhase::goodbye,
                          variant_, Clock::time_point(std::chrono::nanoseconds(acquired)));
        }
      }
    }
  }
  return sequence_.frame(now);
}
bool claim_greeting_boot() {
#ifdef CLOCK_BOOTTIME
  timespec elapsed{};
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (!runtime || clock_gettime(CLOCK_BOOTTIME, &elapsed) || elapsed.tv_sec < 0 ||
      (elapsed.tv_sec > 60 || (elapsed.tv_sec == 60 && elapsed.tv_nsec > 0)))
    return false;
  const std::filesystem::path path(runtime);
  std::error_code error;
  if (!path.is_absolute() || std::filesystem::canonical(path, error) != path || error) return false;
  File directory{open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
  struct stat info{};
  if (directory.fd < 0 || fstat(directory.fd, &info) || info.st_uid != getuid() ||
      (info.st_mode & 0777) != 0700)
    return false;
  File marker{openat(directory.fd, "headunit-cluster-greeting-attempt",
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
  return marker.fd >= 0;
#else
  return false;
#endif
}
}  // namespace headunit
