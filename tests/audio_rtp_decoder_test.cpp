// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_rtp_decoder.hpp"
#include "audio_pipeline_fixture.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) try {
  if (argc != 2) throw std::invalid_argument("expected AAC fixture path");
  std::ifstream file(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> adts(std::istreambuf_iterator<char>(file),
                                       std::istreambuf_iterator<char>{});
  if (adts.size() < 7) throw std::runtime_error("AAC fixture missing");

  headunit::AudioRtpDecoder decoder(20);
  if (!decoder.diagnostics().jitter)
    throw std::runtime_error("production jitter buffer statistics unavailable");
  std::size_t blocks = 0;
  std::uint64_t amplitude = 0;
  const auto collect = [&] {
    for (const auto& block : decoder.take()) {
      ++blocks;
      for (auto value : block)
        amplitude += value < 0 ? -std::int32_t(value) : value;
    }
  };
  // A phone sends one AAC frame per 1024 samples. An instant burst of the
  // whole file is not a real arrival pattern: on a loaded host it can reach
  // the live jitter buffer (20 ms, drop-on-latency) before the pipeline runs,
  // and every packet is then legitimately dropped as late.
  const auto frame_period = std::chrono::microseconds(1024 * 1000000 / 48000);
  auto next_frame = std::chrono::steady_clock::now();
  std::uint16_t sequence = 1;
  std::uint32_t timestamp = 1024;
  std::uint64_t submitted = 0;
  for (std::size_t at = 0; at + 7 <= adts.size();) {
    if (adts[at] != 0xff || (adts[at + 1] & 0xf0) != 0xf0)
      throw std::runtime_error("invalid AAC fixture frame");
    const auto length = (std::size_t(adts[at + 3] & 3) << 11) | (std::size_t(adts[at + 4]) << 3) |
                        (adts[at + 5] >> 5);
    const auto header = (adts[at + 1] & 1) ? 7u : 9u;
    if (length <= header || at + length > adts.size())
      throw std::runtime_error("invalid AAC frame length");
    std::vector<std::uint8_t> rtp{0x80,
                                  96,
                                  std::uint8_t(sequence >> 8),
                                  std::uint8_t(sequence),
                                  std::uint8_t(timestamp >> 24),
                                  std::uint8_t(timestamp >> 16),
                                  std::uint8_t(timestamp >> 8),
                                  std::uint8_t(timestamp),
                                  1,
                                  2,
                                  3,
                                  4};
    rtp.insert(rtp.end(), adts.begin() + at + header, adts.begin() + at + length);
    // One deliberate gap and duplicate exercise the actual receiver statistics.
    // These are controlled RTP observations, not measured physical packet loss.
    if (sequence != 10) {
      if (!decoder.submit(rtp)) throw std::runtime_error("AAC packet rejected");
      ++submitted;
      if (sequence == 15) {
        if (!decoder.submit(rtp)) throw std::runtime_error("AAC duplicate rejected before jitter");
        ++submitted;
      }
    }
    collect();
    next_frame += frame_period;
    std::this_thread::sleep_until(next_frame);
    at += length;
    ++sequence;
    timestamp += 1024;
  }

  // Upper bound only: the loop ends as soon as 20 blocks arrive. Sanitized
  // builds on shared CI hosts need more than 3 s to start the decoder.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && blocks < 20) {
    collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (blocks < 20 || amplitude < 100000)
    throw std::runtime_error("AAC RTP did not produce nonzero PCM blocks");
  if (sequence <= 15) throw std::runtime_error("AAC fixture too short for loss accounting");
  if (decoder.submit({})) throw std::runtime_error("malformed decoder packet accepted");
  const auto measured = decoder.diagnostics();
  if (measured.submitted != submitted || measured.invalid_packets != 1 ||
      measured.input_backpressure || measured.input_failures || !measured.jitter ||
      measured.jitter->considered_lost < 1 ||
      (!measured.jitter->duplicates && !measured.jitter->late))
    throw std::runtime_error("actual jitter gap/duplicate or decoder admission was not accounted");
  guint major = 0, minor = 0, micro = 0, nano = 0;
  gst_version(&major, &minor, &micro, &nano);
  if (major == 1 && minor < 28 && measured.appsink_dropped_buffers)
    throw std::runtime_error("older appsink manufactured a zero drop receipt");
  {
    audio_fixture::OutputBus actual_bus("", "jitter", "source");
    headunit::AudioRtpDecoder bounded(20);
    actual_bus.block_sink();
    std::vector<std::uint8_t> packet(4096);
    packet[0] = 0x80;
    for (unsigned index = 0; index < 128; ++index)
      (void)bounded.submit(packet);
    const auto overloaded = bounded.diagnostics();
    actual_bus.unblock_sink();
    if (!overloaded.submitted || !overloaded.input_backpressure || overloaded.invalid_packets ||
        overloaded.input_failures)
      throw std::runtime_error("bounded appsrc overload was counted as packet rejection");
  }
  // Exercise the real decoder bus with controlled offline messages. This
  // observes actual pipeline handling, not a phone or physical driver's fault.
  for (const bool ended : {false, true}) {
    audio_fixture::OutputBus actual_bus("", "decoded", "source");
    headunit::AudioRtpDecoder idle(20);
    for (unsigned index = 0; index < 10; ++index)
      (void)idle.take();
    (void)actual_bus.pending_messages();
    actual_bus.notices(96);
    (void)idle.take();
    const auto remaining = actual_bus.pending_messages(true);
    if (remaining < 64 || remaining > 96)
      throw std::runtime_error("decoder bus poll was not bounded to 32 notices");
    actual_bus.notices(96);
    for (unsigned index = 0; index < 10; ++index)
      (void)idle.take();
    if (actual_bus.pending_messages(true))
      throw std::runtime_error("decoder retained ordinary bus notices");
    const std::string expected =
        ended ? "AAC decoder ended unexpectedly" : "offline AAC decoder failed";
    if (ended)
      actual_bus.ended();
    else
      actual_bus.error(expected.c_str());
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      bool failed = false;
      try {
        if (attempt == 1)
          (void)idle.submit({});
        else
          (void)idle.take();
      } catch (const std::runtime_error& error) {
        if (error.what() != expected) throw;
        failed = true;
      }
      if (!failed) throw std::runtime_error("decoder did not retain terminal fault");
      if (idle.diagnostics().submitted || !idle.diagnostics().jitter)
        throw std::runtime_error("terminal decoder diagnostics lost or manufactured input");
    }
  }
  std::cout << "AAC decode, real jitter gap/duplicate statistics, bounded admission, "
               "bus polling and retained error/EOS passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
