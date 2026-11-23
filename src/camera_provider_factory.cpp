// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_capture.hpp>
#include <stdexcept>

namespace headunit::camera {
#ifndef __linux__
std::unique_ptr<CaptureDevice> make_v4l2_capture_device(const Configuration&) {
  throw std::invalid_argument("V4L2 camera capture requires Linux");
}
#endif
std::unique_ptr<Provider> make_provider(const Configuration& configuration, Queue& queue) {
  validate_configuration(configuration);
  if (!configuration.enabled || configuration.source == SourceKind::external) return {};
  if (configuration.source == SourceKind::synthetic_test_pattern)
    return std::make_unique<SyntheticProvider>(configuration);
  return std::make_unique<CaptureProvider>(
      configuration, queue, [configuration] { return make_v4l2_capture_device(configuration); });
}
}  // namespace headunit::camera
