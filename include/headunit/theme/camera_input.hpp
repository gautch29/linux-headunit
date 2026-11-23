// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/camera.hpp>
#include <headunit/theme/display_video_input.hpp>
#include <functional>
#include <array>
#include <ostream>

namespace headunit::theme::video {
struct CameraInputStatistics {
  std::uint64_t staged = 0, presented = 0, discarded = 0;
  std::uint64_t unmapped = 0, non_monotonic = 0, backpressure = 0, expired = 0;
};
// Numeric copy of the exact source selected by Stream::composition_token().
// No pixel/native lease is retained. A prepared source is not a presentation;
// the accepted receipt observation is present only after complete(token,true).
struct CameraConsumption {
  std::uint64_t source_generation = 0, lease_sequence = 0, stream_token = 0;
  std::uint64_t capture_monotonic_ns = 0, prepared_monotonic_ns = 0, clock_epoch = 0;
  std::optional<std::uint64_t> receipt_observed_monotonic_ns;
  std::optional<camera::CaptureReference> capture_reference;
  std::optional<camera::CaptureAcquisition> acquisition;
  bool simulated = false, native = false;
  unsigned width = 0, height = 0, storage_width = 0, storage_height = 0;
  unsigned crop_x = 0, crop_y = 0, plane_count = 0;
  std::array<std::uint32_t, 4> stride_bytes{};
  // Registered local input bounds, NOT signed scene or physical window bounds.
  unsigned input_width = 0, input_height = 0;
  void write_json(std::ostream&) const;
};
// The Ultra owner maps LOCAL capture times through an already qualified phone
// clock. No clock is created here. Without that clock, the independent native
// camera display is used instead; an Ultra scene never gains eligibility.
// Widget/settings/road-sign sources are explicit EXTERNAL queues. They share
// acquisition-age, qualified-clock, lease and real-receipt rules with cameras;
// their producer runs separately and may never refresh dates on a redraw.
class LocalInput final : public DisplayVideoInput {
 public:
  using TimeSource = std::function<std::uint64_t()>;
  using VisibilitySink = std::function<void(bool, std::uint64_t, std::uint64_t)>;
  LocalInput(camera::Configuration, std::shared_ptr<Stream>, std::shared_ptr<camera::Queue>,
             TimeSource = {}, VisibilitySink = {}, InputType = InputType::camera);
  ~LocalInput() override;
  LocalInput(const LocalInput&) = delete;
  LocalInput& operator=(const LocalInput&) = delete;
  void set_active(bool);  // Display owner only. Deactivation withdraws immediately.
  const std::shared_ptr<Stream>& stream() const override;
  void pump(const MediaClock*) override;
  std::optional<std::uint64_t> stage(const MediaClock&) override;
  void complete(std::uint64_t token, bool presented) override;
  void suspend() override;
  bool composition_fresh() const override;
  // The current source generation, including after explicit fault recovery.
  camera::Availability availability() const;
  CameraInputStatistics statistics() const;
  // Owner-thread snapshot. Unselected, expired, retired or terminal frames are
  // unavailable even before the next pump; newer queued metadata cannot renew
  // an older composition. Dates remain original and reads do not mint receipts.
  std::optional<CameraConsumption> consumption() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
using CameraInput = LocalInput;
}  // namespace headunit::theme::video
