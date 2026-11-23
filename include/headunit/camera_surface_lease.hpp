// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <memory>
#include <string>

namespace headunit::camera {
// Main surfaces hold a shared lease; a native camera needs the exclusive
// lease. Withdraw and finish the corresponding GPU surface before releasing.
// The exact owned 0600 file lives in a real owned 0700 parent. It is never
// replaced by either owner; replacing it invalidates an open generation.
class CameraSurfaceLease {
 public:
  // Read-only preflight: a missing lease is permitted in a valid private
  // parent. An existing lease must satisfy the runtime metadata contract.
  // Does not create a file, acquire a lock, read bytes or retain an owner.
  static void validate(const std::string& absolute_path);
  explicit CameraSurfaceLease(const std::string& absolute_path);
  ~CameraSurfaceLease();
  CameraSurfaceLease(const CameraSurfaceLease&) = delete;
  CameraSurfaceLease& operator=(const CameraSurfaceLease&) = delete;
  bool acquire_shared();
  bool acquire_exclusive();
  bool holds() const;
  void release() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::camera
