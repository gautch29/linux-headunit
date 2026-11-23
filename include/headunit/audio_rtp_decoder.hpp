// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/audio_observation.hpp"

#include "headunit/audio_mixer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace headunit {

struct AudioJitterDiagnostics {
  // GStreamer's receiver classifications, not verified physical network loss.
  std::uint64_t pushed = 0, considered_lost = 0, late = 0, duplicates = 0, average_jitter_ns = 0;
};
struct AudioDecoderDiagnostics {
  std::uint64_t submitted = 0, invalid_packets = 0, input_backpressure = 0, input_failures = 0;
  std::uint64_t discarded_pcm_frames = 0, invalid_pcm_buffers = 0;
  std::optional<AudioJitterDiagnostics> jitter{};
  // A buffer is not a packet, PCM block or playback receipt. Older appsink
  // versions do not expose this property; unavailable must not become zero.
  std::optional<std::uint64_t> appsink_dropped_buffers{};
};

// Decodes advertised AAC-LC, Opus and network-order PCM to 48 kHz stereo PCM
// blocks. Network authentication and packet replay checks happen upstream.
// submit/take poll at most 32 real bus messages without waiting. A decoder error
// or unexpected EOS is terminal and is retained by every later owner call.
class AudioRtpDecoder {
 public:
  enum class Format : std::uint64_t {
    pcm_8000_mono = 1 << 2,
    pcm_8000_stereo = 1 << 3,
    pcm_16000_mono = 1 << 4,
    pcm_16000_stereo = 1 << 5,
    pcm_24000_mono = 1 << 6,
    pcm_24000_stereo = 1 << 7,
    pcm_32000_mono = 1 << 8,
    pcm_32000_stereo = 1 << 9,
    pcm_48000_mono = 1 << 14,
    pcm_48000_stereo = 1 << 15,
    aac_lc_48000_stereo = 1 << 23,
    opus_16000_mono = 1 << 28,
    opus_24000_mono = 1 << 29,
    opus_48000_mono = 1 << 30
  };
  enum class Encoding { pcm, opus, aac };
  struct Description {
    Encoding encoding;
    unsigned rate, channels;
  };
  static Description describe(Format);
  explicit AudioRtpDecoder(unsigned latency_ms, Format format = Format::aac_lc_48000_stereo,
                           audio_observation::Point observation = {});
  ~AudioRtpDecoder();
  AudioRtpDecoder(const AudioRtpDecoder&) = delete;
  AudioRtpDecoder& operator=(const AudioRtpDecoder&) = delete;

  bool submit(std::span<const std::uint8_t> rtp);
  std::vector<AudioMixer::Block> take(std::size_t maximum = 8);
  // Query only on the decoder owner thread; retained numeric diagnostics remain
  // readable after a terminal bus fault. MediaStream caches this for control.
  AudioDecoderDiagnostics diagnostics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace headunit
