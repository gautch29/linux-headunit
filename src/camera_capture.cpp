// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_capture.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace headunit::camera {
namespace {
void require(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
std::uint64_t monotonic_now() {
  return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count());
}
constexpr std::uint32_t yuyv_fourcc = std::uint32_t('Y') | (std::uint32_t('U') << 8) |
                                      (std::uint32_t('Y') << 16) | (std::uint32_t('V') << 24);
void validate_device_format(const Configuration& c, const CaptureDeviceFormat& f) {
  validate_packed_capture_format(f.pixels);
  const auto width = c.v4l2->adaptation ? c.v4l2->adaptation->width : c.width;
  const auto height = c.v4l2->adaptation ? c.v4l2->adaptation->height : c.height;
  require(f.pixels.width == width && f.pixels.height == height &&
              f.pixels.pixel_format == c.v4l2->pixel_format && f.delivery == c.v4l2->delivery,
          "capture negotiation differs from the configured source");
  require(f.buffer_count >= 2 && f.buffer_count <= Queue::maximum_leases &&
              f.buffer_storage_bytes >= std::size_t(f.pixels.stride) * f.pixels.height &&
              f.buffer_storage_bytes <= maximum_packed_capture_storage,
          "capture device exceeds its fixed storage bound");
  require(f.interval_numerator && f.interval_denominator,
          "capture device omitted its negotiated interval");
  const auto scaled = std::uint64_t(f.interval_numerator) * c.frame_rate;
  const auto denominator = std::uint64_t(f.interval_denominator);
  require(std::uint64_t(f.interval_numerator) * 60 >= denominator &&
              scaled * 100 >= denominator * 95 && scaled * 100 <= denominator * 105,
          "capture device cadence differs from the configured limit");
}
}  // namespace
struct CaptureProvider::Impl {
  Configuration configuration;
  Queue& queue;
  DeviceFactory factory;
  TimeSource time;
  mutable std::mutex mutex;
  std::condition_variable wake;
  CaptureStatistics stats;
  std::atomic<bool> stopping{false};
  std::atomic<bool> finished{false};
  std::uint64_t last_polled_published = 0;
  std::optional<std::uint64_t> last_observed, last_capture;
  std::optional<std::uint32_t> last_sequence;
  std::optional<CaptureReference> reference;
  std::thread worker;
  Impl(Configuration c, Queue& q, DeviceFactory f, TimeSource t)
      : configuration(std::move(c)),
        queue(q),
        factory(std::move(f)),
        time(t ? std::move(t) : TimeSource{monotonic_now}) {
    validate_configuration(configuration);
    require(configuration.enabled && configuration.source == SourceKind::v4l2 && bool(factory),
            "capture worker needs an enabled V4L2 source and explicit device factory");
    require(queue.availability(0) != Availability::closed,
            "capture worker cannot reuse a closed queue generation");
    worker = std::thread([this] { run(); });
  }
  ~Impl() {
    stopping = true;
    wake.notify_all();
    if (worker.joinable()) worker.join();
  }
  std::uint64_t observed() {
    const auto at = time();
    require(at && (!last_observed || at >= *last_observed),
            "camera capture observation clock reversed");
    last_observed = at;
    return at;
  }
  void count(std::uint64_t CaptureStatistics::* field) {
    std::lock_guard lock(mutex);
    ++(stats.*field);
  }
  bool fresh(std::uint64_t capture, std::uint64_t at) const {
    return at - capture <= std::uint64_t(configuration.maximum_age_ms) * 1000000;
  }
  void validate_sample(const CaptureSample& s, const CaptureDeviceFormat& f, std::uint64_t at) {
    if (last_sequence) {
      const std::uint32_t delta = s.driver_sequence - *last_sequence;
      require(delta && delta < 0x80000000U, "camera driver sequence repeated or reversed");
    }
    require(s.reference == CaptureReference::end_of_frame ||
                s.reference == CaptureReference::start_of_exposure,
            "camera driver acquisition reference is unknown");
    require(!reference || *reference == s.reference, "camera driver acquisition reference changed");
    require(s.capture_monotonic_ns && s.capture_monotonic_ns <= at &&
                (!last_capture || s.capture_monotonic_ns > *last_capture),
            "camera driver acquisition time is missing, future, repeated or reversed");
    if (f.delivery == CaptureDelivery::cpu_copy) {
      const auto minimum =
          std::size_t(f.pixels.stride) * (f.pixels.height - 1) +
          std::size_t(f.pixels.width) * (f.pixels.pixel_format == CapturePixelFormat::yuyv ? 2 : 3);
      require(!s.native && s.lifetime && s.packed.size() >= minimum &&
                  s.packed.size() <= f.buffer_storage_bytes,
              "camera copy sample lost immutable bounded backing");
    } else {
      const auto& n = s.native;
      require(s.packed.empty() && n && n->lifetime &&
                  f.pixels.pixel_format == CapturePixelFormat::yuyv && n->fourcc == yuyv_fourcc &&
                  n->modifier == 0 && n->plane_count == 1 && n->planes[0].fd >= 0 &&
                  n->planes[0].offset == 0 && n->planes[0].stride == f.pixels.stride &&
                  n->metadata.width == f.pixels.width && n->metadata.height == f.pixels.height &&
                  n->storage_width == f.pixels.width && n->storage_height == f.pixels.height &&
                  !n->crop_x && !n->crop_y && n->matrix == f.pixels.matrix &&
                  n->full_range == f.pixels.full_range && !n->metadata.sender_ntp &&
                  n->metadata.timestamp_ns == s.capture_monotonic_ns,
              "camera DMA-buf sample differs from original bounded capture metadata");
    }
    {
      std::lock_guard lock(mutex);
      if (last_sequence)
        stats.sequence_gaps += std::uint32_t(s.driver_sequence - *last_sequence) - 1;
      stats.last_dequeued =
          CaptureTiming{s.driver_sequence, s.capture_monotonic_ns, at, s.reference};
      stats.maximum_dequeue_age_ns =
          std::max(stats.maximum_dequeue_age_ns, at - s.capture_monotonic_ns);
    }
    last_sequence = s.driver_sequence;
    reference = s.reference;
    last_capture = s.capture_monotonic_ns;
  }
  void published(const CaptureSample& sample, std::uint64_t at) {
    std::lock_guard lock(mutex);
    ++stats.published;
    stats.last_published =
        CaptureTiming{sample.driver_sequence, sample.capture_monotonic_ns, at, sample.reference};
    stats.maximum_publication_age_ns =
        std::max(stats.maximum_publication_age_ns, at - sample.capture_monotonic_ns);
  }
  void capture_turn(CaptureDevice& device, const CaptureDeviceFormat& format) {
    device.reclaim();
    std::optional<CaptureSample> latest;
    // No catch-up loop: inspect at most the allocated capture slots and retain
    // only the newest valid sample. A released slot can be requeued next turn.
    for (unsigned i = 0; i < format.buffer_count && !stopping; ++i) {
      auto next = device.take();
      if (!next) break;
      count(&CaptureStatistics::dequeued);
      if (next->corrupted) {
        count(&CaptureStatistics::corrupted);
        continue;
      }
      const auto at = observed();
      validate_sample(*next, format, at);
      if (!fresh(next->capture_monotonic_ns, at)) {
        count(&CaptureStatistics::stale);
        continue;
      }
      if (latest) count(&CaptureStatistics::superseded);
      latest = std::move(next);
    }
    if (latest && !stopping) {
      if (!queue.can_offer())
        count(&CaptureStatistics::backpressure);
      else {
        bool accepted = false, attempted = false;
        std::uint64_t publication_observed = 0;
        CaptureAcquisition acquisition;
        acquisition.width = format.pixels.width;
        acquisition.height = format.pixels.height;
        acquisition.stride_bytes = format.pixels.stride;
        acquisition.driver_sequence = latest->driver_sequence;
        if (format.delivery == CaptureDelivery::cpu_copy) {
          // A driver allocation/bytesused may include a trailing padded tail.
          // Validation above checks the original span covers visible rows;
          // conversion reads only the negotiated pixel storage, never that tail.
          const auto pixel_storage = std::size_t(format.pixels.stride) * format.pixels.height;
          auto image = copy_capture_image(
              format.pixels, latest->packed.first(std::min(latest->packed.size(), pixel_storage)));
          if (configuration.v4l2->adaptation) {
            CaptureMapping mapping;
            image = adapt_capture_image(image, configuration.width, configuration.height,
                                        configuration.v4l2->adaptation->fit, &mapping);
            acquisition.adaptation_fit = configuration.v4l2->adaptation->fit;
            acquisition.mapping = mapping;
          }
          const auto at = observed();
          publication_observed = at;
          acquisition.published_monotonic_ns = at;
          require(latest->capture_monotonic_ns <= at, "camera copy observation precedes capture");
          if (fresh(latest->capture_monotonic_ns, at)) {
            attempted = true;
            accepted = queue.offer_image(image, latest->capture_monotonic_ns, at, false,
                                         latest->reference, acquisition);
          } else
            count(&CaptureStatistics::stale);
        } else {
          const auto at = observed();
          publication_observed = at;
          acquisition.published_monotonic_ns = at;
          require(latest->capture_monotonic_ns <= at, "camera import observation precedes capture");
          if (fresh(latest->capture_monotonic_ns, at)) {
            attempted = true;
            accepted = queue.offer_native(latest->native, latest->capture_monotonic_ns, at,
                                          latest->reference, acquisition);
          } else
            count(&CaptureStatistics::stale);
        }
        if (accepted)
          published(*latest, publication_observed);
        else if (attempted)
          count(&CaptureStatistics::backpressure);
      }
    }
    latest.reset();
    device.reclaim();
  }
  void run() noexcept {
    std::unique_ptr<CaptureDevice> device;
    try {
      if (!stopping) {
        device = factory();
        require(bool(device), "capture device factory returned no driver");
        const auto format = device->start();
        validate_device_format(configuration, format);
        {
          std::lock_guard lock(mutex);
          stats.format = format;
          stats.state = CaptureStatistics::State::running;
        }
        while (!stopping) {
          capture_turn(*device, format);
          std::unique_lock lock(mutex);
          wake.wait_for(lock, std::chrono::milliseconds(4), [this] { return stopping.load(); });
        }
      }
    } catch (const std::exception& error) {
      // Publish terminal withdrawal before driver teardown, which may block.
      queue.close();
      std::lock_guard lock(mutex);
      stats.error = std::string(error.what()).substr(0, 512);
      stats.state = CaptureStatistics::State::failed;
    } catch (...) {
      // Publish terminal withdrawal before driver teardown, which may block.
      queue.close();
      std::lock_guard lock(mutex);
      stats.error = "camera capture failed";
      stats.state = CaptureStatistics::State::failed;
    }
    if (device) device->stop();
    device.reset();  // Driver cleanup precedes the retirement acknowledgement.
    {
      std::lock_guard lock(mutex);
      if (stats.state != CaptureStatistics::State::failed)
        stats.state = CaptureStatistics::State::stopped;
    }
    finished = true;
  }
};
CaptureProvider::CaptureProvider(Configuration c, Queue& q, DeviceFactory f, TimeSource t)
    : impl_(std::make_unique<Impl>(std::move(c), q, std::move(f), std::move(t))) {}
CaptureProvider::~CaptureProvider() = default;
bool CaptureProvider::poll(std::uint64_t, Queue& q) {
  require(&q == &impl_->queue, "capture provider cannot publish to a different queue");
  std::lock_guard lock(impl_->mutex);
  const auto count = impl_->stats.published;
  const bool changed = count != impl_->last_polled_published;
  impl_->last_polled_published = count;
  return changed;
}
std::optional<std::string> CaptureProvider::failure() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats.error;
}
CaptureStatistics CaptureProvider::statistics() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}
void CaptureProvider::request_stop() noexcept {
  impl_->stopping = true;
  impl_->wake.notify_all();
}
bool CaptureProvider::stopped() const noexcept {
  return impl_->finished.load();
}
}  // namespace headunit::camera
