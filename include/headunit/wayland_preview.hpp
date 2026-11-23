// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/touch_input.hpp"
#include <memory>
#include <string>
#include <vector>
typedef struct _GstElement GstElement;
namespace headunit {
class RouteGuidance;
// Owns the parent surface and its input. GStreamer owns only video subsurfaces.
// The pipeline must reach NULL and release its sink before this object dies.
class WaylandPreview {
 public:
  WaylandPreview(unsigned width, unsigned height, bool cluster = false,
                 const std::string& display = {});
  ~WaylandPreview();
  void attach(GstElement* sink);
  void pump();
  // Borrowed readiness descriptor; only pump() may read or dispatch it.
  int input_fd() const noexcept;
  void set_navigation(std::shared_ptr<RouteGuidance> source);
  std::vector<TouchInput> take_touches();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
