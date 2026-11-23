// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_output.hpp>
#include "audio_pipeline_fixture.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void check(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}
template <class Function>
void reject(Function function) {
  bool rejected = false;
  try {
    function();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid audio selection was accepted");
}
template <class Function>
void fault(Function function, const char* expected) {
  bool failed = false;
  try {
    function();
  } catch (const std::runtime_error& error) {
    check(std::string(error.what()) == expected, "wrong actual audio bus failure reported");
    failed = true;
  }
  check(failed, "failed audio output still reported healthy");
}

// Observe the actual production sink, then fault only when its real downstream
// EOS arrives. finish() must already have requested that EOS; no delay or
// producer-thread timing guess decides when the injected error becomes visible.
class DrainFault {
 public:
  DrainFault() {
    gst_init(nullptr, nullptr);
    auto* klass = g_type_class_ref(GST_TYPE_BIN);
    signal_ = g_signal_lookup("deep-element-added", GST_TYPE_BIN);
    g_type_class_unref(klass);
    check(signal_ != 0, "GStreamer creation signal unavailable");
    hook_ = g_signal_add_emission_hook(signal_, 0, created, this, nullptr);
    check(hook_ != 0, "GStreamer drain-fault hook unavailable");
  }
  ~DrainFault() {
    g_signal_remove_emission_hook(signal_, hook_);
    if (pad_) {
      gst_pad_remove_probe(pad_, probe_);
      gst_object_unref(pad_);
    }
  }
  DrainFault(const DrainFault&) = delete;
  DrainFault& operator=(const DrainFault&) = delete;
  bool observed() const { return eos_observed_.load(); }

 private:
  guint signal_ = 0;
  gulong hook_ = 0, probe_ = 0;
  GstPad* pad_ = nullptr;
  std::atomic<bool> eos_observed_{false};
  static gboolean created(GSignalInvocationHint*, guint count, const GValue* values,
                          gpointer user) noexcept {
    if (count != 3) return TRUE;
    auto* bin = static_cast<GstElement*>(g_value_get_object(&values[0]));
    auto* element = static_cast<GstElement*>(g_value_get_object(&values[2]));
    if (!bin || !element || !GST_IS_PIPELINE(bin) ||
        std::strcmp(GST_OBJECT_NAME(bin), "headunit-audio") ||
        std::strcmp(GST_OBJECT_NAME(element), "output"))
      return TRUE;
    auto& self = *static_cast<DrainFault*>(user);
    if (self.pad_) return TRUE;
    self.pad_ = gst_element_get_static_pad(element, "sink");
    if (self.pad_)
      self.probe_ =
          gst_pad_add_probe(self.pad_, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, eos, &self, nullptr);
    return TRUE;
  }
  static GstPadProbeReturn eos(GstPad* pad, GstPadProbeInfo* info, gpointer user) noexcept {
    auto* event = GST_PAD_PROBE_INFO_EVENT(info);
    if (!event || GST_EVENT_TYPE(event) != GST_EVENT_EOS) return GST_PAD_PROBE_OK;
    auto& self = *static_cast<DrainFault*>(user);
    self.eos_observed_ = true;
    auto* sink = gst_pad_get_parent_element(pad);
    if (sink) {
      auto* error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_WRITE,
                                        "offline output fault during drain");
      auto* message = gst_message_new_error(GST_OBJECT(sink), error, "controlled drain fault");
      g_error_free(error);
      (void)gst_element_post_message(sink, message);
      gst_object_unref(sink);
    }
    // This controlled failed sink never acknowledges a successful drain.
    return GST_PAD_PROBE_DROP;
  }
};
}  // namespace

int main() try {
  headunit::AudioOutput::validate_selection("alsasink", "hw:CARD=Headunit,DEV=0");
  headunit::AudioOutput::validate_selection("pulsesink", "alsa_output.usb-Headunit.analog-stereo");
  reject([] { headunit::AudioOutput::validate_selection("autoaudiosink", {}); });
  reject([] { headunit::AudioOutput::validate_selection("fakesink", "default"); });
  reject([] { headunit::AudioOutput::validate_selection("alsasink", "bad\nroute"); });
  reject([] { headunit::AudioOutput::validate_selection("alsasink", std::string(256, 'x')); });
  headunit::AudioMixer mixer;
  std::mutex observed_mutex;
  std::vector<headunit::AudioOutput::Receipt> observed;
  headunit::AudioOutput output("fakesink", {}, [&](const auto& receipt) {
    std::lock_guard lock(observed_mutex);
    observed.push_back(receipt);
  });
  headunit::AudioMixer::Block media{}, guidance{};
  for (unsigned frame = 0; frame < headunit::AudioMixer::frames_per_block; ++frame) {
    const auto sample = std::int16_t(8000 * std::sin(frame * .08));
    media[frame * 2] = media[frame * 2 + 1] = sample;
    guidance[frame * 2] = guidance[frame * 2 + 1] = 2000;
  }
  const auto start = std::chrono::steady_clock::now();
  for (unsigned n = 0; n < 20; ++n) {
    const auto block = mixer.mix(media,
                                 n >= 5 && n < 10 ? std::span<const std::int16_t>(guidance)
                                                  : std::span<const std::int16_t>{},
                                 n >= 5 && n < 10);
    bool accepted = false;
    for (unsigned attempt = 0; attempt < 100 && !accepted; ++attempt) {
      accepted = output.submit(block);
      if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!accepted) throw std::runtime_error("silent test sink did not drain audio blocks");
  }
  output.finish();
  check(output.accepted() == 20, "audio block accounting changed");
  check(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(150),
        "silent output bypassed real-time PCM pacing");
  {
    std::lock_guard lock(observed_mutex);
    check(observed.size() == 20, "sink did not receive each accepted PCM block");
    for (std::size_t i = 0; i < observed.size(); ++i) {
      const auto& receipt = observed[i];
      check(receipt.duration_ns == 10000000 && receipt.bytes == sizeof(headunit::AudioMixer::Block),
            "sink received wrong PCM duration or byte count");
      if (i)
        check(!receipt.discontinuity &&
                  receipt.presentation_ns == observed[i - 1].presentation_ns + 10000000,
              "decoder burst lost contiguous sample times at the sink");
    }
    check(observed.front().discontinuity && output.discontinuities() == 1,
          "initial PCM origin was not marked once");
  }
  check(!output.submit(media), "finished audio sink accepted PCM");
  const auto finished_underruns = output.underruns();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  output.poll();
  check(output.underruns() == finished_underruns, "finished output counted a new underrun");
  // A burst cannot hide a long queue downstream of appsrc. Compare sink
  // receipts, not just a producer's claimed timestamp/counter.
  observed.clear();
  {
    headunit::AudioOutput burst("fakesink", {}, [&](const auto& receipt) {
      std::lock_guard lock(observed_mutex);
      observed.push_back(receipt);
    });
    for (unsigned i = 0; i < 32; ++i)
      (void)burst.submit(media);
    check(burst.accepted() <= 8 && burst.rejected() > 0,
          "audio burst exceeded bounded playback horizon");
    burst.finish();
    std::lock_guard lock(observed_mutex);
    check(observed.size() == burst.accepted(), "backpressure erased accepted sink buffers");
  }
  // Model a 50 ms scheduling interruption with only one accepted source block.
  // Its real pipeline schedule must retain the next contiguous sample time.
  // The prior 20 ms startup/resume lead exhausts here and marks a new origin.
  {
    audio_fixture::OutputBus actual_bus;
    headunit::AudioOutput delayed("fakesink");
    check(delayed.submit(media), "delayed-owner first block rejected");
    check(delayed.timing().scheduled_ahead_ns == 70000000,
          "startup reserve is not the bounded 60 ms lead plus one block");
    actual_bus.advance_running_time(50 * GST_MSECOND);
    delayed.poll();
    check(!delayed.starved() && delayed.underruns() == 0,
          "bounded owner delay exhausted the startup reserve");
    check(delayed.timing().maximum_owner_gap_ns >= 50000000 &&
              delayed.timing().scheduled_ahead_ns <= 20000000,
          "owner delay and remaining PCM horizon were not measured");
    check(delayed.submit(media) && delayed.discontinuities() == 1,
          "owner delay broke the contiguous sample schedule");
    actual_bus.advance_running_time(100 * GST_MSECOND);
    delayed.poll();
    const auto timing = delayed.timing();
    check(delayed.starved() && delayed.underruns() == 1 && timing.scheduled_ahead_ns == 0 &&
              timing.last_underrun_owner_gap_ns >= 100000000 && timing.last_underrun_late_ns > 0,
          "real exhaustion was concealed or lacked owner-gap diagnostics");
    delayed.poll();
    check(delayed.underruns() == 1 &&
              delayed.timing().last_underrun_owner_gap_ns == timing.last_underrun_owner_gap_ns,
          "repeated polls rewrote the exhausted interval's timing");
    check(delayed.submit(media) && delayed.discontinuities() == 2,
          "exhausted interval did not restart with a new origin");
    delayed.finish();
  }
  // A blocked streaming path can exhaust the local schedule while real PCM
  // remains queued. Keep that evidence at the episode, even after recovery.
  {
    audio_fixture::OutputBus actual_bus;
    headunit::AudioOutput blocked("fakesink");
    actual_bus.block_sink();
    struct Unblock {
      audio_fixture::OutputBus& bus;
      ~Unblock() { bus.unblock_sink(); }
    } unblock{actual_bus};
    check(blocked.submit(media) && blocked.submit(media), "blocked-queue PCM rejected");
    actual_bus.advance_running_time(100 * GST_MSECOND);
    blocked.poll();
    const auto queued = blocked.timing();
    check(blocked.starved() && queued.queued_bytes > 0 &&
              queued.queued_bytes <= 8 * sizeof(headunit::AudioMixer::Block) &&
              queued.last_underrun_queued_bytes == queued.queued_bytes,
          "exhaustion concealed buffered PCM on a blocked output path");
    actual_bus.unblock_sink();
    blocked.finish();
    check(blocked.timing().last_underrun_queued_bytes == queued.last_underrun_queued_bytes,
          "drain rewrote the last exhaustion's queue observation");
  }
  // After genuine starvation, do not send the next block with an old PTS.
  observed.clear();
  {
    headunit::AudioOutput starved("fakesink", {}, [&](const auto& receipt) {
      std::lock_guard lock(observed_mutex);
      observed.push_back(receipt);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    starved.poll();
    check(!starved.starved() && starved.underruns() == 0,
          "initial source absence counted as an underrun");
    check(starved.submit(media), "first starvation fixture block rejected");
    starved.poll();
    check(!starved.starved() && starved.underruns() == 0,
          "starting PCM lead counted as an underrun");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    starved.poll();
    check(starved.starved() && starved.underruns() == 1,
          "idle owner did not observe exhausted PCM schedule");
    for (unsigned turn = 0; turn < 10; ++turn)
      starved.poll();
    check(starved.underruns() == 1, "repeated idle polls counted the same gap again");
    check(starved.submit(media), "post-starvation block rejected");
    check(!starved.starved() && starved.underruns() == 1,
          "accepted recovery did not end the starvation episode");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    starved.poll();
    check(starved.starved() && starved.underruns() == 2,
          "second exhausted PCM schedule was not a distinct episode");
    starved.finish();
    std::lock_guard lock(observed_mutex);
    check(observed.size() == 2 && observed[0].discontinuity && observed[1].discontinuity &&
              observed[1].presentation_ns - observed[0].presentation_ns >= 100000000 &&
              starved.discontinuities() == 2,
          "starvation reused an obsolete PCM schedule");
  }
  // A paused/starved stream has no submit call to discover a hardware failure.
  // Post an actual message from the real sink to the retained production bus.
  {
    audio_fixture::OutputBus actual_bus;
    headunit::AudioOutput idle("fakesink");
    idle.poll();
    actual_bus.error("offline output device disappeared");
    fault([&] { idle.poll(); }, "offline output device disappeared");
    fault([&] { idle.poll(); }, "offline output device disappeared");
    fault([&] { (void)idle.submit(media); }, "offline output device disappeared");
    fault([&] { idle.finish(); }, "offline output device disappeared");
    check(idle.accepted() == 0, "idle failure invented a PCM receipt");
  }
  // CLOCK_LOST must not disappear behind an ERROR-only filtered bus pop.
  {
    audio_fixture::OutputBus actual_bus;
    headunit::AudioOutput lost_clock("fakesink");
    lost_clock.poll();
    actual_bus.clock_lost();
    fault([&] { lost_clock.poll(); }, "audio output lost its playback clock");
    fault([&] { (void)lost_clock.submit(media); }, "audio output lost its playback clock");
    fault([&] { lost_clock.finish(); }, "audio output lost its playback clock");
  }
  {
    // A directly invalid pipeline clock also retires the timeline. Restoring
    // its original observed base cannot revive this failed output generation.
    audio_fixture::OutputBus actual_bus;
    headunit::AudioOutput invalid_clock("fakesink");
    check(invalid_clock.submit(media), "invalid-clock fixture did not begin PCM");
    const auto original = actual_bus.invalidate_base_time();
    fault([&] { invalid_clock.poll(); }, "audio output clock is invalid");
    actual_bus.restore_base_time(original);
    fault([&] { invalid_clock.poll(); }, "audio output clock is invalid");
    fault([&] { (void)invalid_clock.submit(media); }, "audio output clock is invalid");
    check(invalid_clock.underruns() == 0, "invalid clock became a recoverable underrun");
  }
  // Each nonblocking poll has a fixed work bound; later faults remain queued
  // and become terminal instead of being discarded by an unrelated filter.
  {
    audio_fixture::OutputBus actual_bus;
    headunit::AudioOutput bounded("fakesink");
    bounded.poll();
    actual_bus.notices(64);
    actual_bus.error("offline queued output fault");
    bounded.poll();  // Cannot inspect 64 notices and the fault in one turn.
    bool failed = false;
    for (unsigned turn = 0; turn < 4 && !failed; ++turn) {
      try {
        bounded.poll();
      } catch (const std::runtime_error& error) {
        check(std::string(error.what()) == "offline queued output fault",
              "queued audio fault changed");
        failed = true;
      }
    }
    check(failed, "bounded audio polls lost a later output fault");
  }
  // An unexpected actual source EOS is terminal; an intentional finish above
  // still drains successfully and can be polled afterward.
  {
    audio_fixture::OutputBus actual_bus;
    headunit::AudioOutput ended("fakesink");
    check(ended.submit(media), "EOS fixture block was not accepted");
    actual_bus.end_source();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool failed = false;
    while (!failed && std::chrono::steady_clock::now() < deadline) {
      try {
        ended.poll();
      } catch (const std::runtime_error& error) {
        check(std::string(error.what()) == "audio output ended before stream teardown",
              "wrong unexpected EOS fault");
        failed = true;
      }
      if (!failed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(failed, "unexpected actual audio EOS remained healthy");
    fault([&] { ended.poll(); }, "audio output ended before stream teardown");
    fault([&] { (void)ended.submit(media); }, "audio output ended before stream teardown");
    fault([&] { ended.finish(); }, "audio output ended before stream teardown");
  }
  output.poll();  // Completed intentional EOS is not an unexpected stream fault.
  {
    DrainFault actual_drain;
    headunit::AudioOutput draining("fakesink");
    check(draining.submit(media), "drain-fault fixture block was not accepted");
    fault([&] { draining.finish(); }, "offline output fault during drain");
    check(actual_drain.observed(), "explicit finish did not reach the actual sink EOS probe");
    fault([&] { draining.poll(); }, "offline output fault during drain");
    fault([&] { (void)draining.submit(media); }, "offline output fault during drain");
    fault([&] { draining.finish(); }, "offline output fault during drain");
    check(draining.accepted() == 1 && draining.rejected() == 0,
          "failed drain accepted or disguised a new PCM block as queue backpressure");
  }
  {
    headunit::AudioOutput fresh("fakesink");
    fresh.poll();
    check(fresh.accepted() == 0 && fresh.discontinuities() == 0 && fresh.underruns() == 0 &&
              !fresh.starved(),
          "new output inherited retired state");
    check(fresh.submit(media), "fresh output could not accept PCM after separate retired outputs");
    fresh.finish();
    fresh.poll();
    fresh.finish();
    check(!fresh.submit(media), "successful finished output accepted a new PCM block");
    check(fresh.accepted() == 1 && fresh.discontinuities() == 1,
          "new output did not own a fresh PCM origin");
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
