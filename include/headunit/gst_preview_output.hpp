// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

typedef struct _GstElement GstElement;

namespace headunit {

// Replaces only a decoded-video output downstream of a closed GStreamer valve.
// The borrowed, PLAYING pipeline and its parser/decoder remain unchanged. One
// owner calls these methods; the streaming thread applies the freshness gate.
// Construct before streaming. Before destruction, the owner must synchronously
// stop the pipeline at NULL, then destroy this object before releasing it.
// Removing a pad probe alone does not wait for an in-flight streaming callback.
class GstPreviewOutput {
 public:
  struct Stats {
    std::uint64_t generation = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t rejected = 0;
    bool draining = true;
    bool barrier_retired = false;
    bool decoder_epoch_retired = false;
    // Exact original running-time boundary of the installed restore. Initial
    // start/drain has no floor. This is not a presentation or phone clock.
    std::optional<std::uint64_t> arrival_floor;
  };

  // The named valve must be the last element in the pipeline. Construction
  // installs an unclocked drain sink, so hidden decoding still receives EOS.
  GstPreviewOutput(GstElement* pipeline, GstElement* valve);
  ~GstPreviewOutput();
  GstPreviewOutput(const GstPreviewOutput&) = delete;
  GstPreviewOutput& operator=(const GstPreviewOutput&) = delete;

  // Initial output, before PLAYING. Takes ownership of the supplied element or
  // bin. It must expose an unlinked sink pad; async output sinks should opt out
  // of preroll. There is no retained-video freshness claim at initial startup.
  void start(GstElement* output);
  // Remove/finalize the old output and install a permanent drain. Only after
  // this returns may a caller destroy that output's borrowed Wayland parent.
  void drain();
  // Takes ownership of a NEW output, without a cached frame. Rejects decoded
  // buffers with missing/old running-time PTS before they can enter its cache.
  // Requires a PLAYING pipeline and an observed TIME segment/stream identity.
  // Clock/base-time/stream/segment changes permanently retire its decoder epoch.
  // Draining does not reset that authority; return then needs a new decoder.
  void restore(GstElement* output);
  Stats stats() const;
  // Nonblocking bootstrap predictor, not restoration authority. False while
  // PLAYING/stream/segment identity is not observed, or the epoch is retired.
  // restore() still validates its actual clock and publishes its exact floor.
  bool restore_ready() const;
  // Genuine local pipeline running time for appsrc arrival stamping BEFORE
  // queueing encoded bytes. This is not the phone's acquisition/presentation
  // timestamp and must never be used to qualify an Ultra clock or receipt.
  std::uint64_t arrival_time() const;

  // A buffer passing this gate is not a presentation receipt. A surface/lease
  // owner must separately qualify actual source-correlated presentation before
  // restoring touch or claiming that fresh video is visible.
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace headunit
