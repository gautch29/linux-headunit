// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace headunit {

// One 10 ms, 48 kHz stereo PCM block. Phone decoders and guidance sources must
// align their own timestamps before calling this mixer; no network transport or
// audio device is owned here. Empty input is silence on that bus.
class AudioMixer {
 public:
  static constexpr unsigned sample_rate = 48000, channels = 2, frames_per_block = 480;
  using Block = std::array<std::int16_t, frames_per_block * channels>;

  void set_master_volume(double value);
  void set_muted(bool value) { muted_ = value; }
  void set_guidance_gain(double value);
  // Phone focus is separate from the user's master gain/mute. Duration is in
  // milliseconds of accepted PCM; repeated targets never restart a ramp.
  void set_phone_duck(double gain, double duration_ms);
  double phone_duck_gain() const { return phone_gain_; }
  // Independent mainAudio permission; never changes user mute or duck targets.
  void set_media_resource_enabled(bool value) { media_resource_enabled_ = value; }
  void reset();  // New phone/session: clear ducking and old control state.
  Block mix(std::span<const std::int16_t> media, std::span<const std::int16_t> guidance,
            bool guidance_active);

 private:
  double volume_ = 1, guidance_gain_ = 1, duck_gain_ = 1;
  double phone_gain_ = 1, phone_target_ = 1, phone_step_ = 0;
  std::uint64_t phone_frames_ = 0;
  bool muted_ = false, media_resource_enabled_ = true;
};

}  // namespace headunit
