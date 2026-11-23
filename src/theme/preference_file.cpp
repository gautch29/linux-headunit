// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/preference_file.hpp>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit::theme::script {
namespace {
struct File {
  int fd = -1;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
};
bool private_directory(const struct stat& value) {
  return S_ISDIR(value.st_mode) && value.st_uid == getuid() && !(value.st_mode & 0022);
}
bool regular(const struct stat& value, std::size_t maximum_bytes) {
  return S_ISREG(value.st_mode) && value.st_uid == getuid() && !(value.st_mode & 0077) &&
         value.st_nlink == 1 && value.st_size >= 0 && std::uint64_t(value.st_size) <= maximum_bytes;
}
bool same(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool unchanged(const struct stat& a, const struct stat& b) {
#ifdef __APPLE__
  const auto modified_a = a.st_mtimespec, modified_b = b.st_mtimespec;
  const auto changed_a = a.st_ctimespec, changed_b = b.st_ctimespec;
#else
  const auto modified_a = a.st_mtim, modified_b = b.st_mtim;
  const auto changed_a = a.st_ctim, changed_b = b.st_ctim;
#endif
  return same(a, b) && a.st_size == b.st_size && modified_a.tv_sec == modified_b.tv_sec &&
         modified_a.tv_nsec == modified_b.tv_nsec && changed_a.tv_sec == changed_b.tv_sec &&
         changed_a.tv_nsec == changed_b.tv_nsec;
}
}  // namespace
PreferenceFile::PreferenceFile(std::string path, std::size_t maximum_bytes)
    : path_(std::move(path)), maximum_bytes_(maximum_bytes) {
  if (path_.size() > 4000 || !std::filesystem::path(path_).is_absolute() ||
      path_.find('\0') != path_.npos)
    throw std::invalid_argument("invalid preference path");
  if (!maximum_bytes_ || maximum_bytes_ > 4096)
    throw std::invalid_argument("invalid preference bound");
  worker_ = std::thread([this] { run(); });
}
PreferenceFile::~PreferenceFile() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_one();
  worker_.join();
}
PreferenceFile::Snapshot PreferenceFile::snapshot() const {
  std::lock_guard lock(mutex_);
  return state_;
}
std::uint64_t PreferenceFile::save(std::string record) {
  if (record.empty() || record.size() > maximum_bytes_)
    throw std::invalid_argument("preference exceeds bound");
  std::lock_guard lock(mutex_);
  if (stopping_ || sequence_ == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("preference owner retired");
  pending_ = std::pair{++sequence_, std::move(record)};
  wake_.notify_one();
  return sequence_;
}
void PreferenceFile::wait() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [&] { return state_.loaded && !busy_ && !pending_; });
}
void PreferenceFile::run() {
  const auto parent = std::filesystem::path(path_).parent_path().string();
  const auto name = std::filesystem::path(path_).filename().string();
  File directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  struct stat pinned{};
  const auto valid_parent = [&] {
    struct stat opened{}, named{};
    return directory.fd >= 0 && !::fstat(directory.fd, &opened) &&
           !::lstat(parent.c_str(), &named) && private_directory(opened) &&
           private_directory(named) && same(opened, pinned) && same(named, pinned);
  };
  const bool pinned_parent =
      directory.fd >= 0 && !::fstat(directory.fd, &pinned) && private_directory(pinned);
  Snapshot initial;
  initial.loaded = true;
  if (!pinned_parent || !valid_parent()) {
    initial.error = "Cannot read saved cluster style";
  } else {
    File input{
        ::openat(directory.fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (input.fd < 0) {
      if (errno != ENOENT) initial.error = "Cannot read saved cluster style";
    } else {
      struct stat before{}, after{}, named{};
      std::string bytes(maximum_bytes_ + 1, '\0');
      ssize_t count = -1;
      if (!::fstat(input.fd, &before) && regular(before, maximum_bytes_)) {
        do {
          count = ::read(input.fd, bytes.data(), bytes.size());
        } while (count < 0 && errno == EINTR);
      }
      if (count < 0 || count != before.st_size || ::fstat(input.fd, &after) ||
          !regular(after, maximum_bytes_) || !unchanged(before, after) ||
          ::fstatat(directory.fd, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
          !regular(named, maximum_bytes_) || !unchanged(after, named) || !valid_parent())
        initial.error = "Invalid private saved cluster style";
      else
        initial.data = bytes.substr(0, std::size_t(count));
    }
  }
  {
    std::lock_guard lock(mutex_);
    state_ = std::move(initial);
    busy_ = false;
  }
  idle_.notify_all();
  for (;;) {
    std::pair<std::uint64_t, std::string> job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stopping_ || pending_; });
      if (!pending_) return;
      job = std::move(*pending_);
      pending_.reset();
      busy_ = true;
    }
    bool saved = false;
    std::string temporary;
    if (valid_parent()) {
      File output;
      for (unsigned attempt = 0; attempt < 64; ++attempt) {
        temporary = ".cluster-preference-" + std::to_string(getpid()) + "-" +
                    std::to_string(job.first) + "-" + std::to_string(attempt);
        output.fd = ::openat(directory.fd, temporary.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (output.fd >= 0 || errno != EEXIST) break;
      }
      if (output.fd >= 0) {
        std::size_t at = 0;
        while (at < job.second.size()) {
          const auto count = ::write(output.fd, job.second.data() + at, job.second.size() - at);
          if (count < 0 && errno == EINTR) continue;
          if (count <= 0) break;
          at += std::size_t(count);
        }
        const bool written = at == job.second.size() && !::fsync(output.fd);
        struct stat held{}, named{};
        const bool owned =
            written && !::fstat(output.fd, &held) && regular(held, maximum_bytes_) &&
            held.st_size == ssize_t(job.second.size()) &&
            !::fstatat(directory.fd, temporary.c_str(), &named, AT_SYMLINK_NOFOLLOW) &&
            unchanged(held, named);
        const bool renamed =
            owned && valid_parent() &&
            !::renameat(directory.fd, temporary.c_str(), directory.fd, name.c_str());
        saved = renamed && !::fsync(directory.fd);
        // Rename frees this temporary name for another worker immediately.
        // Even a failed directory sync must not unlink its new occupant.
        if (!renamed && !::fstat(output.fd, &held) &&
            !::fstatat(directory.fd, temporary.c_str(), &named, AT_SYMLINK_NOFOLLOW) &&
            same(held, named))
          ::unlinkat(directory.fd, temporary.c_str(), 0);
      }
    }
    {
      std::lock_guard lock(mutex_);
      state_.completed = job.first;
      state_.error = saved ? "" : "Cannot save confirmed cluster style";
      if (saved) state_.data = std::move(job.second);
      busy_ = false;
    }
    idle_.notify_all();
  }
}
}  // namespace headunit::theme::script
