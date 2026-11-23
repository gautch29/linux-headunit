// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <memory>
#include <string>

namespace headunit {
// Explicit native-page/main-renderer handoff. The request is an atomic owned
// 0600 OFF or timed ON record; the stable empty lease is an owned 0600 flock
// inode. Both live in the same canonical real owned 0700 parent.
// The native page may map foreground only while holding an exclusive lease.
// The renderer releases its shared lease AFTER destroying its GPU/window.
class MainSurfaceControl {
 public:
  // Read-only admission, including existing endpoint metadata. Missing request
  // and lease are permitted; an unavailable request never admits rendering.
  static void validate(const std::string& control_path, const std::string& lease_path);
  MainSurfaceControl(const std::string& control_path, const std::string& lease_path);
  ~MainSurfaceControl();
  MainSurfaceControl(const MainSurfaceControl&) = delete;
  MainSurfaceControl& operator=(const MainSurfaceControl&) = delete;
  // Thread-safe bounded queries for Standard control and presentation owners.
  // Unknown input requests withdrawal; only valid OFF/expiry permits return.
  bool native_requested() const noexcept;
  bool renderer_acquire();
  bool renderer_holds() const;
  void renderer_release() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
