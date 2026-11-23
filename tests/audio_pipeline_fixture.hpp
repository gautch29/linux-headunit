// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <cstring>
#include <span>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace audio_fixture {

// Observe the production pipeline through GObject's real creation signal.
// No private AudioOutput access or substitute sink/decoder is needed. Injected
// messages qualify actual bus handling, not a hardware driver's behavior.
class OutputBus {
 public:
  explicit OutputBus(std::string pipeline_name = "headunit-audio", std::string sink_name = "output",
                     std::string source_name = "input")
      : pipeline_name_(std::move(pipeline_name)),
        sink_name_(std::move(sink_name)),
        source_name_(std::move(source_name)) {
    gst_init(nullptr, nullptr);
    auto* klass = g_type_class_ref(GST_TYPE_BIN);
    signal_ = g_signal_lookup("deep-element-added", GST_TYPE_BIN);
    g_type_class_unref(klass);
    if (!signal_) throw std::runtime_error("GStreamer creation signal unavailable");
    hook_ = g_signal_add_emission_hook(signal_, 0, created, this, nullptr);
    if (!hook_) throw std::runtime_error("GStreamer creation hook unavailable");
  }
  ~OutputBus() {
    unblock_sink();
    g_signal_remove_emission_hook(signal_, hook_);
    if (pipeline_) gst_object_unref(pipeline_);
  }
  OutputBus(const OutputBus&) = delete;
  OutputBus& operator=(const OutputBus&) = delete;

  void push_pcm(std::span<const std::int16_t> pcm, GstClockTime pts) {
    auto* pipeline = reference();
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline), source_name_.c_str());
    auto* buffer = gst_buffer_new_allocate(nullptr, pcm.size_bytes(), nullptr);
    gst_buffer_fill(buffer, 0, pcm.data(), pcm.size_bytes());
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
    const auto result = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
    gst_object_unref(source);
    gst_object_unref(pipeline);
    if (result != GST_FLOW_OK) throw std::runtime_error("fixture capture input rejected PCM");
  }
  void error(const char* text) {
    auto* pipeline = reference();
    auto* output = gst_bin_get_by_name(GST_BIN(pipeline), sink_name_.c_str());
    if (!output) {
      gst_object_unref(pipeline);
      throw std::runtime_error("production output sink missing");
    }
    auto* failure = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_WRITE, text);
    auto* message =
        gst_message_new_error(GST_OBJECT(output), failure, "controlled offline bus fault");
    g_error_free(failure);
    const bool posted = gst_element_post_message(output, message);
    gst_object_unref(output);
    gst_object_unref(pipeline);
    if (!posted) throw std::runtime_error("actual output bus rejected its error message");
  }
  void clock_lost() {
    auto* pipeline = reference();
    auto* clock = gst_element_get_clock(pipeline);
    if (!clock) {
      gst_object_unref(pipeline);
      throw std::runtime_error("production output has no clock");
    }
    const bool posted =
        gst_element_post_message(pipeline, gst_message_new_clock_lost(GST_OBJECT(pipeline), clock));
    gst_object_unref(clock);
    gst_object_unref(pipeline);
    if (!posted) throw std::runtime_error("actual output bus rejected clock loss");
  }
  GstClockTime invalidate_base_time() {
    auto* pipeline = reference();
    const auto original = gst_element_get_base_time(pipeline);
    auto* clock = gst_element_get_clock(pipeline);
    if (!clock) {
      gst_object_unref(pipeline);
      throw std::runtime_error("production output has no clock");
    }
    // GStreamer rejects NONE at this API boundary. A base later than its real
    // observed clock is API-valid but cannot define a valid running timeline.
    const auto observed = gst_clock_get_time(clock);
    gst_object_unref(clock);
    gst_element_set_base_time(pipeline, observed + GST_SECOND);
    gst_object_unref(pipeline);
    return original;
  }
  void restore_base_time(GstClockTime original) {
    auto* pipeline = reference();
    gst_element_set_base_time(pipeline, original);
    gst_object_unref(pipeline);
  }
  void advance_running_time(GstClockTime duration) {
    auto* pipeline = reference();
    const auto base = gst_element_get_base_time(pipeline);
    if (!GST_CLOCK_TIME_IS_VALID(base) || base < duration) {
      gst_object_unref(pipeline);
      throw std::runtime_error("fixture cannot advance the output running timeline");
    }
    // Explicit clock model of an owner suspension, with no wall-clock sleep.
    // Production never changes its base or invents a clock/presentation receipt.
    gst_element_set_base_time(pipeline, base - duration);
    gst_object_unref(pipeline);
  }
  void notices(unsigned count) {
    auto* pipeline = reference();
    for (unsigned index = 0; index < count; ++index) {
      if (!gst_element_post_message(
              pipeline,
              gst_message_new_application(GST_OBJECT(pipeline),
                                          gst_structure_new_empty("offline-audio-notice")))) {
        gst_object_unref(pipeline);
        throw std::runtime_error("actual output bus rejected a notice");
      }
    }
    gst_object_unref(pipeline);
  }
  void end_source() {
    auto* pipeline = reference();
    auto* input = gst_bin_get_by_name(GST_BIN(pipeline), source_name_.c_str());
    if (!input) {
      gst_object_unref(pipeline);
      throw std::runtime_error("production audio source missing");
    }
    const auto result = gst_app_src_end_of_stream(GST_APP_SRC(input));
    gst_object_unref(input);
    gst_object_unref(pipeline);
    if (result != GST_FLOW_OK) throw std::runtime_error("actual audio source did not accept EOS");
  }
  void ended() {
    auto* pipeline = reference();
    const bool posted =
        gst_element_post_message(pipeline, gst_message_new_eos(GST_OBJECT(pipeline)));
    gst_object_unref(pipeline);
    if (!posted) throw std::runtime_error("actual audio bus rejected EOS");
  }
  unsigned pending_messages(bool application_only = false) {
    auto* pipeline = reference();
    auto* bus = gst_element_get_bus(pipeline);
    unsigned count = 0;
    while (auto* message = gst_bus_pop(bus)) {
      if (!application_only || GST_MESSAGE_TYPE(message) == GST_MESSAGE_APPLICATION) ++count;
      gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return count;
  }
  guint64 queued_input_bytes() {
    auto* pipeline = reference();
    auto* source = gst_bin_get_by_name(GST_BIN(pipeline), source_name_.c_str());
    if (!source) {
      gst_object_unref(pipeline);
      throw std::runtime_error("production output source missing");
    }
    const auto queued = gst_app_src_get_current_level_bytes(GST_APP_SRC(source));
    gst_object_unref(source);
    gst_object_unref(pipeline);
    return queued;
  }
  void block_sink() {
    if (blocked_pad_) throw std::runtime_error("test output sink already blocked");
    auto* pipeline = reference();
    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), sink_name_.c_str());
    if (sink) blocked_pad_ = gst_element_get_static_pad(sink, "sink");
    if (sink) gst_object_unref(sink);
    gst_object_unref(pipeline);
    if (!blocked_pad_) throw std::runtime_error("production output sink pad missing");
    blocked_probe_ = gst_pad_add_probe(
        blocked_pad_, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
        [](GstPad*, GstPadProbeInfo*, gpointer) { return GST_PAD_PROBE_OK; }, nullptr, nullptr);
    if (!blocked_probe_) throw std::runtime_error("production output pad could not be blocked");
  }
  void unblock_sink() {
    if (blocked_pad_) {
      if (blocked_probe_) gst_pad_remove_probe(blocked_pad_, blocked_probe_);
      gst_object_unref(blocked_pad_);
      blocked_pad_ = nullptr;
      blocked_probe_ = 0;
    }
  }

 private:
  guint signal_ = 0;
  gulong hook_ = 0;
  std::mutex mutex_;
  GstElement* pipeline_ = nullptr;
  std::string pipeline_name_, sink_name_, source_name_;
  GstPad* blocked_pad_ = nullptr;
  gulong blocked_probe_ = 0;

  GstElement* reference() {
    std::lock_guard lock(mutex_);
    if (!pipeline_) throw std::runtime_error("production audio pipeline was not observed");
    return GST_ELEMENT(gst_object_ref(pipeline_));
  }
  static gboolean created(GSignalInvocationHint*, guint count, const GValue* values,
                          gpointer user) noexcept {
    try {
      if (count != 3) return TRUE;
      auto* bin = static_cast<GstElement*>(g_value_get_object(&values[0]));
      auto* element = static_cast<GstElement*>(g_value_get_object(&values[2]));
      auto& self = *static_cast<OutputBus*>(user);
      if (!bin || !element || !GST_IS_PIPELINE(bin) ||
          (!self.pipeline_name_.empty() &&
           std::strcmp(GST_OBJECT_NAME(bin), self.pipeline_name_.c_str())) ||
          std::strcmp(GST_OBJECT_NAME(element), self.sink_name_.c_str()))
        return TRUE;
      std::lock_guard lock(self.mutex_);
      if (!self.pipeline_) self.pipeline_ = GST_ELEMENT(gst_object_ref(bin));
    } catch (...) {
      // A C signal callback must not throw. reference() detects missing capture.
    }
    return TRUE;
  }
};

}  // namespace audio_fixture
