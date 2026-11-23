// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/camera.hpp>
#include <headunit/camera_pixels.hpp>
#include <functional>
#include <span>

namespace headunit::camera {
struct CaptureDeviceFormat {
  PackedCaptureFormat pixels;
  CaptureDelivery delivery = CaptureDelivery::dma_buf;
  unsigned buffer_count = 0;
  std::size_t buffer_storage_bytes = 0;
  // Actual negotiated seconds per frame; not an invented frame clock.
  std::uint32_t interval_numerator = 0, interval_denominator = 0;
};
struct CaptureSample {
  std::uint32_t driver_sequence = 0;
  std::uint64_t capture_monotonic_ns = 0;
  CaptureReference reference = CaptureReference::end_of_frame;
  bool corrupted = false;
  std::span<const std::uint8_t> packed;
  std::shared_ptr<const void> lifetime;
  std::shared_ptr<const theme::video::NativeFrame> native;
};
// All driver calls belong to one capture worker. take() is nonblocking.
// Returned storage stays immutable until its lifetime is released, including
// after stop(). Release only posts bounded return information; it must never
// perform an ioctl on the GPU/presentation thread.
// Live take/reclaim faults must throw before blocking cleanup. The worker
// closes its Queue and reports failure, then calls stop to retire the driver.
class CaptureDevice {
 public:
  virtual ~CaptureDevice() = default;
  virtual CaptureDeviceFormat start() = 0;
  virtual std::optional<CaptureSample> take() = 0;
  virtual void reclaim() = 0;
  virtual void stop() noexcept = 0;
};
struct CaptureTiming {
  std::uint32_t driver_sequence = 0;
  std::uint64_t capture_monotonic_ns = 0, observed_monotonic_ns = 0;
  CaptureReference reference = CaptureReference::end_of_frame;
};
struct CaptureStatistics {
  enum class State { starting, running, failed, stopped };
  State state = State::starting;
  std::uint64_t dequeued = 0, published = 0, superseded = 0, corrupted = 0, stale = 0,
                backpressure = 0;
  // Gaps between valid driver sequences include samples rejected as corrupt;
  // they are not a count of lost physical exposures. The first sequence is an
  // unknown baseline. A genuine forward 32-bit wrap adds no gap by itself.
  std::uint64_t sequence_gaps = 0;
  // Original acquisition and worker observation dates, not display receipts.
  // Dequeue includes valid stale samples. Only a successful Queue offer updates
  // publication timing; missing/corrupt/stale/backpressured turns never renew it.
  std::optional<CaptureTiming> last_dequeued, last_published;
  std::uint64_t maximum_dequeue_age_ns = 0, maximum_publication_age_ns = 0;
  std::optional<CaptureDeviceFormat> format;
  std::optional<std::string> error;
};
// Queue outlives the provider. Construction starts the worker; construction and
// poll do no device work on the caller. The worker owns initialization, capture,
// conversion and cleanup and may begin before construction returns.
class CaptureProvider final : public Provider {
 public:
  using DeviceFactory = std::function<std::unique_ptr<CaptureDevice>()>;
  using TimeSource = std::function<std::uint64_t()>;
  CaptureProvider(Configuration, Queue&, DeviceFactory, TimeSource = {});
  ~CaptureProvider();
  CaptureProvider(const CaptureProvider&) = delete;
  CaptureProvider& operator=(const CaptureProvider&) = delete;
  bool poll(std::uint64_t, Queue&) override;
  std::optional<std::string> failure() const override;
  void request_stop() noexcept override;
  bool stopped() const noexcept override;
  CaptureStatistics statistics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Allocates an unopened Linux driver adapter. Only CaptureDevice::start opens
// the configured device. The default factory is unavailable on other systems.
std::unique_ptr<CaptureDevice> make_v4l2_capture_device(const Configuration&);
}  // namespace headunit::camera
