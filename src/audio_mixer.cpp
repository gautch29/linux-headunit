// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_mixer.hpp>
#include <headunit/audio_focus_command.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace headunit {
namespace {
void check_gain(double gain) {
  if (!std::isfinite(gain) || gain < 0 || gain > 1)
    throw std::invalid_argument("audio gain must be finite and between zero and one");
}
void check_block(std::span<const std::int16_t> block) {
  if (!block.empty() && block.size() != AudioMixer::frames_per_block * AudioMixer::channels)
    throw std::invalid_argument("audio bus must contain one complete stereo block");
}
}  // namespace
void AudioMixer::set_master_volume(double value) {
  check_gain(value);
  volume_ = value;
}
void AudioMixer::set_guidance_gain(double value) {
  check_gain(value);
  guidance_gain_ = value;
}
void AudioMixer::set_phone_duck(double gain, double duration_ms) {
  validate_audio_focus_command({gain, duration_ms});
  if (gain == phone_target_) return;
  phone_target_ = gain;
  phone_frames_ = std::uint64_t(std::ceil(duration_ms * sample_rate / 1000.));
  if (!phone_frames_) {
    phone_gain_ = gain;
    phone_step_ = 0;
  } else
    phone_step_ = (phone_target_ - phone_gain_) / double(phone_frames_);
}
void AudioMixer::reset() {
  volume_ = guidance_gain_ = duck_gain_ = phone_gain_ = phone_target_ = 1;
  phone_step_ = 0;
  phone_frames_ = 0;
  muted_ = false;
  media_resource_enabled_ = true;
}
AudioMixer::Block AudioMixer::mix(std::span<const std::int16_t> media,
                                  std::span<const std::int16_t> guidance, bool guidance_active) {
  check_block(media);
  check_block(guidance);
  Block result{};
  const double target = guidance_active ? .25 : 1.;
  // Ease into ducking over ~5 ms and release over ~100 ms so consecutive
  // navigation prompts do not abruptly change music level.
  const double rate = guidance_active ? 1. / 240 : 1. / 4800;
  for (unsigned frame = 0; frame < frames_per_block; ++frame) {
    duck_gain_ += std::clamp(target - duck_gain_, -rate, rate);
    if (phone_frames_) {
      phone_gain_ += phone_step_;
      if (!--phone_frames_) phone_gain_ = phone_target_;
    }
    for (unsigned channel = 0; channel < channels; ++channel) {
      const auto index = frame * channels + channel;
      const double mixed =
          (media.empty() || !media_resource_enabled_ ? 0
                                                     : media[index] * duck_gain_ * phone_gain_) +
          (guidance.empty() ? 0 : guidance[index] * guidance_gain_);
      const auto scaled = muted_ ? 0. : mixed * volume_;
      result[index] = std::int16_t(std::clamp(std::lround(scaled),
                                              long(std::numeric_limits<std::int16_t>::min()),
                                              long(std::numeric_limits<std::int16_t>::max())));
    }
  }
  return result;
}
}  // namespace headunit
