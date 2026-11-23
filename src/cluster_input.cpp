// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/cluster_input.hpp>
#include <charconv>
#include <filesystem>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit {
namespace {
void check(bool value) {
  if (!value) throw std::invalid_argument("invalid private cluster input request");
}
struct File {
  int fd = -1;
  ~File() {
    if (fd >= 0) close(fd);
  }
};
bool identity(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
std::uint64_t decimal(const std::string& value) {
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  check(!value.empty() && value.front() != '0' && value.size() <= 20 && parsed.ec == std::errc{} &&
        parsed.ptr == value.data() + value.size());
  return result;
}
std::string read_request(const std::string& requested) {
  const std::filesystem::path path(requested);
  File directory{open(path.parent_path().c_str(),
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
  struct stat parent{}, named_parent{};
  check(directory.fd >= 0 && !fstat(directory.fd, &parent) && S_ISDIR(parent.st_mode) &&
        parent.st_uid == getuid() && (parent.st_mode & 07777) == 0700 &&
        !lstat(path.parent_path().c_str(), &named_parent) && identity(parent, named_parent));
  File file{openat(directory.fd, path.filename().c_str(),
                   O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)};
  if (file.fd < 0) return {};
  struct stat before{}, after{}, named{};
  check(!fstat(file.fd, &before) && S_ISREG(before.st_mode) && before.st_uid == getuid() &&
        (before.st_mode & 07777) == 0600 && before.st_nlink == 1 && before.st_size > 0 &&
        before.st_size <= 192);
  char bytes[193]{};
  const auto count = read(file.fd, bytes, sizeof(bytes));
  check(count == before.st_size && !fstat(file.fd, &after) && identity(before, after) &&
        before.st_size == after.st_size && before.st_mode == after.st_mode &&
        before.st_uid == after.st_uid && before.st_nlink == after.st_nlink &&
        !fstatat(directory.fd, path.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) &&
        identity(before, named) && !lstat(path.parent_path().c_str(), &named_parent) &&
        identity(parent, named_parent));
#ifdef __APPLE__
  check(before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
        before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
        before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
        before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec);
#else
  check(before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
        before.st_ctim.tv_nsec == after.st_ctim.tv_nsec);
#endif
  return {bytes, static_cast<std::size_t>(count)};
}
}  // namespace
ClusterInputControl::ClusterInputControl(std::string path, std::string nonce,
                                         std::uint64_t generation)
    : path_(std::move(path)), nonce_(std::move(nonce)), generation_(generation) {
  check(std::filesystem::path(path_).is_absolute() && path_.size() <= 4000 &&
        !std::filesystem::path(path_).filename().empty() && path_.find('\0') == path_.npos &&
        nonce_.size() == 32 && nonce_.find_first_not_of("0123456789abcdef") == nonce_.npos &&
        generation_ > 0);
  for (const auto& component : std::filesystem::path(path_))
    check(component != "." && component != "..");
}
void ClusterInputControl::poll(bool ready, const Sender& send, Clock::time_point now,
                               const Opener& open) {
  const bool was_pressed = pressed_;
  if (pressed_ && (now >= release_at_ || !ready)) {
    if (send(pressed_target_, 0)) pressed_ = false;
  }
  if (now < next_read_) return;
  next_read_ = now + std::chrono::milliseconds(100);
  try {
    const auto bytes = read_request(path_);
    if (bytes.empty()) return;
    std::istringstream input(bytes);
    std::string version, nonce, generation, sequence, acquired, action, extra;
    check(bool(std::getline(input, version)) && bool(std::getline(input, nonce)) &&
          bool(std::getline(input, generation)) && bool(std::getline(input, sequence)) &&
          bool(std::getline(input, acquired)) && bool(std::getline(input, action)) &&
          !std::getline(input, extra) && bytes.back() == '\n' && version == "HUI1");
    if (nonce != nonce_ || decimal(generation) != generation_) return;
    const auto number = decimal(sequence);
    if (number <= watermark_) return;
    watermark_ = number;
    const auto sampled = decimal(acquired);
    const auto observed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    if (!ready || was_pressed || pressed_ || observed <= 0 || sampled > std::uint64_t(observed) ||
        std::uint64_t(observed) - sampled > 2000000000ULL)
      return;
    // App shortcuts are one-shot requests, not held keys.
    const char* url = action == "open_maps"    ? "maps:"
                      : action == "open_music" ? "music:"
                      : action == "open_phone" ? "mobilephone:"
                                               : nullptr;
    if (url) {
      if (open) (void)open(url);
      return;
    }
    std::uint8_t report = 0;
    auto target = HidKeyTarget::cluster;
    if (action == "play_pause" || action == "next_track" || action == "previous_track") {
      target = HidKeyTarget::media;
      report = std::uint8_t(action == "play_pause"   ? MediaButton::play_pause
                            : action == "next_track" ? MediaButton::next_track
                                                     : MediaButton::previous_track);
    } else if (action == "next")
      report = std::uint8_t(ClusterButton::next);
    else if (action == "previous")
      report = std::uint8_t(ClusterButton::previous);
    else if (action == "select")
      report = std::uint8_t(ClusterButton::select);
    else if (action == "back")
      report = std::uint8_t(ClusterButton::back);
    else if (action == "up")
      report = std::uint8_t(ClusterButton::up);
    else if (action == "down")
      report = std::uint8_t(ClusterButton::down);
    else
      return;
    if (send(target, report)) {
      pressed_ = true;
      pressed_target_ = target;
      release_at_ = now + std::chrono::milliseconds(100);
    }
  } catch (const std::exception&) {
    // Bad or replaced requests cannot interrupt the current connection or
    // prevent a previously accepted key from being released.
  }
}
}  // namespace headunit
