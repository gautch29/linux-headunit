// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_control.hpp>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit::camera {
namespace {
struct File {
  int fd = -1;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
};
bool private_directory(const struct stat& info) {
  return S_ISDIR(info.st_mode) && info.st_uid == getuid() && (info.st_mode & 07777) == 0700;
}
// Never a hard-link alias. A concurrent rename unlinks the replaced inode
// before the pathname resolves to its successor, so a reader may see nlink 0.
bool private_request(const struct stat& info) {
  return S_ISREG(info.st_mode) && info.st_uid == getuid() && (info.st_mode & 07777) == 0600 &&
         info.st_nlink <= 1 && info.st_size >= 0 &&
         info.st_size <= off_t(maximum_activation_request_bytes);
}
bool same_file(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool same_date(const struct stat& a, const struct stat& b) {
#ifdef __APPLE__
  return a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
         a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
         a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
         a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
  return a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
}
ActivationRequest requested(std::string_view text, std::uint64_t now) {
  if (text == "on\n") return ActivationRequest::on;
  if (text == "off\n") return ActivationRequest::off;
  if (!text.starts_with("on ") || !text.ends_with('\n')) return ActivationRequest::unavailable;
  const auto number = text.substr(3, text.size() - 4);
  if (number.empty() || number.front() < '1' || number.front() > '9')
    return ActivationRequest::unavailable;
  for (const char digit : number)
    if (digit < '0' || digit > '9') return ActivationRequest::unavailable;
  std::uint64_t deadline = 0;
  const auto result = std::from_chars(number.data(), number.data() + number.size(), deadline);
  if (result.ec != std::errc{} || result.ptr != number.data() + number.size())
    return ActivationRequest::unavailable;
  if (deadline <= now) return ActivationRequest::off;
  return deadline - now <= 1'000'000'000 ? ActivationRequest::on : ActivationRequest::unavailable;
}
}  // namespace
struct ActivationControl::Impl {
  std::string parent, name;
  File directory;
  struct stat identity{};
  mutable std::mutex mutex;
  mutable std::uint64_t last_now = 0;
  mutable bool retired = false;

  explicit Impl(const std::string& path) {
    const std::filesystem::path requested(path);
    if (path.empty() || path.size() > 4000 || path.find('\0') != path.npos ||
        !requested.is_absolute())
      throw std::invalid_argument("camera control path must be a bounded absolute path");
    for (const auto& component : requested)
      if (component == "." || component == "..")
        throw std::invalid_argument("camera control path must not traverse dot components");
    parent = requested.parent_path().string();
    name = requested.filename().string();
    if (name.empty()) throw std::invalid_argument("camera control path must name a request file");
    struct stat observed{};
    if (::lstat(parent.c_str(), &observed) || !private_directory(observed))
      throw std::invalid_argument("camera control parent must be a real owned 0700 directory");
    directory.fd =
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (directory.fd < 0 || ::fstat(directory.fd, &identity) || !private_directory(identity) ||
        !same_file(identity, observed))
      throw std::invalid_argument("camera control parent could not be opened safely");
  }
  ActivationRequest read(std::optional<std::uint64_t> supplied_now) const noexcept {
    std::lock_guard lock(mutex);
    auto now =
        supplied_now.value_or(std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count()));
    if (!now || now < last_now) retired = true;
    if (retired) return ActivationRequest::unavailable;
    last_now = now;
    const auto parent_ok = [&] {
      struct stat named{}, opened{};
      return !::lstat(parent.c_str(), &named) && private_directory(named) &&
             same_file(identity, named) && !::fstat(directory.fd, &opened) &&
             private_directory(opened) && same_file(identity, opened);
    };
    const auto replacement = [&](const struct stat& previous) -> std::optional<ActivationRequest> {
      struct stat current{};
      // Only an independently validated safe new inode, or the same inode just
      // unlinked by a replacing rename, justifies retrying. Missing/unsafe
      // input, parent loss or same-inode mutation stays unknown.
      if (parent_ok() && !::fstatat(directory.fd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) &&
          private_request(current) &&
          (!same_file(previous, current) || (previous.st_nlink && !current.st_nlink)))
        return std::nullopt;
      return ActivationRequest::unavailable;
    };
    const auto attempt = [&]() -> std::optional<ActivationRequest> {
      if (!parent_ok()) return ActivationRequest::unavailable;
      // Inspect before open; never open a known device/FIFO/redirected request.
      struct stat observed{};
      if (::fstatat(directory.fd, name.c_str(), &observed, AT_SYMLINK_NOFOLLOW) ||
          !private_request(observed))
        return ActivationRequest::unavailable;
      File request{
          ::openat(directory.fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
      struct stat opened{};
      if (request.fd < 0 || ::fstat(request.fd, &opened) || !private_request(opened) ||
          !same_file(observed, opened))
        return replacement(observed);
      char bytes[maximum_activation_request_bytes + 1]{};
      std::size_t count = 0;
      unsigned interrupted = 0;
      while (count < sizeof(bytes)) {
        const auto received = ::read(request.fd, bytes + count, sizeof(bytes) - count);
        if (received < 0 && errno == EINTR && ++interrupted <= 3) continue;
        if (received < 0) return ActivationRequest::unavailable;
        if (received == 0) break;
        count += std::size_t(received);
      }
      struct stat after{}, current{};
      if (::fstat(request.fd, &after) || !private_request(after) || !same_file(opened, after) ||
          count != std::size_t(opened.st_size) || after.st_size != opened.st_size ||
          !same_date(opened, after))
        return replacement(opened);
      if (!parent_ok()) return ActivationRequest::unavailable;
      if (::fstatat(directory.fd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) ||
          !private_request(current))
        return ActivationRequest::unavailable;
      if (!same_file(after, current) || after.st_size != current.st_size ||
          !same_date(after, current))
        return replacement(after);
      if (!supplied_now) {
        now = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
        if (!now || now < last_now) {
          retired = true;
          return ActivationRequest::unavailable;
        }
        last_now = now;
      }
      return requested(std::string_view(bytes, count), now);
    };
    for (unsigned retry = 0; retry < 4; ++retry)
      if (const auto result = attempt()) return *result;
    return ActivationRequest::unavailable;
  }
};
ActivationControl::ActivationControl(std::string path) : impl_(std::make_unique<Impl>(path)) {}
ActivationControl::~ActivationControl() = default;
bool ActivationControl::poll() const noexcept {
  return impl_->read(std::nullopt) == ActivationRequest::on;
}
bool ActivationControl::poll(std::uint64_t now) const noexcept {
  return read(now) == ActivationRequest::on;
}
ActivationRequest ActivationControl::read(std::uint64_t now) const noexcept {
  return impl_->read(now);
}
ActivationRequest ActivationControl::read() const noexcept {
  return impl_->read(std::nullopt);
}
}  // namespace headunit::camera
