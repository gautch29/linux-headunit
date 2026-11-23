// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_microphone.hpp"
#include "headunit/carplay_media_stream.hpp"
#include "headunit/audio_playback.hpp"
#include "headunit/audio_rtp_decoder.hpp"
#include "headunit/airplay_audio_packet.hpp"
#include "headunit/hap_crypto.hpp"
#include "audio_pipeline_fixture.hpp"
#include <gst/app/gstappsrc.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
template <class F>
void wait(F predicate) {
  const auto end = std::chrono::steady_clock::now() + 2s;
  while (!predicate() && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(2ms);
  check(predicate(), "voice fixture timed out");
}
}  // namespace
int main() try {
  using Format = headunit::AudioRtpDecoder::Format;
  for (const auto format :
       {Format::opus_16000_mono, Format::opus_24000_mono, Format::opus_48000_mono,
        Format::pcm_8000_mono, Format::pcm_16000_mono, Format::pcm_24000_mono,
        Format::pcm_32000_mono, Format::pcm_48000_mono}) {
    const auto description = headunit::AudioRtpDecoder::describe(format);
    const bool compressed = description.encoding == headunit::AudioRtpDecoder::Encoding::opus;
    const unsigned step = description.rate / (compressed ? 50 : 100);
    audio_fixture::OutputBus bus("", "encoded", "capture");
    headunit::AudioMicrophone microphone("appsrc", "", description.rate, compressed);
    headunit::AirplayMicrophonePackets transport(std::array<std::uint8_t, 32>{}, 57, step);
    const auto key = headunit::hap_hkdf_sha512(std::array<std::uint8_t, 32>{}, "DataStream-Salt57",
                                               "DataStream-Input-Encryption-Key");
    headunit::AudioRtpDecoder decoder(80, format);
    sockaddr_in6 peer{};
    peer.sin6_family = AF_INET6;
    peer.sin6_addr = in6addr_loopback;
    auto playback = std::make_shared<headunit::AudioPlayback>("fakesink", "");
    headunit::CarplayAudioOptions options;
    options.format = format;
    options.media = false;
    options.playback = playback;
    const bool missing_capture = format == Format::opus_48000_mono;
    if (missing_capture) {
      options.microphone_port = 4567;
      options.microphone_source = "alsasrc";
      options.microphone_device = "hw:HeadunitMissingCaptureCard,0";
    }
    headunit::CarplayMediaStream receiver(std::array<std::uint8_t, 32>{}, 57, peer, "fakesink", 80,
                                          "", {}, {}, true, options);
    const int udp = socket(AF_INET6, SOCK_DGRAM, 0);
    check(udp >= 0, "voice loopback UDP socket failed");
    peer.sin6_port = htons(receiver.data_port());
    const auto output_key = headunit::hap_hkdf_sha512(
        std::array<std::uint8_t, 32>{}, "DataStream-Salt57", "DataStream-Output-Encryption-Key");

    std::uint64_t packets = 0, blocks = 0, amplitude = 0;
    std::uint32_t previous_time = 0;
    bool saw_gap = false;
    for (unsigned turn = 0; turn < 40; ++turn) {
      std::array<std::int16_t, 960> tone{};
      for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = (i / 24) % 2 ? 4000 : -4000;
      bus.push_pcm(tone, (turn * 20 + (turn >= 20 ? 200 : 0)) * GST_MSECOND);
      std::this_thread::sleep_for(20ms);
      while (auto opus = microphone.take()) {
        const auto sealed = transport.seal(opus->bytes, opus->sample_time);
        check(sealed.size() == opus->bytes.size() + 36 && sealed[0] == 0x80 && sealed[1] == 100,
              "microphone RTP header invalid");
        const auto sample_time = (std::uint32_t(sealed[4]) << 24) |
                                 (std::uint32_t(sealed[5]) << 16) |
                                 (std::uint32_t(sealed[6]) << 8) | sealed[7];
        check(sample_time == opus->sample_time && sealed.back() == 0 &&
                  sealed[sealed.size() - 8] == packets,
              "microphone RTP timeline or nonce invalid");
        if (packets && sample_time - previous_time > step * 2) saw_gap = true;
        previous_time = sample_time;
        const std::string nonce(reinterpret_cast<const char*>(sealed.data() + sealed.size() - 8),
                                8);
        const auto clear =
            headunit::hap_decrypt(key, nonce, std::span(sealed).subspan(12, sealed.size() - 20),
                                  std::span(sealed).subspan(4, 8));
        check(clear && *clear == opus->bytes, "microphone ciphertext/key direction invalid");
        if (!packets)
          check(
              sendto(udp, sealed.data(), sealed.size(), 0, reinterpret_cast<const sockaddr*>(&peer),
                     sizeof(peer)) == static_cast<ssize_t>(sealed.size()),
              "wrong-direction fixture send failed");
        const auto phone_sealed =
            headunit::hap_encrypt(output_key, nonce, opus->bytes, std::span(sealed).subspan(4, 8));
        std::vector<std::uint8_t> phone_packet(sealed.begin(), sealed.begin() + 12);
        phone_packet.insert(phone_packet.end(), phone_sealed.begin(), phone_sealed.end());
        phone_packet.insert(phone_packet.end(), sealed.end() - 8, sealed.end());
        check(sendto(udp, phone_packet.data(), phone_packet.size(), 0,
                     reinterpret_cast<const sockaddr*>(&peer),
                     sizeof(peer)) == static_cast<ssize_t>(phone_packet.size()),
              "voice UDP downlink send failed");
        auto corrupt = sealed;
        corrupt[4] ^= 1;
        check(
            !headunit::hap_decrypt(key, nonce, std::span(corrupt).subspan(12, corrupt.size() - 20),
                                   std::span(corrupt).subspan(4, 8)),
            "microphone timestamp was not authenticated");
        std::vector<std::uint8_t> rtp(sealed.begin(), sealed.begin() + 12);
        rtp.insert(rtp.end(), clear->begin(), clear->end());
        check(decoder.submit(rtp), "microphone Opus failed decode admission");
        ++packets;
      }
      for (const auto& block : decoder.take()) {
        ++blocks;
        for (const auto value : block)
          amplitude += std::abs(int(value));
      }
    }
    check(packets >= (compressed ? 35U : 60U) && blocks >= 40 && amplitude > 100000 && saw_gap,
          "capture/encode/encrypt/decode produced no substantial audio");

    wait([&] {
      const auto status = receiver.status();
      return status.authenticated == packets && status.decoded_blocks >= 40 &&
             status.output_accepted >= 20;
    });
    const auto voice_state = receiver.status();
    check(voice_state.voice && voice_state.authenticated == packets && voice_state.rejected == 1 &&
              voice_state.fault.empty(),
          "actual voice UDP worker lost format/key/delivery isolation");
    if (missing_capture)
      check(voice_state.microphone_requested && !voice_state.microphone_available &&
                voice_state.microphone_sent == 0 && !voice_state.microphone_fault.empty() &&
                receiver.audio_control()->snapshot().available,
            "missing ALSA capture device interrupted voice playback or fabricated input");
    close(udp);
    bus.ended();
    bool retired = false;
    for (unsigned i = 0; i < 8 && !retired; ++i)
      try {
        (void)microphone.take();
      } catch (const std::runtime_error&) {
        retired = true;
      }
    check(retired, "microphone EOS was not terminal");
  }

  for (const auto format :
       {Format::pcm_8000_mono, Format::pcm_8000_stereo, Format::pcm_16000_mono,
        Format::pcm_16000_stereo, Format::pcm_24000_mono, Format::pcm_24000_stereo,
        Format::pcm_32000_mono, Format::pcm_32000_stereo, Format::pcm_48000_mono,
        Format::pcm_48000_stereo}) {
    const auto description = headunit::AudioRtpDecoder::describe(format);
    headunit::AudioRtpDecoder decoder(20, format);
    std::uint64_t blocks = 0, amplitude = 0;
    for (unsigned index = 0; index < 30; ++index) {
      const auto timestamp = index * (description.rate / 100);
      std::vector<std::uint8_t> packet{0x80,
                                       100,
                                       0,
                                       std::uint8_t(index),
                                       std::uint8_t(timestamp >> 24),
                                       std::uint8_t(timestamp >> 16),
                                       std::uint8_t(timestamp >> 8),
                                       std::uint8_t(timestamp),
                                       0,
                                       0,
                                       0,
                                       0};
      for (unsigned sample = 0; sample < description.rate / 100 * description.channels; ++sample) {
        packet.push_back(0x13);
        packet.push_back(0x88);  // 5000, network-order signed PCM.
      }
      check(decoder.submit(packet), "PCM RTP admission failed");
      std::this_thread::sleep_for(10ms);
      for (const auto& block : decoder.take()) {
        ++blocks;
        for (const auto value : block)
          amplitude += std::abs(int(value));
      }
    }
    std::this_thread::sleep_for(50ms);
    for (const auto& block : decoder.take()) {
      ++blocks;
      for (const auto value : block)
        amplitude += std::abs(int(value));
    }
    check(blocks >= 20 && amplitude > 100000,
          "negotiated PCM did not decode into substantial stereo audio");
  }
  // Exercise the actual worker's capture/send path, not just the packet codec.
  // appsrc is an explicit in-process fixture; saved production config rejects it.
  for (const auto format : {Format::opus_16000_mono, Format::pcm_48000_mono}) {
    const int phone_socket = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    check(phone_socket >= 0, "microphone fixture destination socket failed");
    sockaddr_in6 phone{};
    phone.sin6_family = AF_INET6;
    phone.sin6_addr = in6addr_loopback;
    check(bind(phone_socket, reinterpret_cast<sockaddr*>(&phone), sizeof(phone)) == 0,
          "microphone fixture destination bind failed");
    socklen_t length = sizeof(phone);
    check(getsockname(phone_socket, reinterpret_cast<sockaddr*>(&phone), &length) == 0,
          "microphone fixture destination port unavailable");
    audio_fixture::OutputBus capture("", "encoded", "capture");
    auto playback = std::make_shared<headunit::AudioPlayback>("fakesink", "");
    headunit::CarplayAudioOptions options;
    options.format = format;
    options.media = false;
    options.playback = playback;
    options.microphone_source = "appsrc";
    options.microphone_port = ntohs(phone.sin6_port);
    headunit::CarplayMediaStream worker(std::array<std::uint8_t, 32>{}, 81, phone, "fakesink", 20,
                                        "", {}, {}, true, options);
    check(worker.status().microphone_available, "fixture capture worker did not open");
    const auto input_key = headunit::hap_hkdf_sha512(
        std::array<std::uint8_t, 32>{}, "DataStream-Salt81", "DataStream-Input-Encryption-Key");
    const auto output_key = headunit::hap_hkdf_sha512(
        std::array<std::uint8_t, 32>{}, "DataStream-Salt81", "DataStream-Output-Encryption-Key");
    const auto rate = headunit::AudioRtpDecoder::describe(format).rate;
    const auto step = rate / (format == Format::opus_16000_mono ? 50 : 100);
    std::uint64_t packets = 0;
    std::uint32_t previous_time = 0;
    bool gap = false;
    const auto receive = [&] {
      std::array<std::uint8_t, 2048> bytes{};
      sockaddr_in6 source{};
      socklen_t size = sizeof(source);
      for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const auto count = recvfrom(phone_socket, bytes.data(), bytes.size(), 0,
                                    reinterpret_cast<sockaddr*>(&source), &size);
        if (count < 0) {
          check(errno == EAGAIN || errno == EWOULDBLOCK,
                "microphone fixture destination read failed");
          break;
        }
        check(count > 36 && source.sin6_port == htons(worker.data_port()) && bytes[0] == 0x80 &&
                  bytes[1] == 100,
              "worker microphone RTP source/header invalid");
        const auto packet = std::span(bytes).first(static_cast<std::size_t>(count));
        const std::string nonce(reinterpret_cast<const char*>(packet.data() + count - 8), 8);
        check(
            static_cast<unsigned char>(nonce[0]) == packets &&
                std::all_of(nonce.begin() + 1, nonce.end(), [](char value) { return value == 0; }),
            "worker microphone nonce was reused or advanced without a packet");
        const auto clear = headunit::hap_decrypt(input_key, nonce, packet.subspan(12, count - 20),
                                                 packet.subspan(4, 8));
        check(clear && !clear->empty() &&
                  !headunit::hap_decrypt(output_key, nonce, packet.subspan(12, count - 20),
                                         packet.subspan(4, 8)),
              "worker microphone used the wrong encryption direction");
        const auto timestamp = (std::uint32_t(bytes[4]) << 24) | (std::uint32_t(bytes[5]) << 16) |
                               (std::uint32_t(bytes[6]) << 8) | bytes[7];
        if (packets && timestamp - previous_time > step * 2) gap = true;
        previous_time = timestamp;
        // Re-encrypt this actual captured frame as downlink. Both directions
        // must keep working on the same advertised endpoint.
        auto encrypted = headunit::hap_encrypt(output_key, nonce, *clear, packet.subspan(4, 8));
        std::vector<std::uint8_t> downlink(packet.begin(), packet.begin() + 12);
        downlink.insert(downlink.end(), encrypted.begin(), encrypted.end());
        downlink.insert(downlink.end(), packet.end() - 8, packet.end());
        auto target = phone;
        target.sin6_port = htons(worker.data_port());
        check(sendto(phone_socket, downlink.data(), downlink.size(), 0,
                     reinterpret_cast<const sockaddr*>(&target),
                     sizeof(target)) == static_cast<ssize_t>(downlink.size()),
              "worker full duplex fixture send failed");
        ++packets;
      }
    };
    for (unsigned index = 0; index < 30; ++index) {
      std::array<std::int16_t, 960> tone{};
      tone.fill(4000);
      capture.push_pcm(tone, (index * 20 + (index >= 15 ? 200 : 0)) * GST_MSECOND);
      std::this_thread::sleep_for(20ms);
      receive();
    }
    wait([&] {
      receive();
      return packets >= 25 && worker.status().microphone_sent == packets &&
             worker.status().authenticated == packets && worker.status().decoded_blocks >= 20;
    });
    check(gap && worker.status().microphone_sent == packets &&
              worker.status().microphone_dropped == 0 && worker.status().fault.empty(),
          "worker microphone lost capture timestamps or full duplex delivery");
    capture.ended();
    wait([&] { return !worker.status().microphone_available; });
    const auto state = worker.status();
    check(!state.microphone_fault.empty() && state.fault.empty() &&
              worker.audio_control()->snapshot().available,
          "capture EOS retired the output/master route");
    close(phone_socket);
  }
  {
    audio_fixture::OutputBus sink;
    std::atomic<unsigned> peak{0};
    std::atomic<bool> saw_mix{false};
    headunit::AudioPlayback playback("fakesink", "", [&](const auto& receipt) {
      if (receipt.peak_sample) {
        peak.store(*receipt.peak_sample);
        if (*receipt.peak_sample >= 10000) saw_mix.store(true);
      }
    });
    auto music_control = playback.audio_control();
    auto music = playback.attach(true);
    auto voice = playback.attach(false);
    headunit::AudioMixer::Block tone;
    tone.fill(5000);
    sink.block_sink();
    check(playback.submit(music, tone, true, {}), "music admission failed");
    check(playback.submit(voice, tone, true, {}), "voice admission failed");
    // Fill bounded queues while the real sink is stalled, rather than growing
    // memory with unbounded PCM. Output must remain one physical owner.
    unsigned accepted = 0;
    for (unsigned i = 0; i < 100; ++i)
      if (playback.submit(music, tone, true, {})) ++accepted;
    for (unsigned i = 0; i < 20; ++i)
      (void)playback.submit(voice, tone, true, {});
    check(accepted < 100, "shared audio queue was unbounded");
    sink.unblock_sink();
    wait([&] { return playback.status().accepted > 0 && saw_mix.load(); });
    wait([&] {
      const auto output = music_control->snapshot().output;
      return output && output->accepted_blocks > 0;
    });
    playback.detach(voice);
    check(music_control->snapshot().available, "voice retirement retired music controls");
    check(
        music_control->request({1, {10, true}}) == headunit::audio_control::RequestResult::accepted,
        "shared gain control failed");
    wait([&] { return music_control->snapshot().applied.sequence == 1; });
    wait([&] {
      const auto output = music_control->snapshot().output;
      return output && output->starved;
    });
    const auto exhausted = *music_control->snapshot().output;
    std::this_thread::sleep_for(40ms);
    const auto idle = *music_control->snapshot().output;
    check(idle.sampled_ns > exhausted.sampled_ns &&
              idle.accepted_blocks == exhausted.accepted_blocks && idle.starved &&
              idle.underruns == exhausted.underruns,
          "output-owner polling fabricated PCM or lost starvation state");
    sink.error("offline shared sink failure");
    wait([&] { return !music_control->snapshot().available; });
    check(!music_control->snapshot().output, "failed shared output retained live progress");
    check(!playback.status().fault.empty(), "shared sink fault was not retained");
    playback.detach(music);
  }
  bool invalid = false;
  try {
    headunit::AudioMicrophone::validate_selection("appsrc", "");
  } catch (const std::invalid_argument&) {
    invalid = true;
  }
  check(invalid, "production configuration accepted a fake microphone");
  std::cout << "Voice capture fixture, encrypted uplink, decode, shared sink, bounded queues and "
               "retirement passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
