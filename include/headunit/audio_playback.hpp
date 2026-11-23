// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/audio_observation.hpp"
#include "headunit/audio_control.hpp"
#include "headunit/audio_focus_command.hpp"
#include "headunit/audio_output.hpp"
#include <memory>
#include <string>

namespace headunit {
// A session has one sink and receiver-clock schedule. Independent phone RTP
// clocks are not claimed to be synchronized; decoded blocks join this bounded
// local schedule. Device work runs on the playback worker, never control/UI.
class AudioPlayback {
 public:
  struct Channel;
  struct Status {
    std::uint64_t accepted = 0, discontinuities = 0, underruns = 0;
    bool starved = false;
    double duck_gain = 1.;
    std::string fault;
  };
  AudioPlayback(std::string factory, std::string device, AudioOutput::Observer observer = {},
                audio_control::Level initial_level = {}, audio_observation::Point observation = {});
  ~AudioPlayback();
  AudioPlayback(const AudioPlayback&) = delete;
  AudioPlayback& operator=(const AudioPlayback&) = delete;
  std::shared_ptr<Channel> attach(bool media, audio_observation::Point observation = {});
  void detach(const std::shared_ptr<Channel>&);
  bool submit(const std::shared_ptr<Channel>&, const AudioMixer::Block&, bool enabled,
              const std::optional<AudioFocusCommand>& focus,
              audio_observation::Point observation = {});
  void configure(const std::shared_ptr<Channel>&, bool enabled,
                 const std::optional<AudioFocusCommand>&);
  std::shared_ptr<audio_control::Mailbox> audio_control() const;
  Status status() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
