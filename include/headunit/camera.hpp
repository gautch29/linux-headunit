// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/native_frame.hpp>
#include <headunit/theme/scene.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace headunit::camera {
enum class SourceKind { synthetic_test_pattern, external, v4l2 };
enum class CapturePixelFormat { yuyv, rgb24, bgr24 };
enum class CaptureDelivery { dma_buf, cpu_copy };
enum class CaptureReference { end_of_frame, start_of_exposure };
enum class CaptureFit { contain, cover };
struct CaptureAdaptation {
  unsigned width = 0, height = 0;  // Exact driver acquisition size.
  CaptureFit fit = CaptureFit::contain;
};
struct V4l2Configuration {
  std::string device;
  CapturePixelFormat pixel_format = CapturePixelFormat::yuyv;
  CaptureDelivery delivery = CaptureDelivery::dma_buf;
  // Optional CPU conversion into Configuration width/height. Capture dates and
  // reference semantics remain those of the original driver sample.
  std::optional<CaptureAdaptation> adaptation = std::nullopt;
};
enum class Panel { main, cluster };
struct Geometry {
  unsigned x = 0, y = 0, width = 1280, height = 720;
};
struct Configuration {
  bool enabled = false, manual_activation = false;
  std::string stream_identifier =
      "rear-camera";  // Local identity; no Apple numeric resource identifier.
  SourceKind source = SourceKind::synthetic_test_pattern;
  unsigned width = 1280, height = 720, frame_rate = 30, maximum_age_ms = 150;
  Panel panel = Panel::main;
  unsigned canvas_width = 1280, canvas_height = 720;
  Geometry destination;
  unsigned rotation_degrees = 0;
  bool mirror_horizontal = false;
  std::optional<V4l2Configuration> v4l2 = std::nullopt;
};
void validate_configuration(const Configuration&);
Configuration decode_configuration(std::string_view);

// Pixel-edge coordinates before orientation or scene placement. image is the
// complete scaled acquisition (cover may extend outside delivery). source and
// destination describe its visible intersection in acquisition/delivery pixels.
// Fractional edges are intentional; sampling uses delivery pixel centres.
struct CaptureMapping {
  double scale = 1;
  theme::graphics::Rect image, source, destination;
};
// Actual validated driver format and successful source publication copied by
// CaptureProvider onto that immutable frame. This is not requested geometry or
// a receipt. No format/stride/driver identity is inferred for other producers.
struct CaptureAcquisition {
  unsigned width = 0, height = 0, stride_bytes = 0;
  std::uint32_t driver_sequence = 0;
  std::uint64_t published_monotonic_ns = 0;
  std::optional<CaptureFit> adaptation_fit;
  std::optional<CaptureMapping> mapping;
};
// Capture time belongs to the local CLOCK_MONOTONIC/steady_clock domain. It is
// never a phone NTP value or a receipt timestamp. Replace a Queue on source or
// clock discontinuity; sequence numbers remain unique within that generation.
struct Frame {
  std::uint64_t sequence = 0, capture_monotonic_ns = 0;
  bool simulated = false;
  std::shared_ptr<const theme::graphics::Image> image;
  std::shared_ptr<const theme::video::NativeFrame> native;
  // The driver reports which acquisition event its monotonic timestamp marks;
  // absence means the producer has supplied no hardware reference semantics.
  std::optional<CaptureReference> capture_reference = std::nullopt;
  std::optional<CaptureAcquisition> acquisition = std::nullopt;
};
enum class Availability { missing, ready, stale, clock_reversed, closed };
struct Statistics {
  std::uint64_t accepted = 0, superseded = 0, backpressure = 0, stale_dropped = 0;
  std::uint64_t staged = 0, presented = 0, cancelled = 0;
  unsigned outstanding_leases = 0, maximum_outstanding_leases = 0;
};

// Capture producers may publish from another thread. The presentation owner
// stages/completes in submission order. One latest queued frame, two staged
// submissions, and one current frame are allowed, with FOUR leases total even
// if consumers retain a frame after cancellation. GPU users must retain the
// returned immutable frame until their own fence signals, not just submit().
class Queue {
 public:
  static constexpr unsigned maximum_leases = 4, maximum_staged = 2;
  explicit Queue(Configuration);
  ~Queue();
  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;
  bool can_offer() const;
  // false preserves the producer's image. Invalid dimensions, future/repeated
  // capture timestamps, changed storage kind or closed generations throw.
  bool offer_image(theme::graphics::Image&, std::uint64_t capture_ns, std::uint64_t observed_ns,
                   bool simulated = false,
                   std::optional<CaptureReference> capture_reference = std::nullopt,
                   std::optional<CaptureAcquisition> acquisition = std::nullopt);
  bool offer_native(std::shared_ptr<const theme::video::NativeFrame>, std::uint64_t capture_ns,
                    std::uint64_t observed_ns,
                    std::optional<CaptureReference> capture_reference = std::nullopt,
                    std::optional<CaptureAcquisition> acquisition = std::nullopt);
  std::shared_ptr<const Frame> stage(std::uint64_t now_ns);
  // false ignores an old receipt after discard/leave. Never-issued tokens and
  // out-of-order outstanding receipts throw; only a true presentation advances
  // current(). Tokens are not recycled by enter/exit.
  bool complete(std::uint64_t sequence, bool actually_presented);
  // A stale or future current frame is never returned for a new composition.
  std::shared_ptr<const Frame> current(std::uint64_t now_ns) const;
  Availability availability(std::uint64_t now_ns) const;
  // Fresh latest retained acquisition, including a queued frame not yet visible.
  // Rejected captures and external-only GPU leases cannot renew this date.
  // Readiness must still check the age of the older source being composed.
  std::optional<std::uint64_t> available_capture_ns(std::uint64_t now_ns) const;
  void discard();  // Retains capture ordering and externally held GPU leases.
  void close();
  Statistics statistics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Provider {
 public:
  virtual ~Provider() = default;
  // Nonblocking; never catches up with a burst of old synthetic frames.
  virtual bool poll(std::uint64_t now_ns, Queue&) = 0;
  virtual std::optional<std::string> failure() const { return std::nullopt; }
  // Asynchronous providers stop on their own worker. Presentation owners
  // withdraw immediately, retain at most one retiring provider, and destroy it
  // only after stopped(). Final process teardown may still wait for a driver.
  virtual void request_stop() noexcept {}
  virtual bool stopped() const noexcept { return true; }
};
class SyntheticProvider final : public Provider {
 public:
  explicit SyntheticProvider(Configuration);
  bool poll(std::uint64_t now_ns, Queue&) override;

 private:
  Configuration configuration_;
  std::optional<std::uint64_t> last_poll_, next_due_;
  std::uint64_t ordinal_ = 0;
};
std::unique_ptr<Provider> make_provider(const Configuration&, Queue&);

struct SessionState {
  bool active = false;
  std::optional<std::string> previous_view;
  Availability availability = Availability::missing;
};
// Local main-view ownership only, with no dependency on a connected phone.
// This does not send Ultra UI Sync requests or change a display automatically.
// The display owner must explicitly render unavailable/stale states and use
// the saved view returned by leave() to restore its previous presentation.
class Session {
 public:
  explicit Session(Configuration);
  bool enter(std::string previous_view);
  std::optional<std::string> leave();
  SessionState turn(std::uint64_t now_ns);
  Queue& frames() { return frames_; }

 private:
  Configuration configuration_;
  Queue frames_;
  std::unique_ptr<Provider> provider_;
  std::optional<std::string> previous_view_;
};
}  // namespace headunit::camera
