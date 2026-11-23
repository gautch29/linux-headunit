// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera.hpp>
#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <cmath>
#include <stdexcept>

namespace headunit::camera {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
bool fresh(std::uint64_t capture, std::uint64_t now, std::uint64_t age) {
  return capture <= now && now - capture <= age;
}
bool view_identifier(std::string_view name) {
  return !name.empty() && name.size() <= 128 &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.' || c == '/';
         });
}
void validate_acquisition(const Configuration& c, std::uint64_t observed,
                          const std::optional<CaptureAcquisition>& acquisition) {
  if (!acquisition) return;
  const auto& source = *acquisition;
  check(c.source == SourceKind::v4l2 && source.width && source.height && source.width <= 1920 &&
            source.height <= 1080 && source.stride_bytes &&
            std::uint64_t(source.stride_bytes) * source.height <= 32U * 1024U * 1024U &&
            source.published_monotonic_ns == observed &&
            (!source.adaptation_fit || *source.adaptation_fit == CaptureFit::contain ||
             *source.adaptation_fit == CaptureFit::cover),
        "invalid camera acquisition metadata");
  if (source.mapping) {
    const auto& mapping = *source.mapping;
    const auto bounded = [](const theme::graphics::Rect& r) {
      return std::isfinite(r.origin.x) && std::isfinite(r.origin.y) &&
             std::isfinite(r.size.width) && std::isfinite(r.size.height) &&
             std::abs(r.origin.x) <= 65536 && std::abs(r.origin.y) <= 65536 && r.size.width > 0 &&
             r.size.width <= 65536 && r.size.height > 0 && r.size.height <= 65536;
    };
    check(source.adaptation_fit && c.v4l2->delivery == CaptureDelivery::cpu_copy &&
              std::isfinite(mapping.scale) && mapping.scale > 0 && mapping.scale <= 30 &&
              bounded(mapping.image) && bounded(mapping.source) && bounded(mapping.destination),
          "invalid consumed camera mapping metadata");
  }
}
}  // namespace
struct Queue::Impl {
  struct Pool {
    std::atomic<unsigned> outstanding{0};
  };
  // Every externally held frame carries this permit. Queue cleanup cannot
  // recycle a capture buffer while an independent GPU submission retains it.
  struct Lease {
    std::shared_ptr<Pool> pool;
    Frame frame;
    explicit Lease(std::shared_ptr<Pool> p) : pool(std::move(p)) { ++pool->outstanding; }
    ~Lease() {
      frame.image.reset();
      frame.native.reset();
      --pool->outstanding;
    }
  };
  Configuration configuration;
  mutable std::mutex mutex;
  std::shared_ptr<Pool> pool = std::make_shared<Pool>();
  std::shared_ptr<const Frame> queued, current;
  std::deque<std::shared_ptr<const Frame>> staged;
  std::optional<std::uint64_t> last_capture;
  std::optional<CaptureReference> capture_reference;
  std::uint64_t serial = 0;
  Statistics stats;
  bool closed = false;
  enum class Storage { unset, image, native } storage = Storage::unset;
  explicit Impl(Configuration c) : configuration(std::move(c)) {
    validate_configuration(configuration);
  }
  std::uint64_t age() const { return std::uint64_t(configuration.maximum_age_ms) * 1000000; }
  bool can_offer() const {
    return configuration.enabled && !closed &&
           (pool->outstanding.load() < maximum_leases || bool(queued));
  }
  void validate_offer(std::uint64_t capture, std::uint64_t observed, Storage kind,
                      std::optional<CaptureReference> reference) const {
    check(!closed, "camera generation is closed");
    check(capture <= observed, "camera capture timestamp is in the future");
    check(!last_capture || capture > *last_capture, "camera capture timestamp did not increase");
    check(storage == Storage::unset || storage == kind,
          "camera storage changed; replace source generation");
    check(!reference || *reference == CaptureReference::end_of_frame ||
              *reference == CaptureReference::start_of_exposure,
          "invalid camera acquisition reference");
    check(!last_capture || reference == capture_reference,
          "camera acquisition reference changed; replace source generation");
    check(configuration.source != SourceKind::v4l2 || reference.has_value(),
          "V4L2 capture requires its driver acquisition reference");
    check(configuration.source != SourceKind::synthetic_test_pattern || !reference,
          "synthetic capture cannot claim a hardware acquisition reference");
    check(serial < std::numeric_limits<std::uint64_t>::max(), "camera sequence exhausted");
  }
  std::shared_ptr<Lease> reserve() {
    // A queued frame has never escaped this class, so replacing it releases
    // its one permit. Staged/current frames may be retained by the GPU.
    if (queued) {
      queued.reset();
      ++stats.superseded;
    }
    check(pool->outstanding.load() < maximum_leases, "camera permit accounting exceeded bound");
    auto lease = std::make_shared<Lease>(pool);
    stats.maximum_outstanding_leases =
        std::max(stats.maximum_outstanding_leases, pool->outstanding.load());
    return lease;
  }
  void commit(std::shared_ptr<Lease> lease, std::uint64_t capture, Storage kind) {
    lease->frame.sequence = ++serial;
    lease->frame.capture_monotonic_ns = capture;
    capture_reference = lease->frame.capture_reference;
    queued = std::shared_ptr<const Frame>(lease, &lease->frame);
    last_capture = capture;
    storage = kind;
    ++stats.accepted;
  }
  std::optional<std::uint64_t> retained_capture() const {
    // Acquisition ordering survives withdrawal, but its watermark is not a
    // source lease. A rejected newer capture cannot renew an older image.
    if (queued) return queued->capture_monotonic_ns;
    if (!staged.empty()) return staged.back()->capture_monotonic_ns;
    if (current) return current->capture_monotonic_ns;
    return {};
  }
  Availability availability(std::uint64_t now) const {
    if (closed) return Availability::closed;
    const auto capture = retained_capture();
    if (!capture) return Availability::missing;
    if (now < *last_capture) return Availability::clock_reversed;
    if (now - *capture > age()) return Availability::stale;
    return Availability::ready;
  }
};
Queue::Queue(Configuration c) : impl_(std::make_unique<Impl>(std::move(c))) {}
Queue::~Queue() = default;
bool Queue::can_offer() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->can_offer();
}
bool Queue::offer_image(theme::graphics::Image& image, std::uint64_t capture,
                        std::uint64_t observed, bool simulated,
                        std::optional<CaptureReference> reference,
                        std::optional<CaptureAcquisition> acquisition) {
  auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  q.validate_offer(capture, observed, Impl::Storage::image, reference);
  validate_acquisition(q.configuration, observed, acquisition);
  check(q.configuration.source != SourceKind::synthetic_test_pattern || simulated,
        "test-pattern source must be marked simulated");
  check(q.configuration.source != SourceKind::v4l2 ||
            (!simulated && q.configuration.v4l2->delivery == CaptureDelivery::cpu_copy),
        "V4L2 image does not match configured delivery");
  check(image.width == q.configuration.width && image.height == q.configuration.height &&
            image.pixels.size() == std::size_t(image.width) * image.height,
        "camera image dimensions differ from configuration");
  if (!q.can_offer()) {
    ++q.stats.backpressure;
    return false;
  }
  // Allocate an empty backing object before transferring producer ownership.
  auto image_owner = std::make_shared<theme::graphics::Image>();
  auto lease = q.reserve();
  *image_owner = std::move(image);
  lease->frame.image = std::move(image_owner);
  lease->frame.simulated = simulated;
  lease->frame.capture_reference = reference;
  lease->frame.acquisition = acquisition;
  q.commit(std::move(lease), capture, Impl::Storage::image);
  return true;
}
bool Queue::offer_native(std::shared_ptr<const theme::video::NativeFrame> frame,
                         std::uint64_t capture, std::uint64_t observed,
                         std::optional<CaptureReference> reference,
                         std::optional<CaptureAcquisition> acquisition) {
  auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  q.validate_offer(capture, observed, Impl::Storage::native, reference);
  validate_acquisition(q.configuration, observed, acquisition);
  check(q.configuration.source == SourceKind::external ||
            (q.configuration.source == SourceKind::v4l2 &&
             q.configuration.v4l2->delivery == CaptureDelivery::dma_buf),
        "native camera frame requires matching external delivery");
  check(frame && frame->lifetime && frame->metadata.width == q.configuration.width &&
            frame->metadata.height == q.configuration.height && frame->plane_count >= 1 &&
            frame->plane_count <= 4 && frame->storage_width >= frame->metadata.width &&
            frame->storage_width <= 4096 && frame->storage_height >= frame->metadata.height &&
            frame->storage_height <= 2160 &&
            frame->crop_x <= frame->storage_width - frame->metadata.width &&
            frame->crop_y <= frame->storage_height - frame->metadata.height,
        "invalid native camera dimensions or lifetime");
  for (unsigned i = 0; i < frame->plane_count; ++i)
    check(frame->planes[i].fd >= 0 && frame->planes[i].stride > 0,
          "invalid native camera plane descriptor");
  if (!q.can_offer()) {
    ++q.stats.backpressure;
    return false;
  }
  auto lease = q.reserve();
  lease->frame.native = std::move(frame);
  lease->frame.capture_reference = reference;
  lease->frame.acquisition = acquisition;
  q.commit(std::move(lease), capture, Impl::Storage::native);
  return true;
}
std::shared_ptr<const Frame> Queue::stage(std::uint64_t now) {
  auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  if (q.closed || !q.queued || q.staged.size() == maximum_staged) return {};
  if (!fresh(q.queued->capture_monotonic_ns, now, q.age())) {
    // Never compose future input on a reversed local clock. Keep the pending
    // frame until the clock catches up, but discard an actually stale frame.
    if (now >= q.queued->capture_monotonic_ns) {
      q.queued.reset();
      ++q.stats.stale_dropped;
    }
    return {};
  }
  q.staged.push_back(q.queued);
  q.queued.reset();
  ++q.stats.staged;
  return q.staged.back();
}
bool Queue::complete(std::uint64_t sequence, bool presented) {
  auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  check(sequence > 0 && sequence <= q.serial, "unknown camera receipt token");
  const auto found = std::find_if(q.staged.begin(), q.staged.end(),
                                  [&](const auto& f) { return f->sequence == sequence; });
  if (found == q.staged.end()) return false;
  check(found == q.staged.begin(), "camera receipt order changed");
  if (presented) {
    q.current = std::move(q.staged.front());
    ++q.stats.presented;
  } else
    ++q.stats.cancelled;
  q.staged.pop_front();
  return true;
}
std::shared_ptr<const Frame> Queue::current(std::uint64_t now) const {
  const auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  return q.availability(now) == Availability::ready && q.current &&
                 fresh(q.current->capture_monotonic_ns, now, q.age())
             ? q.current
             : nullptr;
}
Availability Queue::availability(std::uint64_t now) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->availability(now);
}
std::optional<std::uint64_t> Queue::available_capture_ns(std::uint64_t now) const {
  const auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  return q.availability(now) == Availability::ready ? q.retained_capture() : std::nullopt;
}
void Queue::discard() {
  auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  q.stats.cancelled += q.staged.size();
  q.queued.reset();
  q.current.reset();
  q.staged.clear();
}
void Queue::close() {
  auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  q.closed = true;
  q.stats.cancelled += q.staged.size();
  q.queued.reset();
  q.current.reset();
  q.staged.clear();
}
Statistics Queue::statistics() const {
  const auto& q = *impl_;
  std::lock_guard lock(q.mutex);
  auto stats = q.stats;
  stats.outstanding_leases = q.pool->outstanding.load();
  return stats;
}

SyntheticProvider::SyntheticProvider(Configuration c) : configuration_(std::move(c)) {
  validate_configuration(configuration_);
  check(configuration_.source == SourceKind::synthetic_test_pattern,
        "synthetic provider requires test-pattern source");
}
bool SyntheticProvider::poll(std::uint64_t now, Queue& queue) {
  check(!last_poll_ || now >= *last_poll_,
        "synthetic camera clock reversed; replace provider generation");
  last_poll_ = now;
  if (!configuration_.enabled || (next_due_ && now < *next_due_) || !queue.can_offer())
    return false;
  const auto interval = (1000000000ULL + configuration_.frame_rate - 1) / configuration_.frame_rate;
  check(now <= std::numeric_limits<std::uint64_t>::max() - interval,
        "synthetic camera clock exhausted");
  theme::graphics::Image image;
  image.width = configuration_.width;
  image.height = configuration_.height;
  image.pixels.resize(std::size_t(image.width) * image.height);
  // Original coloured bars and a moving black/white checker stripe are
  // deliberately unlike a camera feed. The simulated flag must also be shown
  // by the eventual display. This CPU generator is an offline fixture only.
  constexpr std::uint32_t colours[] = {0xffdddddd, 0xffdddd20, 0xff20dddd, 0xff20dd20,
                                       0xffdd20dd, 0xffdd2020, 0xff2020dd, 0xff202020};
  for (unsigned y = 0; y < image.height; ++y)
    for (unsigned x = 0; x < image.width; ++x) {
      auto pixel = colours[std::uint64_t(x) * 8 / image.width];
      if (y >= image.height * 3 / 4)
        pixel = ((x / 16 + ordinal_ + y / 16) & 1) ? 0xffffffff : 0xff101010;
      image.pixels[std::size_t(y) * image.width + x] = pixel;
    }
  if (!queue.offer_image(image, now, now, true)) return false;
  ++ordinal_;
  next_due_ = now + interval;
  return true;
}

Session::Session(Configuration c) : configuration_(std::move(c)), frames_(configuration_) {}
bool Session::enter(std::string previous) {
  check(view_identifier(previous) && previous != "camera", "invalid camera restore view");
  if (!configuration_.enabled || !configuration_.manual_activation || previous_view_) return false;
  if (provider_ && provider_->stopped()) provider_.reset();
  if (provider_ || frames_.availability(0) == Availability::closed) return false;
  frames_.discard();
  provider_ = make_provider(configuration_, frames_);
  previous_view_ = std::move(previous);
  return true;
}
std::optional<std::string> Session::leave() {
  auto previous = std::move(previous_view_);
  previous_view_.reset();
  frames_.discard();
  if (provider_) {
    provider_->request_stop();
    if (provider_->stopped()) provider_.reset();
  }
  return previous;
}
SessionState Session::turn(std::uint64_t now) {
  if (!previous_view_ && provider_ && provider_->stopped()) provider_.reset();
  if (previous_view_ && provider_) provider_->poll(now, frames_);
  return {bool(previous_view_), previous_view_,
          previous_view_ ? frames_.availability(now) : Availability::missing};
}
}  // namespace headunit::camera
