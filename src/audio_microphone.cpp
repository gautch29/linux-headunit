// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_microphone.hpp"
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>
namespace headunit {
void AudioMicrophone::validate_selection(std::string_view factory, std::string_view device) {
  if (factory != "alsasrc" && factory != "pulsesrc")
    throw std::invalid_argument("microphone requires alsasrc or pulsesrc");
  if (device.size() > 255 ||
      std::any_of(device.begin(), device.end(), [](unsigned char c) { return c < 32 || c > 126; }))
    throw std::invalid_argument("invalid microphone device");
}
struct AudioMicrophone::Impl {
  GstElement* pipeline = nullptr;
  GstElement* sink = nullptr;
  GstBus* bus = nullptr;
  std::string fault;
  std::deque<std::uint8_t> pcm;
  std::uint64_t pcm_time = 0;
  unsigned rate;
  bool opus;
  Impl(const std::string& factory, const std::string& device, unsigned sample_rate, bool compressed)
      : rate(sample_rate), opus(compressed) {
    if ((opus && rate != 16000 && rate != 24000 && rate != 48000) ||
        (!opus && rate != 8000 && rate != 16000 && rate != 24000 && rate != 32000 && rate != 48000))
      throw std::invalid_argument("unsupported microphone format");
    if (factory != "appsrc")
      AudioMicrophone::validate_selection(factory, device);
    else if (!device.empty())
      throw std::invalid_argument("fixture microphone cannot select a device");
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
    GError* error = nullptr;
    const auto description =
        factory + " name=capture " +
        (factory == "appsrc"
             ? "is-live=true format=time block=false max-bytes=16384 ! "
               "audio/x-raw,format=S16LE,rate=48000,channels=1,layout=interleaved ! "
             : "buffer-time=80000 latency-time=10000 ! ") +
        "audioconvert ! audioresample ! audio/x-raw,format=" + (opus ? "S16LE" : "S16BE") +
        ",rate=" + std::to_string(rate) + ",channels=1,layout=interleaved ! " +
        (opus ? "opusenc frame-size=20 audio-type=voice bitrate=32000 max-payload-size=1400 ! "
              : "") +
        "appsink name=encoded sync=false max-buffers=8 drop=true wait-on-eos=false";
    pipeline = gst_parse_launch(description.c_str(), &error);
    if (!pipeline || error) {
      if (error) g_error_free(error);
      if (pipeline) gst_object_unref(pipeline);
      pipeline = nullptr;
      throw std::runtime_error("microphone Opus pipeline unavailable");
    }
    auto* capture = gst_bin_get_by_name(GST_BIN(pipeline), "capture");
    if (!device.empty()) g_object_set(capture, "device", device.c_str(), nullptr);
    gst_object_unref(capture);
    gst_object_set_name(GST_OBJECT(pipeline), "headunit-microphone");
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "encoded");
    bus = gst_element_get_bus(pipeline);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      cleanup();
      throw std::runtime_error("microphone capture could not start");
    }
  }
  void cleanup() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) gst_object_unref(bus);
    if (sink) gst_object_unref(sink);
    if (pipeline) gst_object_unref(pipeline);
    pipeline = nullptr;
    bus = nullptr;
    sink = nullptr;
  }
  ~Impl() { cleanup(); }
  void check() {
    if (!fault.empty()) throw std::runtime_error(fault);
    for (unsigned i = 0; i < 32; ++i) {
      auto* message = gst_bus_pop(bus);
      if (!message) break;
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) fault = "microphone capture failed";
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) fault = "microphone capture ended";
      gst_message_unref(message);
      if (!fault.empty()) throw std::runtime_error(fault);
    }
  }
};
AudioMicrophone::AudioMicrophone(std::string factory, std::string device, unsigned sample_rate,
                                 bool opus)
    : impl_(std::make_unique<Impl>(factory, device, sample_rate, opus)) {}
AudioMicrophone::~AudioMicrophone() = default;
std::optional<AudioMicrophone::Packet> AudioMicrophone::take() {
  impl_->check();
  const auto pcm_packet_size = impl_->rate / 100 * 2;
  if (!impl_->opus && impl_->pcm.size() >= pcm_packet_size) {
    std::vector<std::uint8_t> packet;
    packet.reserve(pcm_packet_size);
    for (unsigned i = 0; i < pcm_packet_size; ++i) {
      packet.push_back(impl_->pcm.front());
      impl_->pcm.pop_front();
    }
    Packet result{std::move(packet), static_cast<std::uint32_t>(impl_->pcm_time)};
    impl_->pcm_time += impl_->rate / 100;
    return result;
  }
  auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->sink), 0);
  if (!sample) return std::nullopt;
  auto* buffer = gst_sample_get_buffer(sample);
  const auto size = gst_buffer_get_size(buffer);
  const auto pts = GST_BUFFER_PTS(buffer);
  if (!GST_CLOCK_TIME_IS_VALID(pts)) {
    gst_sample_unref(sample);
    throw std::runtime_error("microphone packet has no capture timestamp");
  }
  const auto sample_time = gst_util_uint64_scale(pts, impl_->rate, GST_SECOND);
  if (size == 0 || size > (impl_->opus ? 1400U : 8192U)) {
    gst_sample_unref(sample);
    throw std::runtime_error("microphone encoded packet exceeds bound");
  }
  std::vector<std::uint8_t> bytes(size);
  const auto read = gst_buffer_extract(buffer, 0, bytes.data(), size);
  gst_sample_unref(sample);
  if (read != size) throw std::runtime_error("microphone packet extraction failed");
  if (impl_->opus) return Packet{std::move(bytes), static_cast<std::uint32_t>(sample_time)};
  if (impl_->pcm.size() + bytes.size() > 8192)
    throw std::runtime_error("microphone PCM queue exceeds bound");
  const auto expected = impl_->pcm_time + impl_->pcm.size() / 2;
  const auto distance = sample_time > expected ? sample_time - expected : expected - sample_time;
  if (impl_->pcm.empty() || distance > 2) {
    impl_->pcm.clear();
    impl_->pcm_time = sample_time;
  }
  impl_->pcm.insert(impl_->pcm.end(), bytes.begin(), bytes.end());
  return std::nullopt;
}
}  // namespace headunit
