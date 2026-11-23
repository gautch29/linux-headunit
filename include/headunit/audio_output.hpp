// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/audio_observation.hpp"
#include <headunit/audio_mixer.hpp>
#include <headunit/audio_control.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace headunit {

// Linux GStreamer output for already-decoded, mixed 48 kHz stereo PCM.
// Source and guidance clocks must be aligned before submission. The caller
// never runs this from a display or CarPlay control thread.
class AudioOutput {
 public:
  struct Receipt {
    std::uint64_t presentation_ns = 0, duration_ns = 0;
    std::size_t bytes = 0;
    bool discontinuity = false;
    // Optional diagnostic for one bounded S16LE stereo block at the sink pad.
    // No PCM bytes are retained; other negotiated formats leave it unavailable.
    std::optional<unsigned> peak_sample{};
  };
  // Optional bounded diagnostic called on GStreamer's streaming thread when a
  // PCM buffer reaches the sink. This is not a speaker/presentation receipt.
  // It must not block, throw, or call back into AudioOutput.
  using Observer = std::function<void(const Receipt&)>;
  explicit AudioOutput(std::string sink_factory, std::string device = {}, Observer observer = {},
                       audio_observation::Point observation = {});
  static void validate_selection(std::string_view sink_factory, std::string_view device);
  ~AudioOutput();
  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;
  // False means the bounded queue is full or the sink is stopping. The caller
  // retains one pending source block and retries outside the control receiver.
  bool submit(const AudioMixer::Block& block);
  // Owner-only, bounded and nonblocking. Check the actual pipeline bus even
  // while paused/starved; output/clock faults are sticky and throw. No PCM is
  // consumed, scheduled or presented by a poll.
  void poll();
  void finish();  // Offline/test drain; ordinary shutdown may simply destroy.
  std::uint64_t accepted() const;
  std::uint64_t rejected() const;
  std::uint64_t discontinuities() const;
  // Local accepted PCM schedule exhausted after playback began. One count per
  // gap, including an intentional phone pause; not an ALSA/device underrun or
  // network-loss measurement. Owner poll detects gaps without another submit.
  std::uint64_t underruns() const;
  bool starved() const;
  // Copied numeric state from the latest owner poll/submit; no clock or I/O query.
  audio_control::OutputTiming timing() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace headunit
