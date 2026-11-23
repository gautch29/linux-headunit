// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Production probe main, with ONLY the capture-device factory substituted.
// Synthetic YUYV pixels and actual host-steady dates; no device/GPU/phone owner.
#include <headunit/camera_capture.hpp>
#include <chrono>
#include <iostream>
#include <vector>

namespace headunit::camera {
namespace {
std::uint64_t monotonic_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
class FixtureDevice final : public CaptureDevice {
  std::shared_ptr<const std::vector<std::uint8_t>> pixels_;
  std::uint64_t next_ = 0;
  std::uint32_t sequence_ = 0;

 public:
  CaptureDeviceFormat start() override {
    // Fixed negotiated backing deliberately differs from arbitrary requests.
    // Padding is sentinel data, never part of the acquired pixels.
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(1296 * 360, 0xe7);
    for (unsigned y = 0; y < 360; ++y)
      for (unsigned x = 0; x < 640; x += 2) {
        auto* p = pixels->data() + y * 1296 + x * 2;
        p[0] = 81;
        p[1] = 90;
        p[2] = 81;
        p[3] = 240;
      }
    pixels_ = std::move(pixels);
    return {{640, 360, 1296, CapturePixelFormat::yuyv, theme::video::YuvMatrix::bt601, false},
            CaptureDelivery::cpu_copy,
            2,
            1296 * 360,
            1,
            30};
  }
  std::optional<CaptureSample> take() override {
    const auto at = monotonic_ns();
    if (at < next_) return {};
    next_ = at + 33333334;
    CaptureSample sample;
    sample.driver_sequence = ++sequence_;
    sample.capture_monotonic_ns = at - 1000000;
    sample.reference = CaptureReference::start_of_exposure;
    sample.packed = *pixels_;
    sample.lifetime = pixels_;
    return sample;
  }
  void reclaim() override {}
  void stop() noexcept override {
    pixels_.reset();
    std::cerr << "synthetic probe fixture: device stopped\n";
  }
};
}  // namespace
std::unique_ptr<CaptureDevice> make_v4l2_capture_device(const Configuration&) {
  return std::make_unique<FixtureDevice>();
}
}  // namespace headunit::camera
