// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_playback.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <vector>

namespace headunit {
struct AudioPlayback::Channel {
  audio_observation::Point observation;
  bool media = true, enabled = true, retired = false;
  struct Entry {
    AudioMixer::Block block;
    audio_observation::Point observation;
  };
  std::deque<Entry> queue;
  std::optional<AudioFocusCommand> focus;
};
struct AudioPlayback::Impl {
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::vector<std::shared_ptr<Channel>> channels;
  Status counts;
  std::shared_ptr<audio_control::Mailbox> master;
  audio_observation::Point observation;
  std::jthread worker;
  Impl(std::string factory, std::string device, AudioOutput::Observer observer,
       audio_control::Level initial, audio_observation::Point observed)
      : master(std::make_shared<audio_control::Mailbox>(initial)),
        observation(std::move(observed)) {
    std::promise<void> ready;
    auto result = ready.get_future();
    worker = std::jthread([this, factory = std::move(factory), device = std::move(device),
                           observer = std::move(observer),
                           ready = std::move(ready)](std::stop_token stop) mutable {
      // A diagnostic role for per-thread bench profiling, without changing
      // scheduling policy or making naming success a playback prerequisite.
      (void)pthread_setname_np(pthread_self(), "hu-audio-play");
      bool started = false;
      try {
        AudioOutput output(std::move(factory), std::move(device), std::move(observer), observation);
        AudioMixer mixer;
        const auto initial = master->snapshot().applied.level;
        mixer.set_master_volume(initial.volume_percent / 100.);
        mixer.set_muted(initial.muted);
        master->open();
        ready.set_value();
        started = true;
        while (!stop.stop_requested()) {
          output.poll();
          std::unique_lock lock(mutex);
          if (const auto command = master->next()) {
            mixer.set_master_volume(command->level.volume_percent / 100.);
            mixer.set_muted(command->level.muted);
            master->applied(*command);
          }
          for (const auto& channel : channels) {
            if (channel->media && channel->focus) {
              mixer.set_phone_duck(channel->focus->gain, channel->focus->duration_ms);
              channel->focus.reset();
            }
          }
          // Refill a bounded burst before sleeping. A producer notification may
          // precede wait_for(); queued PCM must not then incur one sleep/block.
          // Fixed channel arrays avoid heap allocation on every retry turn.
          for (unsigned turn = 0; turn < 8; ++turn) {
            // Peek without consuming until the sink accepts the mix. Failed
            // attempts do not advance envelopes or erase any source PCM.
            std::array<Channel*, 3> media{};
            std::size_t media_count = 0;
            AudioMixer::Block media_mix{};
            std::array<Channel*, 3> voices{};
            std::size_t voice_count = 0;
            AudioMixer::Block voice_mix{};
            for (const auto& channel : channels) {
              if (channel->retired || channel->queue.empty()) continue;
              if (channel->media) {
                media[media_count++] = channel.get();
                if (channel->enabled)
                  for (std::size_t i = 0; i < media_mix.size(); ++i)
                    media_mix[i] = static_cast<std::int16_t>(std::clamp(
                        int(media_mix[i]) + int(channel->queue.front().block[i]), -32768, 32767));
              } else {
                voices[voice_count++] = channel.get();
                if (channel->enabled)
                  for (std::size_t i = 0; i < voice_mix.size(); ++i)
                    voice_mix[i] = static_cast<std::int16_t>(std::clamp(
                        int(voice_mix[i]) + int(channel->queue.front().block[i]), -32768, 32767));
              }
            }
            if (media_count == 0 && voice_count == 0) break;
            auto candidate = mixer;
            candidate.set_media_resource_enabled(true);
            const auto mixed = candidate.mix(
                media_count != 0 ? std::span(media_mix) : std::span<const std::int16_t>{},
                voice_count != 0 ? std::span(voice_mix) : std::span<const std::int16_t>{}, false);
            if (!output.submit(mixed)) break;
            for (std::size_t i = 0; i < media_count; ++i) {
              const auto point = media[i]->queue.front().observation;
              media[i]->queue.pop_front();
              point.observe(audio_observation::Kind::channel_pop, media[i]->queue.size(),
                            output.accepted());
            }
            for (std::size_t i = 0; i < voice_count; ++i) {
              const auto point = voices[i]->queue.front().observation;
              voices[i]->queue.pop_front();
              point.observe(audio_observation::Kind::channel_pop, voices[i]->queue.size(),
                            output.accepted());
            }
            mixer = candidate;
            counts.duck_gain = mixer.phone_duck_gain();
          }
          counts.accepted = output.accepted();
          counts.discontinuities = output.discontinuities();
          counts.underruns = output.underruns();
          counts.starved = output.starved();
          master->observe_output({audio_control::monotonic_time_ns(), counts.accepted,
                                  counts.discontinuities, counts.underruns, counts.starved,
                                  output.timing()});

          wake.wait_for(lock, std::chrono::milliseconds(2));
        }
      } catch (const std::exception& error) {
        if (!started) ready.set_exception(std::current_exception());
        std::lock_guard lock(mutex);
        counts.fault = error.what();
      }
      std::lock_guard lock(mutex);
      if (started) master->close();
      wake.notify_all();
    });
    if (result.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
      throw std::runtime_error("shared audio output startup timed out");
    result.get();
  }
  ~Impl() {
    worker.request_stop();
    wake.notify_all();
    if (worker.joinable()) worker.join();
  }
};
AudioPlayback::AudioPlayback(std::string factory, std::string device,
                             AudioOutput::Observer observer, audio_control::Level initial_level,
                             audio_observation::Point observation)
    : impl_(std::make_unique<Impl>(std::move(factory), std::move(device), std::move(observer),
                                   initial_level, std::move(observation))) {}
AudioPlayback::~AudioPlayback() = default;

std::shared_ptr<AudioPlayback::Channel> AudioPlayback::attach(
    bool media, audio_observation::Point observation) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->counts.fault.empty()) throw std::runtime_error(impl_->counts.fault);
  if (impl_->channels.size() >= 3) throw std::runtime_error("shared audio channel limit reached");
  auto channel = std::make_shared<Channel>();
  channel->media = media;
  channel->observation = std::move(observation);
  impl_->channels.push_back(channel);
  return channel;
}
void AudioPlayback::detach(const std::shared_ptr<Channel>& channel) {
  if (!channel) return;
  std::lock_guard lock(impl_->mutex);
  channel->observation.observe(audio_observation::Kind::channel_detach, channel->queue.size(),
                               impl_->counts.accepted, audio_observation::unavailable,
                               audio_observation::unavailable, true);
  channel->retired = true;
  channel->queue.clear();
  std::erase(impl_->channels, channel);
}

bool AudioPlayback::submit(const std::shared_ptr<Channel>& channel, const AudioMixer::Block& block,
                           bool enabled, const std::optional<AudioFocusCommand>& focus,
                           audio_observation::Point observation) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->counts.fault.empty()) throw std::runtime_error(impl_->counts.fault);
  if (!channel || channel->retired) throw std::runtime_error("audio channel retired");
  if (observation.capture) channel->observation = std::move(observation);
  channel->enabled = enabled;
  if (focus) channel->focus = focus;
  if (channel->queue.size() >= 8) {
    channel->observation.observe(audio_observation::Kind::supply_blocked, channel->queue.size());
    return false;
  }
  channel->queue.push_back({block, channel->observation});
  channel->observation.observe(audio_observation::Kind::supply, channel->queue.size(), enabled);
  impl_->wake.notify_all();
  return true;
}
void AudioPlayback::configure(const std::shared_ptr<Channel>& channel, bool enabled,
                              const std::optional<AudioFocusCommand>& focus) {
  std::lock_guard lock(impl_->mutex);
  if (!channel || channel->retired) return;
  channel->enabled = enabled;
  if (focus) channel->focus = focus;
}

std::shared_ptr<audio_control::Mailbox> AudioPlayback::audio_control() const {
  return impl_->master;
}

AudioPlayback::Status AudioPlayback::status() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->counts;
}
}  // namespace headunit
