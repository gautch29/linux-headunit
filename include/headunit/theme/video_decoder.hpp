// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/scene.hpp>
#include <headunit/airplay_video_stream.hpp>
#include <headunit/theme/screen_configuration.hpp>
#include <headunit/theme/native_frame.hpp>
#include <memory>
#include <string>
#include <array>

namespace headunit::theme::video {
struct DecodedFrame {
  std::uint64_t sender_ntp;    // Exact authenticated source bits, not decoder uptime.
  std::uint64_t timestamp_ns;  // Same sender clock, converted to GStreamer units.
  graphics::Image image;       // CPU reference import; not zero-copy GPU composition.
};
struct DecoderOptions {
  Codec codec = Codec::h264;
  Bytes codec_data;  // Bare avcC/hvcC record, from the negotiated screen configuration.
  std::uint32_t max_width = 4096, max_height = 2160;
  // Trusted host policy only. Empty chooses a registered hardware decoder first.
  std::string decoder;
  // Diagnostic sink only: decode real frames but consume caps/PTS without
  // colour conversion or CPU pixel mapping. This produces no renderable image.
  bool metadata_only = false;
  bool native_buffers = false;     // Mutually exclusive; requires genuine DMA-buf output.
  unsigned max_native_leases = 3;  // Trusted bounded owner policy: 1..4.
};
// One owner thread. GStreamer workers are internal. Nonblocking bounded input
// and output; the session owner supplies the socket/decoder stall deadline.
// A codec/clock discontinuity requires replacing this object and its queues.
class Decoder {
 public:
  explicit Decoder(DecoderOptions);
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  bool push(const AirplayVideoPacket&);  // false = backpressure; caller retains this frame.
  std::optional<DecodedFrame> pull();
  std::optional<DecodedMetadata> pull_metadata();  // Requires metadata_only.
  // Returns empty at the configured native sample limit (three by default).
  // No CPU mapping, colour conversion or format/modifier guessing.
  std::shared_ptr<const NativeFrame> pull_native();
  void finish();
  bool drained();
  std::string decoder_name() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::video
