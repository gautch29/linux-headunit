// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <memory>
#include <string>

namespace headunit {
namespace camera {
class ActivationControl;
}
class MainSurfaceControl;

// Immutable startup selection for the optional owned Standard main presenter.
// Cluster output never receives the main window's camera ownership.
struct MainPreviewOptions {
  std::string wayland_display;
  std::string output_name;
  std::shared_ptr<const camera::ActivationControl> camera_control;
  std::string camera_lease_file;
  std::shared_ptr<MainSurfaceControl> native_page_control;
};
}  // namespace headunit
