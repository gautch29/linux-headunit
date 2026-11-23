// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

typedef struct _GstElement GstElement;
namespace headunit::theme::graphics {
struct Image;
}

namespace headunit {

// Bounded CPU-copy adapter between a retained decoder and an owned presenter.
// One owner calls release_output(), pull(), closed() and eos() sequentially;
// in particular, pull() must finish before that owner drains/replaces output.
// The adapter never retains a GstSample or a decoded buffer after pull().
class GstPreviewFrames {
 public:
  struct Frame {
    // Original local pipeline arrival PTS, converted through the sample's
    // exact unit-rate TIME segment. Neither phone capture nor presentation.
    std::uint64_t arrival_running_ns = 0;
    std::shared_ptr<const theme::graphics::Image> image;
  };

  // Creates videoconvert ! appsink, with at most three queued decoded samples.
  // Dimensions must be positive and at most 4096 x 2160. Caps negotiation and
  // pull() both enforce these limits, progressive mono/none mode with no
  // multiview flags, and square pixel aspect. Pixels are opaque 0xFFRRGGBB.
  GstPreviewFrames(std::uint32_t max_width, std::uint32_t max_height);
  ~GstPreviewFrames();
  GstPreviewFrames(const GstPreviewFrames&) = delete;
  GstPreviewFrames& operator=(const GstPreviewFrames&) = delete;

  // Transfers the adapter's sole owned bin reference exactly once. Compatible
  // with GstPreviewOutput::start/restore. After transfer only a weak reference
  // to appsink remains, so successful drain can finalize the entire output.
  GstElement* release_output();

  // Nonblocking: consumes at most one pending sample, copying it into a new
  // immutable image. Equal/backward accepted stamps are discarded; timestamps
  // are never synthesized. Invalid timestamp/segment/caps/storage throw.
  // Input before an explicit segment and buffer lists are rejected at the sink
  // pad, before GstBaseSink can invent default segment provenance.
  // Empty optional means no frame or a discarded repeat; consult closed()/eos()
  // separately. Caller retention bounds its returned immutable image leases.
  std::optional<Frame> pull();
  // True only when the weak appsink has disappeared; an empty live queue and
  // EOS do not imply closure. EOS is true after the live queue has drained.
  bool closed() const;
  bool eos() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace headunit
