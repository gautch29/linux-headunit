// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_mixer.hpp>
#include <headunit/audio_output.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Options {
  std::string sink;
  std::string device;
  unsigned seconds = 2;
  unsigned volume = 5;
};

unsigned integer(std::string_view text, unsigned maximum) {
  unsigned value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0 || value > maximum)
    throw std::invalid_argument("duration and volume must be integers in 1..10");
  return value;
}

Options options(int argc, char** argv) {
  Options result;
  bool have_sink = false, have_device = false, have_seconds = false, have_volume = false;
  for (int at = 1; at < argc; at += 2) {
    if (at + 1 == argc) throw std::invalid_argument("each option requires a value");
    const std::string_view name = argv[at];
    const std::string_view value = argv[at + 1];
    if (name == "--sink" && !have_sink) {
      result.sink = value;
      have_sink = true;
    } else if (name == "--device" && !have_device) {
      result.device = value;
      have_device = true;
    } else if (name == "--seconds" && !have_seconds) {
      result.seconds = integer(value, 10);
      have_seconds = true;
    } else if (name == "--volume" && !have_volume) {
      result.volume = integer(value, 10);
      have_volume = true;
    } else {
      throw std::invalid_argument("unknown or repeated audio check option");
    }
  }
  if (!have_sink) throw std::invalid_argument("--sink must be explicitly selected");
  headunit::AudioOutput::validate_selection(result.sink, result.device);
  if (result.sink != "fakesink" && result.device.empty())
    throw std::invalid_argument("a physical sink requires an explicit --device");
  if (have_device && result.device.empty()) throw std::invalid_argument("--device cannot be empty");
  return result;
}

std::string json_quoted(std::string_view value) {
  std::string text = "\"";
  for (const auto ch : value) {
    if (ch == '"' || ch == '\\') text += '\\';
    text += ch;
  }
  return text + '"';
}

struct Observations {
  std::atomic<std::uint64_t> buffers{0}, peak_available{0}, nonzero{0}, zero{0};
  std::atomic<unsigned> peak{0};

  void observe(const headunit::AudioOutput::Receipt& receipt) {
    buffers.fetch_add(1, std::memory_order_relaxed);
    if (!receipt.peak_sample) return;
    peak_available.fetch_add(1, std::memory_order_relaxed);
    if (*receipt.peak_sample == 0)
      zero.fetch_add(1, std::memory_order_relaxed);
    else
      nonzero.fetch_add(1, std::memory_order_relaxed);
    auto previous = peak.load(std::memory_order_relaxed);
    while (previous < *receipt.peak_sample &&
           !peak.compare_exchange_weak(previous, *receipt.peak_sample, std::memory_order_relaxed)) {
    }
  }
};

void run(const Options& selected) {
  // This process owns only its chosen output. It opens no radios, phone session,
  // microphone, capture or display and does not change the appliance profile.
  Observations observed;
  headunit::AudioOutput output(selected.sink, selected.device,
                               [&](const auto& receipt) { observed.observe(receipt); });
  headunit::AudioMixer mixer;
  mixer.set_master_volume(selected.volume / 100.);
  std::uint64_t sample_offset = 0;
  const auto run_deadline = Clock::now() + std::chrono::seconds(selected.seconds + 5);
  const auto send = [&](unsigned blocks, bool left, bool right) {
    for (unsigned at = 0; at < blocks; ++at) {
      if (Clock::now() >= run_deadline)
        throw std::runtime_error("audio check exceeded its run deadline");
      headunit::AudioMixer::Block source{};
      for (unsigned frame = 0; frame < headunit::AudioMixer::frames_per_block; ++frame) {
        const auto phase = 2. * std::numbers::pi * 440. * double(sample_offset + frame) /
                           headunit::AudioMixer::sample_rate;
        const auto sample = static_cast<std::int16_t>(std::lround(std::sin(phase) * 32767.));
        source[frame * 2] = left ? sample : 0;
        source[frame * 2 + 1] = right ? sample : 0;
      }
      const auto mixed = mixer.mix(source, {}, false);
      const auto deadline = Clock::now() + 1s;
      while (!output.submit(mixed)) {
        if (Clock::now() >= deadline || Clock::now() >= run_deadline)
          throw std::runtime_error("audio check output made no progress for one second");
        std::this_thread::sleep_for(1ms);
      }
      sample_offset += headunit::AudioMixer::frames_per_block;
    }
  };

  // Channel identification, silence, reduced user gain and starvation recovery
  // all traverse the production mixer and output. Maximum run is about 12 s.
  send(selected.seconds * 50, true, false);
  send(selected.seconds * 50, false, true);
  mixer.set_muted(true);
  send(25, true, true);
  mixer.set_muted(false);
  mixer.set_master_volume(selected.volume / 200.);
  send(25, true, true);
  const auto pause_end = Clock::now() + 250ms;
  while (Clock::now() < pause_end) {
    output.poll();
    std::this_thread::sleep_for(5ms);
  }
  mixer.set_master_volume(selected.volume / 100.);
  send(50, true, true);
  output.finish();
  if (observed.buffers.load() == 0)
    throw std::runtime_error("audio check drained without observing a sink buffer");
  std::cout << "{\"sink\":" << json_quoted(selected.sink)
            << ",\"device\":" << json_quoted(selected.device)
            << ",\"volume_percent\":" << selected.volume
            << ",\"submitted_blocks\":" << output.accepted()
            << ",\"backpressure_retries\":" << output.rejected()
            << ",\"discontinuities\":" << output.discontinuities()
            << ",\"schedule_underruns\":" << output.underruns()
            << ",\"sink_pad_buffers\":" << observed.buffers.load()
            << ",\"sink_pad_peak_available\":" << observed.peak_available.load()
            << ",\"sink_pad_nonzero_buffers\":" << observed.nonzero.load()
            << ",\"sink_pad_zero_buffers\":" << observed.zero.load()
            << ",\"sink_pad_maximum_peak\":" << observed.peak.load()
            << ",\"drained\":true,\"audible_verified\":false}\n";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Usage: headunit-audio-output-check --sink fakesink|alsasink|pulsesink\n"
                 "       [--device NAME] [--seconds 1..10] [--volume 1..10]\n"
                 "Physical sinks require an explicit device. Default tone gain is 5%.\n"
                 "Sink-pad observations do not verify audible playback.\n";
    return 0;
  }
  try {
    run(options(argc, argv));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "audio output check: " << error.what() << '\n';
    return 1;
  }
}
