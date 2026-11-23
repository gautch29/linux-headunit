// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_mixer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
}  // namespace
int main() try {
  headunit::AudioMixer mixer;
  using Block = headunit::AudioMixer::Block;
  const Block music = [] {
    Block out{};
    out.fill(10000);
    return out;
  }();
  const Block prompt = [] {
    Block out{};
    out.fill(5000);
    return out;
  }();
  check(mixer.mix(music, {}, false)[0] == 10000, "music passthrough failed");
  const auto guided = mixer.mix(music, prompt, true);
  check(guided[0] < 15000 && guided.back() <= 7600 && guided.back() >= 7400,
        "guidance did not duck music smoothly");
  const auto released = mixer.mix(music, {}, false);
  check(released[0] >= guided.back() - 5000 && released.back() > released[0] &&
            released.back() < 10000,
        "music did not recover smoothly");
  mixer.set_master_volume(.5);
  mixer.reset();
  mixer.set_master_volume(.5);
  check(mixer.mix(music, {}, false)[0] == 5000, "master volume failed");
  mixer.set_muted(true);
  check(mixer.mix(music, prompt, true)[0] == 0, "mute failed");
  mixer.reset();
  Block loud{};
  loud.fill(30000);
  check(mixer.mix(loud, loud, false)[0] == std::numeric_limits<std::int16_t>::max(),
        "positive mix did not saturate");
  loud.fill(-30000);
  check(mixer.mix(loud, loud, false)[0] == std::numeric_limits<std::int16_t>::min(),
        "negative mix did not saturate");
  bool rejected = false;
  try {
    mixer.set_master_volume(std::numeric_limits<double>::quiet_NaN());
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "NaN gain was accepted");
  rejected = false;
  try {
    (void)mixer.mix(std::span<const std::int16_t>(music.data(), 3), {}, false);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "partial audio block was accepted");
  mixer.reset();
  mixer.set_master_volume(.5);
  mixer.set_phone_duck(.2, 20.);
  const auto first = mixer.mix(music, {}, false);
  check(first.front() < 5000 && first.back() == 3000, "phone duck did not ramp over accepted PCM");
  mixer.set_phone_duck(.2, 60000.);  // Same target must not prolong the in-flight ramp.
  const auto final = mixer.mix(music, {}, false);
  check(final.back() == 1000, "repeated phone target restarted its ramp");
  mixer.set_muted(true);
  mixer.set_phone_duck(1., 10.);
  check(mixer.mix(music, {}, false).back() == 0 && mixer.phone_duck_gain() == 1.,
        "phone release changed user mute or failed to advance accepted silent PCM");
  mixer.set_muted(false);
  check(mixer.mix(music, {}, false).front() == 5000, "phone release changed user master volume");
  mixer.set_phone_duck(0., 0.25);  // Twelve genuine frames, not one rounded millisecond.
  const auto fractional = mixer.mix(music, {}, false);
  check(fractional[0] > 0 && fractional[22] == 0,
        "fractional millisecond ramp lost source sample units");
  mixer.reset();
  check(mixer.mix(music, {}, false).front() == 10000, "new mixer retained prior phone focus");
  mixer.set_master_volume(.5);
  mixer.set_phone_duck(.2, 0);
  mixer.set_media_resource_enabled(false);
  const auto denied = mixer.mix(music, prompt, false);
  check(std::all_of(denied.begin(), denied.end(), [](auto sample) { return sample == 2500; }),
        "mainAudio withdrawal muted alternate guidance or retained media");
  mixer.set_phone_duck(1., 0);
  check(mixer.mix(music, {}, false).back() == 0, "unduck bypassed mainAudio permission");
  mixer.set_media_resource_enabled(true);
  check(mixer.mix(music, {}, false).back() == 5000,
        "resource return changed user volume or lost duck restoration");
  mixer.set_muted(true);
  mixer.set_media_resource_enabled(false);
  mixer.set_media_resource_enabled(true);
  check(mixer.mix(music, {}, false).back() == 0, "resource return cleared user mute");
  mixer.reset();
  check(mixer.mix(music, {}, false).back() == 10000, "new mixer inherited resource denial");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
