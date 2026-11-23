// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/display_response.hpp>
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

DisplayResponsePolicy display_response_policy(DisplayResponse mode) {
  switch (mode) {
    case DisplayResponse::smooth:
      return {0, 2};
    case DisplayResponse::balanced:
      return {50000000, 2};
    case DisplayResponse::fast:
      return {80000000, 1};
  }
  throw std::invalid_argument("invalid display response mode");
}

std::string_view display_response_name(DisplayResponse mode) {
  switch (mode) {
    case DisplayResponse::smooth:
      return "smooth";
    case DisplayResponse::balanced:
      return "balanced";
    case DisplayResponse::fast:
      return "fast";
  }
  throw std::invalid_argument("invalid display response mode");
}

std::optional<DisplayResponse> parse_display_response(std::string_view content) {
  if (content.ends_with('\n')) content.remove_suffix(1);
  for (const auto mode :
       {DisplayResponse::smooth, DisplayResponse::balanced, DisplayResponse::fast})
    if (content == display_response_name(mode)) return mode;
  return std::nullopt;
}

DisplayResponseFile::DisplayResponseFile(std::string path) : path_(std::move(path)) {
  if (path_.empty() || path_.front() != '/' || path_.size() > 4096)
    throw std::invalid_argument("display response file path must be absolute");
}

std::optional<DisplayResponse> DisplayResponseFile::poll(Clock::time_point now) {
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
  Stamp stamp{std::uint64_t(info.st_dev), std::uint64_t(info.st_ino), std::uint64_t(info.st_size)};
#ifdef __APPLE__
  stamp.seconds = info.st_mtimespec.tv_sec;
  stamp.nanoseconds = info.st_mtimespec.tv_nsec;
#else
  stamp.seconds = info.st_mtim.tv_sec;
  stamp.nanoseconds = info.st_mtim.tv_nsec;
#endif
  if (stamp_ == stamp) return std::nullopt;
  char bytes[max_file_bytes + 1]{};
  const auto count = ::read(input.fd, bytes, sizeof(bytes));
  if (count != info.st_size) return std::nullopt;
  stamp_ = stamp;
  const auto mode = parse_display_response({bytes, std::size_t(count)});
  if (!mode || mode == current_) return std::nullopt;
  current_ = mode;
  return mode;
}
}  // namespace headunit::theme
