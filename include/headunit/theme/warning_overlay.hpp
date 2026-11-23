// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/scene.hpp>
#include <headunit/vehicle.hpp>
#include <memory>
#include <string_view>

namespace headunit::theme::graphics {
struct WarningPlacement {
  Indicator signal;
  Rect frame;
};
struct WarningOverlayConfiguration {
  bool enabled = true;
  bool hide_during_startup = true;
  Size canvas;
  std::chrono::milliseconds maximum_age{500};
  Rect simulation_badge;
  std::vector<WarningPlacement> indicators;
};
// Strict versioned JSON, independent of Apple resource identifiers or layouts.
WarningOverlayConfiguration decode_warning_overlay_configuration(std::string_view);
void validate_warning_overlay_configuration(const WarningOverlayConfiguration&);

// Display-owner only. Rasterizes small original symbol tiles once, then retains
// immutable scene/image objects for GPU composition. Fades select prebuilt
// brightness trees without C++ heap allocation; topology/output changes rebuild
// a bounded table. Never rasterizes a frame.
class WarningOverlay {
 public:
  explicit WarningOverlay(WarningOverlayConfiguration);
  ~WarningOverlay();
  WarningOverlay(const WarningOverlay&) = delete;
  WarningOverlay& operator=(const WarningOverlay&) = delete;
  void set_hide_during_startup(bool);
  std::shared_ptr<const Layer> scene(const VehicleSample&, Clock::time_point, Size output,
                                     bool startup = false);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::graphics
