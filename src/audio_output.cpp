// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_output.hpp>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace headunit {
namespace {
constexpr std::size_t block_bytes = sizeof(AudioMixer::Block);
constexpr std::size_t queue_bytes = 8 * block_bytes;
constexpr GstClockTime block_duration = 10 * GST_MSECOND;
constexpr GstClockTime start_lead = 60 * GST_MSECOND;
constexpr GstClockTime maximum_lead = 8 * block_duration;
void increment(std::uint64_t& value) {
  if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
void initialize() {
  static std::once_flag once;
  std::call_once(once, [] { gst_init(nullptr, nullptr); });
}
}  // namespace
struct AudioOutput::Impl {
  GstElement* pipeline = nullptr;
  GstAppSrc* source = nullptr;
  GstBus* bus = nullptr;
  std::uint64_t accepted = 0, rejected = 0, discontinuities = 0;
  std::uint64_t underruns = 0;
  bool starved = false;
  std::optional<GstClockTime> next_presentation;
  std::optional<GstClockTime> last_owner_turn;
  audio_control::OutputTiming timing;
  Observer observer;
  audio_observation::Point observation;
  bool finished = false;
  std::string fault;
  explicit Impl(const std::string& factory, const std::string& device, Observer observed,
                audio_observation::Point point)
      : observer(std::move(observed)), observation(std::move(point)) {
    AudioOutput::validate_selection(factory, device);
    initialize();
    pipeline = gst_pipeline_new("headunit-audio");
    auto* input = gst_element_factory_make("appsrc", "input");
    auto* convert = gst_element_factory_make("audioconvert", "convert");
    auto* resample = gst_element_factory_make("audioresample", "resample");
    auto* sink = gst_element_factory_make(factory.c_str(), "output");
    if (!pipeline || !input || !convert || !resample || !sink) {
      if (input) gst_object_unref(input);
      if (convert) gst_object_unref(convert);
      if (resample) gst_object_unref(resample);
      if (sink) gst_object_unref(sink);
      if (pipeline) gst_object_unref(pipeline);
      pipeline = nullptr;
      throw std::runtime_error("requested audio output plugin is unavailable");
    }
    gst_bin_add_many(GST_BIN(pipeline), input, convert, resample, sink, nullptr);
    try {
      check(gst_element_link_many(input, convert, resample, sink, nullptr),
            "cannot link audio output pipeline");
      source = GST_APP_SRC(input);
      GstCaps* caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate",
                                          G_TYPE_INT, int(AudioMixer::sample_rate), "channels",
                                          G_TYPE_INT, int(AudioMixer::channels), "layout",
                                          G_TYPE_STRING, "interleaved", nullptr);
      gst_app_src_set_caps(source, caps);
      gst_caps_unref(caps);
      g_object_set(input, "is-live", TRUE, "do-timestamp", FALSE, "format", GST_FORMAT_TIME,
                   "block", FALSE, "max-bytes", guint64(queue_bytes), "emit-signals", FALSE,
                   nullptr);
      // Keep a 60 ms startup/resume reserve for delayed owner turns. It stays
      // within the existing 80 ms admission horizon and adds no synthetic PCM.
      // A decoder may return several consecutive blocks at once. Arrival time
      // is not their sample time: explicitly schedule them at 10 ms intervals.
      // The silent sink also obeys this clock so offline tests exercise pacing.
      g_object_set(sink, "sync", TRUE, nullptr);
      if (!device.empty()) g_object_set(sink, "device", device.c_str(), nullptr);
      if (observer) {
        auto* pad = gst_element_get_static_pad(sink, "sink");
        check(pad != nullptr, "audio output lacks a sink pad");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, observe, this, nullptr);
        gst_object_unref(pad);
      }
      bus = gst_element_get_bus(pipeline);
      check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
            "audio output could not start");
      check(gst_element_get_state(pipeline, nullptr, nullptr, 250 * GST_MSECOND) !=
                GST_STATE_CHANGE_FAILURE,
            "audio output could not reach playing state");
      errors();
      observation.observe(audio_observation::Kind::route_start, start_lead, maximum_lead,
                          queue_bytes, audio_observation::unavailable, true);
    } catch (...) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      if (bus) gst_object_unref(bus);
      gst_object_unref(pipeline);
      pipeline = nullptr;
      throw;
    }
  }
  ~Impl() {
    observation.observe(audio_observation::Kind::route_stop, accepted, underruns, discontinuities,
                        !fault.empty(), true);
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) gst_object_unref(bus);
    if (pipeline) gst_object_unref(pipeline);
  }
  void errors() {
    if (!fault.empty()) throw std::runtime_error(fault);
    // One bus owner, with bounded nonblocking work. A filtered pop could erase
    // CLOCK_LOST/EOS and allow PCM to continue on an invalid output timeline.
    for (unsigned turn = 0; turn < 32; ++turn) {
      auto* message = gst_bus_pop(bus);
      if (!message) break;
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        fault = error && error->message && *error->message ? error->message
                                                           : "unknown audio output error";
        if (error) g_error_free(error);
        g_free(debug);
      } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_CLOCK_LOST) {
        fault = "audio output lost its playback clock";
      } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        fault = "audio output ended before stream teardown";
      }
      gst_message_unref(message);
      if (!fault.empty()) throw std::runtime_error(fault);
    }
  }
  void poll() {
    if (!fault.empty()) throw std::runtime_error(fault);
    if (!finished) {
      errors();
      if (next_presentation) observe_schedule(running_time());
    }
  }
  static GstPadProbeReturn observe(GstPad* pad, GstPadProbeInfo* info, gpointer user) {
    auto& self = *static_cast<Impl*>(user);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer) {
      try {
        Receipt receipt{GST_BUFFER_PTS(buffer), GST_BUFFER_DURATION(buffer),
                        gst_buffer_get_size(buffer),
                        bool(GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT))};
        auto* caps = gst_pad_get_current_caps(pad);
        if (caps && gst_caps_get_size(caps) == 1 && receipt.bytes == block_bytes) {
          const auto* structure = gst_caps_get_structure(caps, 0);
          const auto* format = gst_structure_get_string(structure, "format");
          int channels = 0, rate = 0;
          if (format && std::string_view(format) == "S16LE" &&
              gst_structure_get_int(structure, "channels", &channels) && channels == 2 &&
              gst_structure_get_int(structure, "rate", &rate) && rate == 48000) {
            GstMapInfo mapping{};
            if (gst_buffer_map(buffer, &mapping, GST_MAP_READ)) {
              unsigned peak = 0;
              for (std::size_t at = 0; at + 1 < std::min(mapping.size, block_bytes); at += 2) {
                const unsigned bits =
                    unsigned(mapping.data[at]) | (unsigned(mapping.data[at + 1]) << 8);
                const int sample = bits >= 32768 ? int(bits) - 65536 : int(bits);
                peak = std::max(peak, unsigned(sample < 0 ? -sample : sample));
              }
              if (mapping.size == block_bytes) receipt.peak_sample = peak;
              gst_buffer_unmap(buffer, &mapping);
            }
          }
        }
        if (caps) gst_caps_unref(caps);
        self.observer(receipt);
      } catch (...) {
        // Diagnostic consumers never interrupt playback or cross the C ABI.
      }
    }
    return GST_PAD_PROBE_OK;
  }
  GstClockTime running_time() {
    auto* clock = gst_element_get_clock(pipeline);
    if (!clock) {
      fault = "audio output has no playback clock";
      throw std::runtime_error(fault);
    }
    const auto time = gst_clock_get_time(clock);
    gst_object_unref(clock);
    const auto base = gst_element_get_base_time(pipeline);
    if (!GST_CLOCK_TIME_IS_VALID(base) || !GST_CLOCK_TIME_IS_VALID(time) || time < base) {
      fault = "audio output clock is invalid";
      throw std::runtime_error(fault);
    }
    return time - base;
  }
  void observe_schedule(GstClockTime now) {
    timing.queued_bytes = gst_app_src_get_current_level_bytes(source);
    const auto gap = last_owner_turn && now >= *last_owner_turn ? now - *last_owner_turn : 0;
    timing.maximum_owner_gap_ns = std::max(timing.maximum_owner_gap_ns, gap);
    last_owner_turn = now;
    timing.scheduled_ahead_ns =
        next_presentation && *next_presentation > now ? *next_presentation - now : 0;
    // No accepted PCM means no playout interval to exhaust.
    if (next_presentation && !starved && *next_presentation < now) {
      increment(underruns);
      timing.last_underrun_owner_gap_ns = gap;
      timing.last_underrun_late_ns = now - *next_presentation;
      timing.last_underrun_queued_bytes = timing.queued_bytes;
      starved = true;
      observation.observe(audio_observation::Kind::output_exhausted, now, *next_presentation,
                          timing.queued_bytes, underruns);
    }
  }
  bool submit(const AudioMixer::Block& block) {
    if (!fault.empty()) throw std::runtime_error(fault);
    if (finished) {
      increment(rejected);
      return false;
    }
    poll();
    if (gst_app_src_get_current_level_bytes(source) + block_bytes > queue_bytes) {
      increment(rejected);
      return false;
    }
    const auto now = running_time();
    observe_schedule(now);
    const bool restart = !next_presentation || *next_presentation < now;
    const auto presentation = restart ? now + start_lead : *next_presentation;
    // Bound scheduled time as well as appsrc bytes: downstream consumption
    // must not turn an upstream burst into seconds of hidden audio latency.
    if (presentation - now >= maximum_lead) {
      increment(rejected);
      return false;
    }
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, block_bytes, nullptr);
    check(buffer != nullptr, "cannot allocate audio block");
    gst_buffer_fill(buffer, 0, block.data(), block_bytes);
    GST_BUFFER_PTS(buffer) = presentation;
    GST_BUFFER_DTS(buffer) = presentation;
    GST_BUFFER_DURATION(buffer) = block_duration;
    GST_BUFFER_OFFSET(buffer) = accepted * AudioMixer::frames_per_block;
    GST_BUFFER_OFFSET_END(buffer) = (accepted + 1) * AudioMixer::frames_per_block;
    if (restart) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
    const auto result = gst_app_src_push_buffer(source, buffer);
    if (result != GST_FLOW_OK) {
      increment(rejected);
      return false;
    }
    next_presentation = presentation + block_duration;
    timing.scheduled_ahead_ns = *next_presentation - now;
    timing.queued_bytes = gst_app_src_get_current_level_bytes(source);
    starved = false;
    if (restart) increment(discontinuities);
    increment(accepted);
    observation.observe(audio_observation::Kind::output_accept, presentation, now, accepted,
                        restart);
    return true;
  }
  void finish() {
    if (finished) {
      if (!fault.empty()) throw std::runtime_error(fault);
      return;
    }
    errors();
    finished = true;
    if (gst_app_src_end_of_stream(source) != GST_FLOW_OK) {
      fault = "audio output could not drain";
      throw std::runtime_error(fault);
    }
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 2 * GST_SECOND,
        GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_CLOCK_LOST));
    if (!message) {
      fault = "audio output drain timed out";
      throw std::runtime_error(fault);
    }
    const auto type = GST_MESSAGE_TYPE(message);
    if (type == GST_MESSAGE_ERROR) {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      fault = error && error->message && *error->message ? error->message
                                                         : "audio output failed while draining";
      if (error) g_error_free(error);
      g_free(debug);
    } else if (type == GST_MESSAGE_CLOCK_LOST) {
      fault = "audio output lost its playback clock";
    }
    gst_message_unref(message);
    if (!fault.empty()) throw std::runtime_error(fault);
  }
};
void AudioOutput::validate_selection(std::string_view factory, std::string_view device) {
  if (factory != "fakesink" && factory != "alsasink" && factory != "pulsesink")
    throw std::invalid_argument("unsupported audio sink factory");
  if (device.size() > 255 ||
      std::any_of(device.begin(), device.end(), [](unsigned char c) { return c < 32 || c > 126; }))
    throw std::invalid_argument("audio device must contain at most 255 printable ASCII characters");
  if (factory == "fakesink" && !device.empty())
    throw std::invalid_argument("silent audio sink has no device selection");
}
AudioOutput::AudioOutput(std::string sink_factory, std::string device, Observer observer,
                         audio_observation::Point observation)
    : impl_(std::make_unique<Impl>(sink_factory, device, std::move(observer),
                                   std::move(observation))) {}
AudioOutput::~AudioOutput() = default;
bool AudioOutput::submit(const AudioMixer::Block& block) {
  return impl_->submit(block);
}
void AudioOutput::poll() {
  impl_->poll();
}
void AudioOutput::finish() {
  impl_->finish();
}
std::uint64_t AudioOutput::accepted() const {
  return impl_->accepted;
}
std::uint64_t AudioOutput::rejected() const {
  return impl_->rejected;
}
std::uint64_t AudioOutput::discontinuities() const {
  return impl_->discontinuities;
}
std::uint64_t AudioOutput::underruns() const {
  return impl_->underruns;
}
bool AudioOutput::starved() const {
  return impl_->starved;
}
audio_control::OutputTiming AudioOutput::timing() const {
  return impl_->timing;
}
}  // namespace headunit
