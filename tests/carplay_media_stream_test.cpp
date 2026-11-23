// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/carplay_media_stream.hpp"
#include "headunit/hap_crypto.hpp"
#include "audio_pipeline_fixture.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
struct ReboundPorts {
  std::array<int, 2> fds{-1, -1};
  ~ReboundPorts() {
    for (const auto fd : fds)
      if (fd >= 0) close(fd);
  }
  void check_open() const {
    for (const auto fd : fds)
      if (fcntl(fd, F_GETFD) < 0)
        throw std::runtime_error("retired audio destructor closed a reused descriptor");
  }
};
std::unique_ptr<ReboundPorts> released_ports(const headunit::CarplayMediaStream& receiver) {
  // Retain the faulted object and prove both real UDP reservations are gone.
  // A closed control mailbox alone does not establish descriptor retirement.
  auto ports = std::make_unique<ReboundPorts>();
  for (unsigned index = 0; index < ports->fds.size(); ++index) {
    ports->fds[index] = socket(AF_INET6, SOCK_DGRAM, 0);
    if (ports->fds[index] < 0) throw std::runtime_error("retirement fixture socket failed");
    sockaddr_in6 local{};
    local.sin6_family = AF_INET6;
    local.sin6_addr = in6addr_any;
    local.sin6_port = htons(index == 0 ? receiver.data_port() : receiver.control_port());
    if (bind(ports->fds[index], reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
      throw std::runtime_error("faulted retained audio owner still reserves a UDP port");
  }
  return ports;
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2 && argc != 3) throw std::invalid_argument("expected AAC fixture");
  std::ifstream file(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> adts(std::istreambuf_iterator<char>(file),
                                       std::istreambuf_iterator<char>{});
  const std::array<std::uint8_t, 32> shared{};
  sockaddr_in6 phone{};
  phone.sin6_family = AF_INET6;
  phone.sin6_addr = in6addr_loopback;
  const auto observation =
      argc == 3 ? std::make_shared<headunit::audio_observation::Capture>() : nullptr;
  headunit::CarplayAudioOptions options;
  options.observation = {observation, 0, 0};
  headunit::CarplayMediaStream receiver(shared, 42, phone, "fakesink", 1000, "", {}, {}, true,
                                        options);
  const auto key =
      headunit::hap_hkdf_sha512(shared, "DataStream-Salt42", "DataStream-Output-Encryption-Key");
  const int socket_fd = socket(AF_INET6, SOCK_DGRAM, 0);
  if (socket_fd < 0) throw std::runtime_error("test UDP socket failed");
  if (fcntl(socket_fd, F_SETFD, FD_CLOEXEC) < 0) {
    close(socket_fd);
    throw std::runtime_error("test UDP socket flags failed");
  }
  sockaddr_in6 target = phone;
  target.sin6_port = htons(receiver.data_port());
  {
    auto control_target = target;
    control_target.sin6_port = htons(receiver.control_port());
    const std::vector<std::uint8_t> oversized_control(4097, 0x5a);
    for (const auto size : {std::size_t(0), oversized_control.size()}) {
      if (sendto(socket_fd, oversized_control.data(), size, 0,
                 reinterpret_cast<const sockaddr*>(&control_target),
                 sizeof(control_target)) != static_cast<ssize_t>(size))
        throw std::runtime_error("opaque control UDP fixture send failed");
    }
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (receiver.status().control_datagrams != 2 && receiver.status().fault.empty() &&
           std::chrono::steady_clock::now() < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto ignored = receiver.status();
    if (ignored.control_datagrams != 2 || ignored.control_truncated_datagrams != 1 ||
        ignored.authenticated || ignored.rejected || !ignored.fault.empty())
      throw std::runtime_error("opaque/empty/truncated UDP control changed media admission");
  }

  // An oversized UDP packet can contain an independently authenticated 4096
  // byte prefix. The actual datagram must be rejected before that prefix is
  // decrypted or its nonce is recorded; the valid fixture below reuses nonce0.
  {
    const std::array<std::uint8_t, 12> header{0x80, 96, 0, 1, 0, 0, 4, 0, 1, 2, 3, 4};
    const std::array<std::uint8_t, 8> nonce{};
    const std::vector<std::uint8_t> clear(4060, 0);
    const std::string nonce_text(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    const auto sealed =
        headunit::hap_encrypt(key, nonce_text, clear, std::span(header).subspan(4, 8));
    std::vector<std::uint8_t> oversized(header.begin(), header.end());
    oversized.insert(oversized.end(), sealed.begin(), sealed.end());
    oversized.insert(oversized.end(), nonce.begin(), nonce.end());
    if (oversized.size() != 4096)
      throw std::runtime_error("oversized fixture prefix has wrong length");
    oversized.push_back(0x5a);
    if (sendto(socket_fd, oversized.data(), oversized.size(), 0,
               reinterpret_cast<const sockaddr*>(&target),
               sizeof(target)) != static_cast<ssize_t>(oversized.size()))
      throw std::runtime_error("oversized test UDP send failed");
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (receiver.status().rejected == 0 && receiver.status().fault.empty() &&
           std::chrono::steady_clock::now() < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto rejected = receiver.status();
    if (rejected.rejected != 1 || rejected.authenticated || !rejected.fault.empty() ||
        rejected.wire_datagrams != 1 || rejected.truncated_datagrams != 1 || !rejected.decoder ||
        rejected.decoder->submitted)
      throw std::runtime_error("oversized UDP prefix reached authentication or the decoder");
  }

  std::uint16_t sequence = 1;
  std::uint32_t timestamp = 1024;
  std::uint64_t nonce_value = 0;
  // ADTS frames as (payload offset, payload length), reused after a flush.
  std::vector<std::pair<std::size_t, std::size_t>> frames;
  const auto send_frame = [&](std::uint16_t seq, std::uint32_t time, std::size_t index) {
    const auto [offset, length] = frames.at(index);
    std::array<std::uint8_t, 12> header{0x80,
                                        96,
                                        std::uint8_t(seq >> 8),
                                        std::uint8_t(seq),
                                        std::uint8_t(time >> 24),
                                        std::uint8_t(time >> 16),
                                        std::uint8_t(time >> 8),
                                        std::uint8_t(time),
                                        1,
                                        2,
                                        3,
                                        4};
    std::array<std::uint8_t, 8> nonce{};
    for (std::size_t i = 0; i < 8; ++i)
      nonce[i] = std::uint8_t(nonce_value >> (i * 8));
    ++nonce_value;
    const std::string nonce_text(reinterpret_cast<const char*>(nonce.data()), 8);
    const auto sealed = headunit::hap_encrypt(
        key, nonce_text, std::span(adts).subspan(offset, length), std::span(header).subspan(4, 8));
    std::vector<std::uint8_t> datagram(header.begin(), header.end());
    datagram.insert(datagram.end(), sealed.begin(), sealed.end());
    datagram.insert(datagram.end(), nonce.begin(), nonce.end());
    if (sendto(socket_fd, datagram.data(), datagram.size(), 0,
               reinterpret_cast<const sockaddr*>(&target),
               sizeof(target)) != static_cast<ssize_t>(datagram.size()))
      throw std::runtime_error("test UDP send failed");
  };
  for (std::size_t at = 0; at + 7 <= adts.size();) {
    const auto length = (std::size_t(adts[at + 3] & 3) << 11) | (std::size_t(adts[at + 4]) << 3) |
                        (adts[at + 5] >> 5);
    const auto header_length = (adts[at + 1] & 1) ? 7u : 9u;
    if (adts[at] != 0xff || length <= header_length || at + length > adts.size())
      throw std::runtime_error("invalid AAC fixture");
    frames.emplace_back(at + header_length, length - header_length);
    send_frame(sequence, timestamp, frames.size() - 1);
    at += length;
    ++sequence;
    timestamp += 1024;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  headunit::MediaStreamStatus status;
  do {
    status = receiver.status();
    if (!status.fault.empty()) throw std::runtime_error(status.fault);
    if (status.decoded_blocks >= 40) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  if (status.authenticated < 20 || status.decoded_blocks < 40 || status.rejected != 1 ||
      !status.decoder || status.decoder->discarded_pcm_frames ||
      status.decoder->input_backpressure || status.decoder->submitted != status.authenticated ||
      status.wire_datagrams != status.authenticated + status.rejected)
    throw std::runtime_error("encrypted AAC did not reach output intact: accepted=" +
                             std::to_string(status.authenticated) +
                             " blocks=" + std::to_string(status.decoded_blocks) +
                             " rejected=" + std::to_string(status.rejected));
  // RTSP FLUSH: rebuild the queue, drop a late packet from before the resume
  // point, accept the resume packet, and ignore another stream's resume point.
  {
    const auto wait_for = [&](auto done, const char* failure) {
      const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!done(receiver.status()) && std::chrono::steady_clock::now() < timeout)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      if (!done(receiver.status())) throw std::runtime_error(failure);
    };
    const std::uint16_t resume = sequence + 4;
    if (!receiver.request_flush(resume)) throw std::runtime_error("flush request refused");
    wait_for([](const auto& s) { return s.flush_applied == 1; }, "nearby resume point not applied");
    send_frame(static_cast<std::uint16_t>(sequence - 2), timestamp, 0);
    wait_for([](const auto& s) { return s.flush_discarded_packets == 1; },
             "late pre-resume packet was not discarded");
    send_frame(resume, timestamp + 4096, 1);
    wait_for([](const auto& s) { return s.decoder && s.decoder->submitted == 1; },
             "resume packet did not reach the rebuilt decoder");
    if (!receiver.request_flush(static_cast<std::uint16_t>(resume + 30000)))
      throw std::runtime_error("distant flush request refused");
    wait_for([](const auto& s) { return s.flush_skipped == 1; },
             "another stream's resume point flushed this stream");
    if (!receiver.request_flush(std::nullopt))
      throw std::runtime_error("unconditional flush request refused");
    wait_for([](const auto& s) { return s.flush_applied == 2 && s.flush_requested == 3; },
             "flush without a resume point not applied");
    if (!receiver.status().fault.empty() || receiver.status().flush_discarded_packets != 1)
      throw std::runtime_error("flush changed stream health");
  }
  close(socket_fd);
  // No packet or PCM submission should be required to discover a failed audio
  // device. The real worker must close availability and reject later controls.
  {
    audio_fixture::OutputBus actual_bus;
    auto idle = std::make_unique<headunit::CarplayMediaStream>(
        shared, 43, phone, "fakesink", 20, "", headunit::audio_control::Level{},
        headunit::AudioOutput::Observer{}, true, options);
    const auto controls = idle->audio_control();
    if (!controls->snapshot().available) throw std::runtime_error("idle audio owner did not open");
    actual_bus.error("offline idle audio device disappeared");
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((idle->status().fault.empty() || controls->snapshot().available) &&
           std::chrono::steady_clock::now() < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (idle->status().fault != "offline idle audio device disappeared" ||
        controls->snapshot().available || idle->status().authenticated ||
        idle->status().decoded_blocks ||
        controls->request({1, {50, false}}) != headunit::audio_control::RequestResult::unavailable)
      throw std::runtime_error("idle output failure did not retire actual audio controls");
    if (idle->request_audio_focus({.2, 500}))
      throw std::runtime_error("faulted output still accepts focus");
    const auto rebound = released_ports(*idle);
    idle.reset();
    rebound->check_open();
  }
  for (const bool ended : {false, true}) {
    audio_fixture::OutputBus actual_bus("", "decoded", "source");
    headunit::CarplayMediaStream idle(shared, 44, phone, "fakesink", 20, "", {}, {}, true, options);
    const auto controls = idle.audio_control();
    if (!controls->snapshot().available)
      throw std::runtime_error("idle decoder owner did not open");
    if (!idle.request_audio_focus({.2, 500}))
      throw std::runtime_error("idle focus request not accepted");
    const std::string expected =
        ended ? "AAC decoder ended unexpectedly" : "offline idle AAC decoder disappeared";
    if (ended)
      actual_bus.ended();
    else
      actual_bus.error(expected.c_str());
    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((idle.status().fault.empty() || controls->snapshot().available) &&
           std::chrono::steady_clock::now() < timeout)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (idle.status().fault != expected || controls->snapshot().available ||
        idle.status().authenticated || idle.status().decoded_blocks || !idle.status().decoder ||
        idle.status().decoder->submitted ||
        controls->request({1, {50, false}}) !=
            headunit::audio_control::RequestResult::unavailable ||
        idle.request_audio_focus({1., 500}) || idle.status().phone_duck_gain != 1.)
      throw std::runtime_error("idle decoder failure did not retire actual audio controls");
    released_ports(idle);
  }
  if (observation) {
    std::array<headunit::audio_observation::Event, 512> batch;
    bool truncated = false, pre_resume = false, flush = false, release = false;
    unsigned retired_faults = 0;
    while (const auto count = observation->take(batch)) {
      for (std::size_t i = 0; i < count; ++i) {
        const auto& e = batch[i];
        if (e.kind == headunit::audio_observation::Kind::stream_stop && e.d == 1 && e.date_ns)
          ++retired_faults;
        if (e.kind == headunit::audio_observation::Kind::truncated) truncated = true;
        if (e.kind == headunit::audio_observation::Kind::pre_resume) pre_resume = e.epoch == 2;
        if (e.kind == headunit::audio_observation::Kind::flush_applied) flush = e.epoch >= 2;
        if (e.kind == headunit::audio_observation::Kind::jitter_release) release = e.date_ns != 0;
      }
    }
    if (!truncated || !pre_resume || !flush || !release || retired_faults != 3)
      throw std::runtime_error("enabled UDP diagnostics lost rejection/epoch/release boundaries");
  }
  std::cout << "encrypted AAC: oversized-wire rejection, nonce preservation, decoded output and "
               "idle-fault UDP retirement/reused-descriptor safety and RTSP flush passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
