// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/display_video_input.hpp>
#include <headunit/airplay_video_stream.hpp>
#include <headunit/theme/touch_response_probe.hpp>
#include <memory>

namespace headunit::theme::video {
// One numeric snapshot, with no decoder/GPU backing retained. Metadata is
// copied from the exact accepted current native frame; CPU fixtures have none.
struct NativeInputPresentation {
  std::uint64_t stream_token = 0, owner_generation = 0;
  std::optional<std::uint64_t> owner_connection_id;
  double source_pts = 0;
  std::optional<DecodedMetadata> decoded;
  InputPresentationReceipt receipt;
};
// Display-owner adoption and ingress state are distinct. A replacement or
// retirement may already be in ingress while old decoder receipts are pending.
// IDs are the actual session-reserved authenticated transport IDs; fixtures
// without a transport owner leave them unavailable (including valid ID zero).
struct NativeInputIdentity {
  std::uint64_t owner_generation = 0, ingress_generation = 0;
  std::optional<std::uint64_t> owner_connection_id, ingress_connection_id;
  bool available = false, closed = false, owner_current = false;
  std::uint64_t observed_ns = 0;  // Actual host steady sample under the ingress lock.
};
struct NativeInputStats {
  std::uint64_t submitted = 0, decoded = 0, late = 0, superseded = 0, presented = 0;
  std::uint64_t discarded = 0, clock_dropped = 0, non_monotonic = 0;
  std::uint64_t max_staged = 0, backpressure = 0;
  std::uint64_t generation_changes = 0, generation_dropped = 0;
  // Decoded-frame lead: how far each scheduled frame's PTS was ahead of the
  // host's media time when decoding finished. Positive lead is time a frame
  // waits before it may be staged (less any presentation advance).
  std::uint64_t lead_samples = 0;
  std::int64_t lead_sum_ns = 0, min_lead_ns = 0, max_lead_ns = 0;
  TouchResponseProbe::Stats touch_response;
};
// Shared transport endpoint contains packets only: no VM, GPU or decoder
// resources. The receiver can safely retain it after the display worker exits.
class NativeInputQueue {
 public:
  static constexpr std::size_t max_packets = 64, max_bytes = 8 * 1024 * 1024,
                               max_packet_bytes = 2 * 1024 * 1024;
  NativeInputQueue();
  ~NativeInputQueue();
  NativeInputQueue(const NativeInputQueue&) = delete;
  NativeInputQueue& operator=(const NativeInputQueue&) = delete;
  // Thread-safe. Moves only on success; false leaves the packet intact. The
  // connection owner retains/retries with bounded TCP backpressure, never drops
  // compressed deltas. Invalid packets or a closed generation throw.
  bool enqueue(AirplayVideoPacket&);
  // The authenticated transport owner supplies fresh, session-reserved IDs.
  // A retired ID cannot be revived. Replacement clears only compressed ingress;
  // the display owner observes the change and replaces its decoder off-thread.
  void begin_generation(std::uint64_t connection_id);
  void retire_generation(std::uint64_t connection_id);
  bool available() const;
  void close();
  bool closed() const;

 private:
  friend class NativeInput;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// One stable Runtime stream, with replaceable authenticated decoder generations
// entirely on the display owner thread. Destruction closes endpoint and stream.
class NativeInput final : public DisplayVideoInput {
 public:
  // presentation_advance_ns lets a decoded frame be staged that long before
  // its PTS is due. Zero keeps strict PTS staging. The frame keeps its genuine
  // PTS and presentation receipts still report actual presentation time.
  static constexpr std::uint64_t max_presentation_advance_ns = 100000000;
  NativeInput(std::shared_ptr<Stream>, std::shared_ptr<NativeInputQueue>, std::string decoder = {},
              std::uint64_t presentation_advance_ns = 0);
  ~NativeInput();
  NativeInput(const NativeInput&) = delete;
  NativeInput& operator=(const NativeInput&) = delete;
  const std::shared_ptr<Stream>& stream() const override;
  void pump(const MediaClock*) override;
  std::optional<std::uint64_t> stage(const MediaClock&) override;
  void complete(std::uint64_t, bool) override;
  void complete(std::uint64_t, bool, const InputPresentationReceipt*) override;
  void suspend() override;
  bool composition_fresh() const override;
  NativeInputIdentity identity() const;  // Display owner only; ingress copied under its lock.
  // Owner thread. Null after flush, replacement, suspension or retirement.
  // Repeated reads preserve original dates; redraws never renew the receipt.
  std::optional<NativeInputPresentation> presentation() const;
  NativeInputStats stats() const;  // Display owner only.
  // Display owner only; same bound as the constructor argument.
  void set_presentation_advance(std::uint64_t presentation_advance_ns);
  // Display owner only: a finger-down on this panel was forwarded to the phone.
  void touch_pressed(std::uint64_t monotonic_ns);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::video
