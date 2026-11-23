// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/main_surface_control.hpp>
#include <headunit/camera_control.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <cerrno>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit {
namespace {
void require(bool value, const char* why) {
  if (!value) throw std::invalid_argument(why);
}
std::filesystem::path endpoint(const std::string& value) {
  const std::filesystem::path path(value);
  require(!value.empty() && value.size() <= 4000 && path.is_absolute() &&
              path.lexically_normal() == path && !path.filename().empty(),
          "invalid native page endpoint path");
  for (const unsigned char c : value)
    require(c >= 32 && c < 127, "native page paths must be printable ASCII");
  require(std::filesystem::canonical(path.parent_path()) == path.parent_path(),
          "native page parent must have real canonical ancestors");
  return path;
}
void validate_paths(const std::string& control, const std::string& lease) {
  const auto request = endpoint(control), lock = endpoint(lease);
  require(request != lock && request.parent_path() == lock.parent_path(),
          "native page endpoints must be distinct in the same parent");
  (void)camera::ActivationControl(control);
  camera::CameraSurfaceLease::validate(lease);
  struct stat first{}, second{};
  if (::lstat(control.c_str(), &first) == 0) {
    require(S_ISREG(first.st_mode) && first.st_uid == ::getuid() && first.st_nlink == 1 &&
                (first.st_mode & 07777) == 0600 && first.st_size >= 0 &&
                first.st_size <= off_t(camera::maximum_activation_request_bytes),
            "unsafe native page request endpoint");
    if (::lstat(lease.c_str(), &second) == 0)
      require(first.st_dev != second.st_dev || first.st_ino != second.st_ino,
              "native page request aliases its surface lease");
  } else
    require(errno == ENOENT, "cannot inspect native page request endpoint");
}
}  // namespace
struct MainSurfaceControl::Impl {
  mutable std::mutex mutex;
  camera::ActivationControl control;
  camera::CameraSurfaceLease lease;
  Impl(const std::string& request, const std::string& lock) : control(request), lease(lock) {}
};
void MainSurfaceControl::validate(const std::string& control, const std::string& lease) {
  validate_paths(control, lease);
}
MainSurfaceControl::MainSurfaceControl(const std::string& control, const std::string& lease) {
  validate_paths(control, lease);
  impl_ = std::make_unique<Impl>(control, lease);
}
MainSurfaceControl::~MainSurfaceControl() = default;
bool MainSurfaceControl::native_requested() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->control.read() != camera::ActivationRequest::off;
}
bool MainSurfaceControl::renderer_acquire() {
  std::lock_guard lock(impl_->mutex);
  return impl_->control.read() == camera::ActivationRequest::off && impl_->lease.acquire_shared();
}
bool MainSurfaceControl::renderer_holds() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->lease.holds();
}
void MainSurfaceControl::renderer_release() noexcept {
  std::lock_guard lock(impl_->mutex);
  impl_->lease.release();
}
}  // namespace headunit
