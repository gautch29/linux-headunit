// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/carplay_media_stream.hpp"

#include "headunit/audio_mixer.hpp"
#include "headunit/audio_microphone.hpp"
#include "headunit/audio_output.hpp"
#include "headunit/audio_rtp_decoder.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <optional>
#include <limits>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>

namespace headunit {
namespace {
void increment(std::uint64_t& value) {
  if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

std::pair<int, std::uint16_t> bind_udp() {
  int flags = SOCK_DGRAM;
#ifdef SOCK_CLOEXEC
  flags |= SOCK_CLOEXEC | SOCK_NONBLOCK;
#endif
  const int fd = socket(AF_INET6, flags, 0);
  if (fd < 0) throw std::runtime_error("cannot create audio UDP port");
#ifndef SOCK_CLOEXEC
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
    close(fd);
    throw std::runtime_error("cannot configure audio UDP port");
  }
#endif
  int ipv6_only = 0;
  sockaddr_in6 local{};
  local.sin6_family = AF_INET6;
  local.sin6_addr = in6addr_any;
  socklen_t length = sizeof(local);
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6_only, sizeof(ipv6_only)) ||
      bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) ||
      getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length)) {
    close(fd);
    throw std::runtime_error("cannot bind audio UDP port");
  }
  return {fd, ntohs(local.sin6_port)};
}

bool same_phone(const sockaddr_in6& expected, const sockaddr_in6& actual) {
  return actual.sin6_family == AF_INET6 &&
         std::memcmp(&expected.sin6_addr, &actual.sin6_addr, sizeof(expected.sin6_addr)) == 0;
}

}  // namespace

CarplayMediaStream::CarplayMediaStream(std::span<const std::uint8_t> secret,
                                       std::uint64_t connection_id, const sockaddr_in6& phone,
                                       std::string sink_factory, unsigned latency_ms,
                                       std::string device, audio_control::Level initial_level,
                                       AudioOutput::Observer output_observer,
                                       bool media_resource_enabled, CarplayAudioOptions options)
    : packets_(secret, connection_id),
      options_(std::move(options)),
      phone_(phone),
      controls_(options_.playback ? options_.playback->audio_control()
                                  : std::make_shared<audio_control::Mailbox>(initial_level)),
      output_observer_(std::move(output_observer)) {
  if (options_.observation.capture) {
    if (!options_.observation.source)
      options_.observation.source = options_.observation.capture->identity();
    if (!options_.observation.route)
      options_.observation.route = options_.observation.capture->identity();
    options_.observation.epoch = 1;
  }
  options_.observation.epoch = 0;
  status_.voice = !options_.media;
  status_.microphone_requested = options_.microphone_port != 0;
  if (options_.microphone_port) {
    const auto microphone_format = AudioRtpDecoder::describe(options_.format);
    if (options_.media || microphone_format.encoding == AudioRtpDecoder::Encoding::aac ||
        microphone_format.channels != 1)
      throw std::invalid_argument("microphone requires Opus main audio");
    microphone_packets_ = std::make_unique<AirplayMicrophonePackets>(
        secret, connection_id,
        microphone_format.rate /
            (microphone_format.encoding == AudioRtpDecoder::Encoding::opus ? 50 : 100));
  }
  status_.media_resource_enabled = media_resource_enabled;
  if (phone.sin6_family != AF_INET6)
    throw std::invalid_argument("audio peer must be an IPv6-mapped phone");
  const auto data = bind_udp();
  data_fd_ = data.first;
  data_port_ = data.second;
  try {
    const auto control = bind_udp();
    control_fd_ = control.first;
    control_port_ = control.second;
    std::promise<void> ready;
    auto result = ready.get_future();
    worker_ =
        std::jthread([this, sink = std::move(sink_factory), latency_ms, device = std::move(device),
                      ready = std::move(ready)](std::stop_token stopped) mutable {
          run(stopped, std::move(sink), latency_ms, std::move(device), std::move(ready));
        });
    if (result.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
      throw std::runtime_error("audio worker startup timed out");
    result.get();
  } catch (...) {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
    if (control_fd_ >= 0) close(control_fd_);
    if (data_fd_ >= 0) close(data_fd_);
    throw;
  }
}

CarplayMediaStream::~CarplayMediaStream() {
  worker_.request_stop();
  if (worker_.joinable()) worker_.join();
  if (control_fd_ >= 0) close(control_fd_);
  if (data_fd_ >= 0) close(data_fd_);
}

MediaStreamStatus CarplayMediaStream::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}
bool CarplayMediaStream::request_audio_focus(const AudioFocusCommand& command) {
  validate_audio_focus_command(command);
  std::lock_guard lock(mutex_);
  if (!options_.media || !status_.fault.empty() || !controls_->snapshot().available) return false;
  if (focus_ && focus_->gain == command.gain) return true;  // Do not restart an existing target.
  if (status_.focus_requested == std::numeric_limits<std::uint64_t>::max()) return false;
  focus_ = command;
  ++status_.focus_requested;
  options_.observation.observe(audio_observation::Kind::focus_requested, status_.focus_requested,
                               std::uint64_t(command.gain * 1000000),
                               std::uint64_t(command.duration_ms * 1000000),
                               audio_observation::unavailable, true);
  return true;
}

bool CarplayMediaStream::set_media_resource_enabled(bool enabled) {
  if (options_.alternate) return true;
  std::lock_guard lock(mutex_);
  if (!status_.fault.empty() || !controls_->snapshot().available) return false;
  if (status_.media_resource_enabled == enabled) return true;
  if (status_.resource_requested == std::numeric_limits<std::uint64_t>::max()) return false;
  status_.media_resource_enabled = enabled;
  ++status_.resource_requested;
  options_.observation.observe(audio_observation::Kind::resource_requested, enabled,
                               status_.resource_requested, audio_observation::unavailable,
                               audio_observation::unavailable, true);
  return true;
}

bool CarplayMediaStream::request_flush(std::optional<std::uint16_t> resume_seq) {
  std::lock_guard lock(mutex_);
  if (!status_.fault.empty()) return false;
  options_.observation.observe(audio_observation::Kind::flush_requested,
                               resume_seq.value_or(audio_observation::unavailable),
                               status_.flush_requested + 1, audio_observation::unavailable,
                               audio_observation::unavailable, true);
  flush_ = resume_seq;
  increment(status_.flush_requested);
  return true;
}

void CarplayMediaStream::run(std::stop_token stopped, std::string factory, unsigned latency_ms,
                             std::string device, std::promise<void> ready) {
  (void)pthread_setname_np(pthread_self(), "hu-audio-rx");
  auto observation = options_.observation;
  observation.epoch = 1;
  observation.observe(audio_observation::Kind::stream_start, std::uint64_t(options_.format),
                      latency_ms, options_.media, options_.alternate, true);
  bool started = false;
  bool controls_open = false;
  std::shared_ptr<AudioPlayback::Channel> playback_channel;
  try {
    auto decoder = std::make_unique<AudioRtpDecoder>(latency_ms, options_.format, observation);
    AudioMixer mixer;
    std::unique_ptr<AudioOutput> output;
    if (options_.playback)
      playback_channel = options_.playback->attach(options_.media, observation);
    else
      output = std::make_unique<AudioOutput>(
          std::move(factory), std::move(device), std::move(output_observer_),
          audio_observation::Point{observation.capture, 0, observation.route});
    const auto initial = controls_->snapshot().applied.level;
    mixer.set_master_volume(initial.volume_percent / 100.);
    mixer.set_muted(initial.muted);
    mixer.set_media_resource_enabled(status_.media_resource_enabled);
    observation.observe(audio_observation::Kind::resource, status_.media_resource_enabled, 0,
                        audio_observation::unavailable, audio_observation::unavailable, true);
    if (!playback_channel) {
      controls_->open();
      controls_open = true;
    }
    const auto cache_diagnostics = [&](bool observe_output = true) {
      const auto diagnostics = decoder->diagnostics();
      std::lock_guard lock(mutex_);
      status_.decoder = diagnostics;
      if (output) {
        status_.output_accepted = output->accepted();
        status_.output_discontinuities = output->discontinuities();
        status_.output_underruns = output->underruns();
        status_.output_starved = output->starved();
        if (observe_output)
          controls_->observe_output({audio_control::monotonic_time_ns(), status_.output_accepted,
                                     status_.output_discontinuities, status_.output_underruns,
                                     status_.output_starved, output->timing()});
      } else {
        const auto playback = options_.playback->status();
        status_.output_accepted = playback.accepted;
        status_.output_discontinuities = playback.discontinuities;
        status_.output_underruns = playback.underruns;
        status_.output_starved = playback.starved;
        status_.phone_duck_gain = playback.duck_gain;
      }
    };
    cache_diagnostics();
    std::unique_ptr<AudioMicrophone> microphone;
    if (options_.microphone_port) {
      try {
        if (options_.microphone_source.empty())
          throw std::runtime_error("microphone not configured");
        const auto capture = AudioRtpDecoder::describe(options_.format);
        microphone = std::make_unique<AudioMicrophone>(
            options_.microphone_source, options_.microphone_device, capture.rate,
            capture.encoding == AudioRtpDecoder::Encoding::opus);
        std::lock_guard lock(mutex_);
        status_.microphone_available = true;
      } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        status_.microphone_fault = error.what();
      }
    }
    ready.set_value();
    started = true;
    std::array<std::uint8_t, 4096> bytes{};
    std::optional<AudioMixer::Block> pending;
    std::uint64_t focus_configured = 0;
    // Sequence of the last accepted packet and an unreached FLUSH resume point.
    std::optional<std::uint16_t> last_seq, resume_seq;
    try {
      while (!stopped.stop_requested()) {
        observation.observe(audio_observation::Kind::receive_turn, bool(pending));
        // A failed device must retire availability even without a new decoded
        // block (for example while the phone pauses playback).
        if (output)
          output->poll();
        else {
          const auto playback = options_.playback->status();
          if (!playback.fault.empty()) throw std::runtime_error(playback.fault);
        }
        if (microphone) {
          try {
            for (unsigned i = 0; i < 16; ++i) {
              const auto opus = microphone->take();
              if (!opus) break;
              const auto packet = microphone_packets_->seal(opus->bytes, opus->sample_time);
              auto target = phone_;
              target.sin6_port = htons(options_.microphone_port);
              const auto sent = sendto(data_fd_, packet.data(), packet.size(), 0,
                                       reinterpret_cast<const sockaddr*>(&target), sizeof(target));
              if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                throw std::runtime_error("microphone UDP send failed");
              std::lock_guard lock(mutex_);
              if (sent == static_cast<ssize_t>(packet.size()))
                increment(status_.microphone_sent);
              else
                increment(status_.microphone_dropped);
            }
          } catch (const std::exception& error) {
            microphone.reset();
            std::lock_guard lock(mutex_);
            status_.microphone_available = false;
            status_.microphone_fault = error.what();
          }
        }
        {
          std::lock_guard lock(mutex_);
          mixer.set_media_resource_enabled(status_.media_resource_enabled);
          if (status_.resource_configured != status_.resource_requested)
            observation.observe(audio_observation::Kind::resource, status_.media_resource_enabled,
                                status_.resource_requested, audio_observation::unavailable,
                                audio_observation::unavailable, true);
          status_.resource_configured = status_.resource_requested;
          std::optional<AudioFocusCommand> changed_focus;
          if (focus_ && status_.focus_requested != focus_configured) {
            mixer.set_phone_duck(focus_->gain, focus_->duration_ms);
            changed_focus = focus_;
            focus_configured = status_.focus_requested;
            status_.focus_configured = focus_configured;
            observation.observe(audio_observation::Kind::focus, focus_configured,
                                std::uint64_t(focus_->gain * 1000000),
                                std::uint64_t(focus_->duration_ms * 1000000),
                                audio_observation::unavailable, true);
          }
          if (playback_channel)
            options_.playback->configure(playback_channel, status_.media_resource_enabled,
                                         changed_focus);
        }
        std::optional<std::optional<std::uint16_t>> flush;
        {
          std::lock_guard lock(mutex_);
          flush = std::exchange(flush_, std::nullopt);
        }
        if (flush) {
          // A resume point names one stream: it follows that stream's packets
          // closely. Main and media streams have independent sequence spaces.
          bool applies = !*flush;
          if (*flush && last_seq) {
            const auto ahead = static_cast<std::uint16_t>(**flush - *last_seq);
            applies = ahead <= 2048 || ahead >= 65536 - 256;
          }
          if (applies) {
            observation.observe(audio_observation::Kind::flush_begin,
                                (*flush).value_or(audio_observation::unavailable), bool(pending),
                                last_seq.value_or(audio_observation::unavailable),
                                audio_observation::unavailable, true);
            decoder.reset();
            ++observation.epoch;
            decoder = std::make_unique<AudioRtpDecoder>(latency_ms, options_.format, observation);
            pending.reset();
            resume_seq = *flush;
            observation.observe(audio_observation::Kind::flush_applied,
                                resume_seq.value_or(audio_observation::unavailable),
                                audio_observation::unavailable, audio_observation::unavailable,
                                audio_observation::unavailable, true);
          } else {
            observation.observe(audio_observation::Kind::flush_skipped,
                                (*flush).value_or(audio_observation::unavailable),
                                last_seq.value_or(audio_observation::unavailable),
                                audio_observation::unavailable, audio_observation::unavailable,
                                true);
          }
          std::lock_guard lock(mutex_);
          increment(applies ? status_.flush_applied : status_.flush_skipped);
        }
        if (output)
          if (const auto command = controls_->next()) {
            mixer.set_master_volume(command->level.volume_percent / 100.);
            mixer.set_muted(command->level.muted);
            controls_->applied(*command);  // Configured gain, not a hardware playback receipt.
          }
        pollfd ports[2]{{data_fd_, POLLIN, 0}, {control_fd_, POLLIN, 0}};
        const auto ready = poll(ports, 2, 5);
        if (ready < 0 && errno != EINTR) throw std::runtime_error("audio UDP poll failed");
        if (ready > 0 && (ports[0].revents & POLLIN)) {
          for (unsigned i = 0; i < 64; ++i) {
            sockaddr_in6 sender{};
            iovec storage{bytes.data(), bytes.size()};
            msghdr message{};
            message.msg_name = &sender;
            message.msg_namelen = sizeof(sender);
            message.msg_iov = &storage;
            message.msg_iovlen = 1;
            const auto received = recvmsg(data_fd_, &message, 0);
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (received < 0 && errno == EINTR) continue;
            if (received < 0) throw std::runtime_error("audio UDP read failed");
            if (message.msg_namelen != sizeof(sender) || !same_phone(phone_, sender)) continue;
            observation.observe(audio_observation::Kind::datagram, received, message.msg_flags,
                                received >= 12 ? (std::uint64_t(bytes[2]) << 8) | bytes[3]
                                               : audio_observation::unavailable,
                                received >= 12 ? (std::uint64_t(bytes[4]) << 24) |
                                                     (std::uint64_t(bytes[5]) << 16) |
                                                     (std::uint64_t(bytes[6]) << 8) | bytes[7]
                                               : audio_observation::unavailable);
            {
              std::lock_guard lock(mutex_);
              increment(status_.wire_datagrams);
            }
            // recvfrom can return a valid-looking bounded prefix of a larger
            // datagram. Reject the actual truncated wire packet before decrypting
            // or changing the nonce replay window.
            if ((message.msg_flags & MSG_TRUNC) ||
                static_cast<std::size_t>(received) > bytes.size()) {
              std::lock_guard lock(mutex_);
              increment(status_.rejected);
              increment(status_.truncated_datagrams);
              observation.observe(audio_observation::Kind::truncated, received);
              continue;
            }
            auto packet = packets_.open(std::span(bytes).first(static_cast<std::size_t>(received)));
            if (!packet) {
              observation.observe(audio_observation::Kind::rejected, received);
              std::lock_guard lock(mutex_);
              increment(status_.rejected);
            } else {
              {
                std::lock_guard lock(mutex_);
                increment(status_.authenticated);
              }
              const auto seq = static_cast<std::uint16_t>(
                  packet->bytes.size() >= 4 ? (packet->bytes[2] << 8) | packet->bytes[3] : 0);
              observation.observe(audio_observation::Kind::authenticated, seq, packet->sample_time,
                                  packet->bytes.size());
              if (resume_seq) {
                // Serial-number order: packets before the resume point belong
                // to the flushed audio even when they arrive afterwards.
                if (static_cast<std::uint16_t>(seq - *resume_seq) >= 32768) {
                  std::lock_guard lock(mutex_);
                  increment(status_.flush_discarded_packets);
                  observation.observe(audio_observation::Kind::pre_resume, seq, *resume_seq);
                  continue;
                }
                resume_seq.reset();
              }
              last_seq = seq;
              (void)decoder->submit(packet->bytes);
            }
          }
        }
        if (ready > 0 && (ports[1].revents & POLLIN)) {
          // Keep the control port drained until RTCP feedback/retransmission is
          // implemented. It is intentionally not interpreted as audio samples.
          for (unsigned i = 0; i < 16; ++i) {
            sockaddr_in6 sender{};
            iovec storage{bytes.data(), bytes.size()};
            msghdr message{};
            message.msg_name = &sender;
            message.msg_namelen = sizeof(sender);
            message.msg_iov = &storage;
            message.msg_iovlen = 1;
            const auto count = recvmsg(control_fd_, &message, 0);
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::runtime_error("audio control UDP read failed");
            if (message.msg_namelen != sizeof(sender) || !same_phone(phone_, sender)) continue;
            std::lock_guard lock(mutex_);
            increment(status_.control_datagrams);
            if ((message.msg_flags & MSG_TRUNC) || static_cast<std::size_t>(count) > bytes.size())
              increment(status_.control_truncated_datagrams);
          }
        }
        for (unsigned i = 0; i < 8; ++i) {
          if (!pending) {
            const auto blocks = decoder->take(1);
            if (blocks.empty()) break;
            pending = blocks.front();
            std::lock_guard lock(mutex_);
            increment(status_.decoded_blocks);
          }
          // Retain unmixed PCM under backpressure. A changed volume/mute applies
          // to its next attempt; failed retries must not advance ducking state.
          {
            // Recheck permission for each bounded submission, including retained
            // PCM retries. Already accepted sink buffers retain their <=80 ms
            // schedule; this is not a physical device flush contract.
            std::lock_guard lock(mutex_);
            mixer.set_media_resource_enabled(status_.media_resource_enabled);
            status_.resource_configured = status_.resource_requested;
          }
          auto candidate = mixer;
          const auto mixed = candidate.mix(*pending, {}, false);
          bool accepted = false;
          if (output)
            accepted = output->submit(mixed);
          else {
            bool enabled;
            {
              std::lock_guard lock(mutex_);
              enabled = status_.media_resource_enabled;
            }
            accepted =
                options_.playback->submit(playback_channel, *pending, enabled, {}, observation);
          }
          if (output)
            observation.observe(accepted ? audio_observation::Kind::supply
                                         : audio_observation::Kind::supply_blocked,
                                output->accepted(), bool(pending));
          if (!accepted) {
            // Retain one block and let the bounded decoder hold the rest. A full
            // output queue is normal pacing, not a reason to erase part of a song.
            std::lock_guard lock(mutex_);
            increment(status_.output_backpressure);
            break;
          }
          mixer = candidate;
          {
            std::lock_guard lock(mutex_);
            if (output) status_.phone_duck_gain = mixer.phone_duck_gain();
          }
          pending.reset();
        }
        cache_diagnostics();
      }
    } catch (...) {
      cache_diagnostics(false);  // Retain counters without acquiring a new clock after failure.
      throw;
    }
  } catch (const std::exception& error) {
    if (!started) ready.set_exception(std::current_exception());
    std::lock_guard lock(mutex_);
    status_.fault = error.what();
  }
  {
    std::lock_guard lock(mutex_);
    focus_.reset();
  }
  // The worker is terminal even if its session retains diagnostics. Release
  // both ports now rather than holding descriptors until phone teardown. The
  // constructor's failure path and destructor access these only after join.
  if (control_fd_ >= 0) close(std::exchange(control_fd_, -1));
  if (data_fd_ >= 0) close(std::exchange(data_fd_, -1));
  if (playback_channel) options_.playback->detach(playback_channel);
  if (controls_open) controls_->close();
  {
    std::lock_guard lock(mutex_);
    status_.microphone_available = false;
    observation.observe(audio_observation::Kind::stream_stop, status_.authenticated,
                        status_.decoded_blocks, status_.flush_applied, !status_.fault.empty(),
                        true);
  }
}

}  // namespace headunit
