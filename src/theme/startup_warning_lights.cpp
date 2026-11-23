// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/startup_warning_lights.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <utility>

namespace headunit::theme {
namespace {
constexpr std::size_t max_file_bytes = 16;

struct File {
  int fd = -1;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
};
}  // namespace

std::optional<bool> parse_startup_warning_lights(std::string_view content) {
  if (content.ends_with('\n')) content.remove_suffix(1);
  if (content == "hide") return true;
  if (content == "show") return false;
  return std::nullopt;
}

StartupWarningLightsFile::StartupWarningLightsFile(std::string path) : path_(std::move(path)) {
  if (path_.empty() || path_.front() != '/' || path_.size() > 4096)
    throw std::invalid_argument("startup warning lights file path must be absolute");
}

std::optional<bool> StartupWarningLightsFile::poll(Clock::time_point now) {
  if (now < next_poll_) return std::nullopt;
  next_poll_ = now + poll_interval;
  File input{::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (input.fd < 0) return std::nullopt;
  struct stat info{};
  // Same private ownership rule as the cluster selection: the display account
  // owns a 0600 regular file written atomically by the phone manager.
  if (fstat(input.fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
      (info.st_mode & 0077) || info.st_nlink != 1 || info.st_size <= 0 ||
      std::size_t(info.st_size) > max_file_bytes)
    return std::nullopt;
  // Recycled inodes and coarse filesystem timestamps can give different
  // same-length choices identical metadata. Read at most 17 bytes each poll;
  // rate limiting and semantic equality bound the work without that cache.
  char bytes[max_file_bytes + 1]{};
  const auto count = ::read(input.fd, bytes, sizeof(bytes));
  if (count != info.st_size) return std::nullopt;
  const auto mode = parse_startup_warning_lights({bytes, std::size_t(count)});
  if (!mode || mode == current_) return std::nullopt;
  current_ = mode;
  return mode;
}
}  // namespace headunit::theme
