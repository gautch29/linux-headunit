// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_rtp_decoder.hpp"
#include "audio_pipeline_fixture.hpp"

#include <gst/app/gstappsink.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// A generated mono tone, with real RTP Opus framing and no phone material.
class Encoder {
 public:
  Encoder() {
    gst_init(nullptr, nullptr);
    GError* error = nullptr;
    pipeline_ = gst_parse_launch(
        "audiotestsrc is-live=true num-buffers=40 samplesperbuffer=480 wave=sine "
        "freq=440 volume=0.25 ! audio/x-raw,format=S16LE,rate=48000,channels=1 ! "
        "opusenc frame-size=10 audio-type=voice ! rtpopuspay pt=96 ! "
        "appsink name=encoded sync=false max-buffers=8 drop=false wait-on-eos=false",
        &error);
    if (error || !pipeline_) {
      if (error) g_error_free(error);
      if (pipeline_) gst_object_unref(pipeline_);
      throw std::runtime_error("synthetic Opus encoder unavailable");
    }
    sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "encoded");
    if (!sink_ || gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      if (sink_) gst_object_unref(sink_);
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(pipeline_);
      throw std::runtime_error("synthetic Opus encoder could not start");
    }
  }
  ~Encoder() {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(sink_);
    gst_object_unref(pipeline_);
  }
  std::vector<std::uint8_t> next() {
    auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 2 * GST_SECOND);
    if (!sample) {
      check(gst_app_sink_is_eos(GST_APP_SINK(sink_)), "synthetic encoder stalled");
      return {};
    }
    auto* buffer = gst_sample_get_buffer(sample);
    std::vector<std::uint8_t> bytes(gst_buffer_get_size(buffer));
    const auto read = gst_buffer_extract(buffer, 0, bytes.data(), bytes.size());
    gst_sample_unref(sample);
    check(read == bytes.size(), "synthetic packet extraction failed");
    return bytes;
  }

 private:
  GstElement* pipeline_ = nullptr;
  GstElement* sink_ = nullptr;
};
}  // namespace

int main() try {
  using Decoder = headunit::AudioRtpDecoder;
  Encoder encoder;
  Decoder decoder(80, Decoder::Format::opus_48000_mono);
  std::uint64_t packets = 0, blocks = 0, amplitude = 0;
  const auto collect = [&] {
    for (const auto& block : decoder.take()) {
      ++blocks;
      for (std::size_t index = 0; index < block.size(); index += 2) {
        check(block[index] == block[index + 1], "mono Opus did not produce matching stereo");
        amplitude += block[index] < 0 ? -std::int32_t(block[index]) : block[index];
      }
    }
  };
  for (auto packet = encoder.next(); !packet.empty(); packet = encoder.next()) {
    check(decoder.submit(packet), "valid synthetic Opus packet rejected");
    ++packets;
    collect();
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    collect();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(packets >= 40 && blocks >= 25 && blocks <= 45 && amplitude > 100000,
        "Opus RTP did not decode substantial mono tone into bounded PCM blocks");
  std::vector<std::uint8_t> empty_rtp(12);
  empty_rtp[0] = 0x80;
  check(!decoder.submit(empty_rtp), "empty Opus payload accepted");
  check(!decoder.submit(std::vector<std::uint8_t>(4097)), "oversized Opus packet accepted");
  const auto counts = decoder.diagnostics();
  check(counts.submitted == packets && counts.invalid_packets == 2 && counts.input_failures == 0 &&
            counts.discarded_pcm_frames == 0 && counts.invalid_pcm_buffers == 0 &&
            counts.jitter.has_value(),
        "Opus decoder counters lost actual admission or PCM faults");
  {
    audio_fixture::OutputBus bus("", "decoded", "source");
    Decoder failing(80, Decoder::Format::opus_48000_mono);
    bus.ended();
    bool failed = false;
    for (unsigned turn = 0; turn < 8 && !failed; ++turn) {
      try {
        (void)failing.take();
      } catch (const std::runtime_error& error) {
        failed = std::string(error.what()) == "Opus decoder ended unexpectedly";
        if (!failed) throw;
      }
    }
    check(failed, "Opus EOS did not retire the decoder");
    try {
      (void)failing.submit(empty_rtp);
      throw std::logic_error("retired Opus decoder accepted another operation");
    } catch (const std::runtime_error& error) {
      check(std::string(error.what()) == "Opus decoder ended unexpectedly",
            "Opus terminal fault was not retained");
    }
  }
  std::cout
      << "Synthetic Opus mono RTP decode, stereo PCM, bounded input and terminal EOS passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
