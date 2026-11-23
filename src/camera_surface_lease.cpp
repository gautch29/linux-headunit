// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_surface_lease.hpp>
#include <headunit/camera_control.hpp>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
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
bool identity(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool private_file(const struct stat& st) {
  return S_ISREG(st.st_mode) && st.st_uid == getuid() && (st.st_mode & 07777) == 0600 &&
         st.st_nlink == 1 && st.st_size == 0;
}
bool private_parent(const struct stat& st) {
  return S_ISDIR(st.st_mode) && st.st_uid == getuid() && (st.st_mode & 07777) == 0700;
}
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
}  // namespace
struct CameraSurfaceLease::Impl {
  std::string parent, name;
  File directory, file;
  struct stat parent_identity{}, file_identity{};
  int mode = 0;
  explicit Impl(const std::string& path, bool create_missing = true) {
    // Reuse the activation path contract without reading or changing a request.
    (void)ActivationControl(path);
    const std::filesystem::path selected(path);
    parent = selected.parent_path().string();
    name = selected.filename().string();
    struct stat observed{};
    check(::lstat(parent.c_str(), &observed) == 0 && private_parent(observed),
          "invalid camera lease parent");
    directory.fd =
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    check(directory.fd >= 0 && ::fstat(directory.fd, &parent_identity) == 0 &&
              private_parent(parent_identity) && identity(observed, parent_identity),
          "camera lease parent changed");
    // Never open an existing device/FIFO, and never truncate a live lock file.
    const int found = ::fstatat(directory.fd, name.c_str(), &observed, AT_SYMLINK_NOFOLLOW);
    check(found == 0 ? private_file(observed) : errno == ENOENT,
          "invalid existing camera lease file");
    if (found != 0 && !create_missing) {
      // A release preflight must not become a runtime surface owner merely
      // because the first connection has not created its lease yet.
      verify();
      return;
    }
    if (found != 0) {
      file.fd = ::openat(directory.fd, name.c_str(),
                         O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
      if (file.fd < 0 && errno == EEXIST) {
        check(::fstatat(directory.fd, name.c_str(), &observed, AT_SYMLINK_NOFOLLOW) == 0 &&
                  private_file(observed),
              "camera lease creation raced an invalid file");
        file.fd =
            ::openat(directory.fd, name.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
      }
    } else
      file.fd =
          ::openat(directory.fd, name.c_str(),
                   (create_missing ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    check(file.fd >= 0 && ::fstat(file.fd, &file_identity) == 0 && private_file(file_identity),
          "invalid opened camera lease file");
    if (!create_missing)
      check(identity(observed, file_identity), "camera lease changed during validation");
    verify();
  }
  void verify() const {
    struct stat parent_now{}, opened_parent{}, named{}, opened{};
    check(::lstat(parent.c_str(), &parent_now) == 0 && private_parent(parent_now) &&
              identity(parent_now, parent_identity) && ::fstat(directory.fd, &opened_parent) == 0 &&
              private_parent(opened_parent) && identity(opened_parent, parent_identity),
          "camera surface lease parent generation changed");
    if (file.fd < 0) {
      check(::fstatat(directory.fd, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 &&
                errno == ENOENT,
            "camera lease appeared during validation");
      return;
    }
    check(::fstatat(directory.fd, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
              private_file(named) && identity(named, file_identity) &&
              ::fstat(file.fd, &opened) == 0 && private_file(opened) &&
              identity(opened, file_identity),
          "camera surface lease generation changed");
  }
  bool acquire(int wanted) {
    verify();
    check(mode == 0 || mode == wanted, "camera lease mode must be released before changing");
    if (mode) return true;
    if (::flock(file.fd, wanted | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) return false;
      throw std::runtime_error("camera surface lease could not be acquired");
    }
    mode = wanted;
    verify();
    return true;
  }
};
void CameraSurfaceLease::validate(const std::string& path) {
  (void)Impl(path, false);
}
CameraSurfaceLease::CameraSurfaceLease(const std::string& path)
    : impl_(std::make_unique<Impl>(path)) {}
CameraSurfaceLease::~CameraSurfaceLease() {
  release();
}
bool CameraSurfaceLease::acquire_shared() {
  return impl_->acquire(LOCK_SH);
}
bool CameraSurfaceLease::acquire_exclusive() {
  return impl_->acquire(LOCK_EX);
}
bool CameraSurfaceLease::holds() const {
  impl_->verify();
  return impl_->mode != 0;
}
void CameraSurfaceLease::release() noexcept {
  if (impl_->mode) {
    (void)::flock(impl_->file.fd, LOCK_UN);
    impl_->mode = 0;
  }
}
}  // namespace headunit::camera
