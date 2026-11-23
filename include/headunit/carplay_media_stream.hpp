// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/audio_observation.hpp"

#include "headunit/airplay_audio_packet.hpp"
#include "headunit/audio_control.hpp"
#include "headunit/audio_focus_command.hpp"
#include "headunit/audio_output.hpp"
#include "headunit/audio_rtp_decoder.hpp"
#include "headunit/audio_playback.hpp"

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace headunit {

struct CarplayAudioOptions {
  AudioRtpDecoder::Format format = AudioRtpDecoder::Format::aac_lc_48000_stereo;
  bool media = true, alternate = false;
  std::shared_ptr<AudioPlayback> playback;
  audio_observation::Point observation;
  std::uint16_t microphone_port = 0;
  std::string microphone_source, microphone_device;
};
struct MediaStreamStatus {
  bool voice = false, microphone_requested = false, microphone_available = false;
  std::uint64_t microphone_sent = 0, microphone_dropped = 0;
  std::string microphone_fault;
  std::uint64_t authenticated = 0;
  std::uint64_t wire_datagrams = 0, truncated_datagrams = 0;
  // Opaque peer packets drained on the control UDP port. No RTCP contract or
  // retransmission completion is inferred from these observations.
  std::uint64_t control_datagrams = 0, control_truncated_datagrams = 0;
  // Peer wire/crypto rejection only; decoder queue admission is separate.
  std::uint64_t rejected = 0;
  std::uint64_t decoded_blocks = 0;
  std::optional<AudioDecoderDiagnostics> decoder{};
  std::uint64_t output_backpressure = 0;
  // Accepted by appsrc, not played by a speaker. Underruns count exhausted
  // local PCM schedules (pauses included), independently of decoder loss.
  std::uint64_t output_accepted = 0, output_discontinuities = 0, output_underruns = 0;
  bool output_starved = false;
  // Configured is the worker accepting a ramp, not completed/audible playback.
  std::uint64_t focus_requested = 0, focus_configured = 0;
  double phone_duck_gain = 1.;  // Last successfully accepted PCM block's envelope.
  // Worker configuration is not a sink or audible receipt. Unobserved sessions
  // retain legacy media admission; an explicit unknown owner disables media.
  std::uint64_t resource_requested = 0, resource_configured = 0;
  bool media_resource_enabled = true;
  // RTSP FLUSH: requests accepted, applied to this stream's queue, skipped as
  // another stream's resume point, and late pre-resume packets discarded.
  // Applying a flush rebuilds the decoder, so its diagnostics restart.
  std::uint64_t flush_requested = 0, flush_applied = 0, flush_skipped = 0,
                flush_discarded_packets = 0;
  std::string fault;
};

// One authenticated audio stream, with optional explicitly selected microphone.
// Owns bounded UDP/codec work; a session shares one playback sink and local
// schedule. Decoding and capture never run on the control/display thread.
class CarplayMediaStream {
 public:
  CarplayMediaStream(std::span<const std::uint8_t> secret, std::uint64_t connection_id,
                     const sockaddr_in6& phone, std::string sink_factory, unsigned latency_ms,
                     std::string device = {}, audio_control::Level initial_level = {},
                     AudioOutput::Observer output_observer = {}, bool media_resource_enabled = true,
                     CarplayAudioOptions options = {});
  ~CarplayMediaStream();
  CarplayMediaStream(const CarplayMediaStream&) = delete;
  CarplayMediaStream& operator=(const CarplayMediaStream&) = delete;

  std::uint16_t data_port() const { return data_port_; }
  std::uint16_t control_port() const { return control_port_; }
  MediaStreamStatus status() const;
  std::shared_ptr<audio_control::Mailbox> audio_control() const { return controls_; }
  // Fixed latest request for this stream generation. True is acceptance only.
  bool request_audio_focus(const AudioFocusCommand&);
  bool set_media_resource_enabled(bool);
  // Latest RTSP FLUSH for this stream generation. The worker discards queued
  // RTP/decoded audio when the resume sequence follows its own packets (or
  // none is given) and drops later-arriving packets before that point. True is
  // acceptance only, not removal of audio already handed to the sink.
  bool request_flush(std::optional<std::uint16_t> resume_seq);

 private:
  void run(std::stop_token stopped, std::string sink_factory, unsigned latency_ms,
           std::string device, std::promise<void> ready);

  AirplayAudioPackets packets_;
  CarplayAudioOptions options_;
  std::unique_ptr<AirplayMicrophonePackets> microphone_packets_;
  sockaddr_in6 phone_{};
  int data_fd_ = -1;
  int control_fd_ = -1;
  std::uint16_t data_port_ = 0;
  std::uint16_t control_port_ = 0;
  mutable std::mutex mutex_;
  MediaStreamStatus status_;
  std::optional<AudioFocusCommand> focus_;
  std::optional<std::optional<std::uint16_t>> flush_;
  std::shared_ptr<audio_control::Mailbox> controls_;
  AudioOutput::Observer output_observer_;
  std::jthread worker_;
};

}  // namespace headunit
