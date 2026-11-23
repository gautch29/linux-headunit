// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace headunit {
// Real ALSA/Pulse capture only. Synthetic appsrc is an explicit, in-process
// fixture endpoint; production configuration cannot select it. No missing
// microphone is replaced by silence. All operations belong to a worker.
class AudioMicrophone {
 public:
  AudioMicrophone(std::string factory, std::string device = {}, unsigned sample_rate = 48000,
                  bool opus = true);
  ~AudioMicrophone();
  AudioMicrophone(const AudioMicrophone&) = delete;
  AudioMicrophone& operator=(const AudioMicrophone&) = delete;
  static void validate_selection(std::string_view factory, std::string_view device);
  struct Packet {
    std::vector<std::uint8_t> bytes;
    // Negotiated PCM sample position from the real capture pipeline PTS.
    // Gaps survive encoder/appsink loss rather than compressing the timeline.
    std::uint32_t sample_time = 0;
  };
  std::optional<Packet> take();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
