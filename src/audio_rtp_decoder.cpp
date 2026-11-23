// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_rtp_decoder.hpp"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <string>

namespace headunit {
namespace {
constexpr std::size_t rtp_header = 12;
constexpr std::size_t maximum_packet = 4096;
constexpr std::size_t block_samples = AudioMixer::frames_per_block * AudioMixer::channels;
void add(std::uint64_t& value, std::uint64_t count = 1) {
  value += std::min(count, std::numeric_limits<std::uint64_t>::max() - value);
}

void initialize_gst() {
  static std::once_flag once;
  std::call_once(once, [] { gst_init(nullptr, nullptr); });
}
}  // namespace

AudioRtpDecoder::Description AudioRtpDecoder::describe(Format format) {
  switch (format) {
    case Format::aac_lc_48000_stereo:
      return {Encoding::aac, 48000, 2};
    case Format::opus_16000_mono:
      return {Encoding::opus, 16000, 1};
    case Format::opus_24000_mono:
      return {Encoding::opus, 24000, 1};
    case Format::opus_48000_mono:
      return {Encoding::opus, 48000, 1};
    case Format::pcm_8000_mono:
      return {Encoding::pcm, 8000, 1};
    case Format::pcm_8000_stereo:
      return {Encoding::pcm, 8000, 2};
    case Format::pcm_16000_mono:
      return {Encoding::pcm, 16000, 1};
    case Format::pcm_16000_stereo:
      return {Encoding::pcm, 16000, 2};
    case Format::pcm_24000_mono:
      return {Encoding::pcm, 24000, 1};
    case Format::pcm_24000_stereo:
      return {Encoding::pcm, 24000, 2};
    case Format::pcm_32000_mono:
      return {Encoding::pcm, 32000, 1};
    case Format::pcm_32000_stereo:
      return {Encoding::pcm, 32000, 2};
    case Format::pcm_48000_mono:
      return {Encoding::pcm, 48000, 1};
    case Format::pcm_48000_stereo:
      return {Encoding::pcm, 48000, 2};
  }
  throw std::invalid_argument("unsupported audio decoder format");
}

struct AudioRtpDecoder::Impl {
  GstElement* pipeline = nullptr;
  GstAppSrc* source = nullptr;
  GstAppSink* sink = nullptr;
  GstBus* bus = nullptr;
  GstElement* jitter = nullptr;
  bool sink_drop_property = false;
  audio_observation::Point observation;
  Format format;
  std::string codec;
  AudioDecoderDiagnostics counts;
  std::deque<std::int16_t> samples;
  std::string fault;

  explicit Impl(unsigned latency_ms, Format selected, audio_observation::Point observed)
      : observation(std::move(observed)), format(selected) {
    const auto selected_format = AudioRtpDecoder::describe(format);
    codec = selected_format.encoding == Encoding::aac    ? "AAC"
            : selected_format.encoding == Encoding::opus ? "Opus"
                                                         : "PCM";
    static_assert(std::endian::native == std::endian::little);
    if (latency_ms < 20 || latency_ms > 2000)
      throw std::invalid_argument("audio latency outside 20..2000 ms");
    initialize_gst();
    std::string caps, decoder;
    if (selected_format.encoding == Encoding::aac) {
      caps =
          "encoding-name=(string)MPEG4-GENERIC,mode=(string)AAC-hbr,config=(string)1190,sizelength="
          "(string)13,indexlength=(string)3,indexdeltalength=(string)3,payload=(int)96";
      decoder = "rtpmp4gdepay ! avdec_aac";
    } else if (selected_format.encoding == Encoding::opus) {
      caps =
          "encoding-name=(string)OPUS,encoding-params=(string)1,sprop-stereo=(string)0,payload=("
          "int)96";
      decoder = "rtpopusdepay ! opusdec";
    } else {
      caps = "encoding-name=(string)L16,channels=(int)" + std::to_string(selected_format.channels) +
             ",payload=(int)96";
      decoder = "rtpL16depay";
    }
    const auto rtp_rate = selected_format.encoding == Encoding::opus ? 48000 : selected_format.rate;
    const std::string description =
        "appsrc name=source is-live=true do-timestamp=true format=time "
        "block=false max-bytes=65536 ! "
        "application/x-rtp,media=(string)audio,clock-rate=(int)" +
        std::to_string(rtp_rate) + "," + caps +
        " ! rtpjitterbuffer name=jitter latency=" + std::to_string(latency_ms) +
        " drop-on-latency=true ! " + decoder +
        " ! "
        "audioconvert ! audioresample ! "
        "audio/x-raw,format=(string)S16LE,rate=(int)48000,"
        "channels=(int)2,layout=(string)interleaved ! "
        "appsink name=decoded sync=false max-buffers=64 drop=true wait-on-eos=false";
    GError* error = nullptr;
    pipeline = gst_parse_launch(description.c_str(), &error);
    if (!pipeline || error) {
      const std::string detail = error ? error->message : codec + " pipeline unavailable";
      if (error) g_error_free(error);
      if (pipeline) gst_object_unref(pipeline);
      pipeline = nullptr;
      throw std::runtime_error(detail);
    }
    gst_object_set_name(GST_OBJECT(pipeline),
                        selected_format.encoding == Encoding::aac    ? "headunit-aac"
                        : selected_format.encoding == Encoding::opus ? "headunit-opus"
                                                                     : "headunit-pcm");
    auto* input = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    auto* output = gst_bin_get_by_name(GST_BIN(pipeline), "decoded");
    jitter = gst_bin_get_by_name(GST_BIN(pipeline), "jitter");
    if (!input || !output || !jitter) {
      if (input) gst_object_unref(input);
      if (output) gst_object_unref(output);
      if (jitter) gst_object_unref(jitter);
      jitter = nullptr;
      gst_object_unref(pipeline);
      pipeline = nullptr;
      throw std::runtime_error(codec + " pipeline lacks its endpoints");
    }
    source = GST_APP_SRC(input);
    sink = GST_APP_SINK(output);
    const auto* drops = g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "dropped");
    sink_drop_property =
        drops && drops->value_type == G_TYPE_UINT64 && (drops->flags & G_PARAM_READABLE);
    if (observation.capture) {
      for (const auto* name : {"sink", "src"}) {
        auto* pad = gst_element_get_static_pad(jitter, name);
        if (pad) {
          gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, observe_packet, this, nullptr);
          gst_object_unref(pad);
        }
      }
      auto* pad = gst_element_get_static_pad(output, "sink");
      if (pad) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, observe_decoded, this, nullptr);
        gst_object_unref(pad);
      }
    }
    bus = gst_element_get_bus(pipeline);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      gst_object_unref(bus);
      gst_object_unref(source);
      gst_object_unref(sink);
      gst_object_unref(jitter);
      gst_object_unref(pipeline);
      bus = nullptr;
      pipeline = nullptr;
      jitter = nullptr;
      throw std::runtime_error(codec + " pipeline could not start");
    }
    observation.observe(audio_observation::Kind::decoder_start, std::uint64_t(format), latency_ms,
                        65536, 64, true);
  }

  static GstPadProbeReturn observe_packet(GstPad* pad, GstPadProbeInfo* info, gpointer user) {
    auto& self = *static_cast<Impl*>(user);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    std::array<std::uint8_t, 12> header{};
    if (buffer && gst_buffer_extract(buffer, 0, header.data(), header.size()) == header.size()) {
      const auto seq = (std::uint64_t(header[2]) << 8) | header[3];
      const auto time = (std::uint64_t(header[4]) << 24) | (std::uint64_t(header[5]) << 16) |
                        (std::uint64_t(header[6]) << 8) | header[7];
      const auto ssrc = (std::uint64_t(header[8]) << 24) | (std::uint64_t(header[9]) << 16) |
                        (std::uint64_t(header[10]) << 8) | header[11];
      self.observation.observe(GST_PAD_DIRECTION(pad) == GST_PAD_SINK
                                   ? audio_observation::Kind::jitter_enter
                                   : audio_observation::Kind::jitter_release,
                               seq, time, ssrc, GST_BUFFER_PTS(buffer));
    }
    return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn observe_decoded(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto& self = *static_cast<Impl*>(user);
    if (auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info))
      self.observation.observe(audio_observation::Kind::decoded_buffer, GST_BUFFER_PTS(buffer),
                               GST_BUFFER_DURATION(buffer), gst_buffer_get_size(buffer),
                               GST_BUFFER_FLAGS(buffer));
    return GST_PAD_PROBE_OK;
  }
  ~Impl() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) gst_object_unref(bus);
    if (source) gst_object_unref(source);
    if (sink) gst_object_unref(sink);
    if (jitter) gst_object_unref(jitter);
    if (pipeline) gst_object_unref(pipeline);
    observation.observe(audio_observation::Kind::decoder_stop, counts.submitted,
                        counts.input_backpressure, counts.discarded_pcm_frames, !fault.empty(),
                        true);
  }

  void error_if_any() {
    if (!fault.empty()) throw std::runtime_error(fault);
    // Ordinary notices share the same work bound as failures. Filtering for
    // errors alone can discard an unlimited notice burst in one owner turn.
    // Later turns inspect a larger burst's remaining messages without waiting.
    for (unsigned index = 0; index < 32; ++index) {
      auto* message = gst_bus_pop(bus);
      if (!message) break;
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* detail = nullptr;
        gst_message_parse_error(message, &error, &detail);
        fault = error && error->message && error->message[0] ? error->message
                                                             : codec + " decode failed";
        if (error) g_error_free(error);
        g_free(detail);
      } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        fault = codec + " decoder ended unexpectedly";
      }
      gst_message_unref(message);
      if (!fault.empty()) throw std::runtime_error(fault);
    }
  }

  bool submit(std::span<const std::uint8_t> rtp) {
    error_if_any();
    if (rtp.size() < rtp_header || rtp.size() > maximum_packet || (rtp[0] >> 6) != 2 ||
        (rtp[0] & 0x3f) != 0) {
      add(counts.invalid_packets);
      observation.observe(audio_observation::Kind::decoder_reject, 1, rtp.size());
      return false;
    }
    const auto payload = rtp.size() - rtp_header;
    const auto selected_format = AudioRtpDecoder::describe(format);
    if ((selected_format.encoding != Encoding::aac && payload == 0) ||
        (selected_format.encoding == Encoding::pcm && payload % (2 * selected_format.channels))) {
      add(counts.invalid_packets);
      observation.observe(audio_observation::Kind::decoder_reject, 1, rtp.size());
      return false;
    }
    const std::size_t header_bytes = format == Format::aac_lc_48000_stereo ? 4 : 0;
    if (gst_app_src_get_current_level_bytes(source) + rtp.size() + header_bytes > 65536) {
      if (observation.capture)
        observation.observe(audio_observation::Kind::decoder_reject, 2, rtp.size(),
                            gst_app_src_get_current_level_bytes(source));
      add(counts.input_backpressure);
      return false;
    }
    auto* buffer = gst_buffer_new_allocate(nullptr, rtp.size() + header_bytes, nullptr);
    if (!buffer) throw std::runtime_error(codec + " packet allocation failed");
    gst_buffer_fill(buffer, 0, rtp.data(), rtp_header);
    if (selected_format.encoding == Encoding::opus && selected_format.rate != 48000) {
      // The SDK timestamps packets in negotiated PCM samples. GStreamer's
      // standard Opus depayloader requires its fixed 48 kHz RTP clock.
      const auto time = (std::uint32_t(rtp[4]) << 24) | (std::uint32_t(rtp[5]) << 16) |
                        (std::uint32_t(rtp[6]) << 8) | rtp[7];
      const auto normalized = time * (48000 / selected_format.rate);
      const std::array<std::uint8_t, 4> timestamp{
          std::uint8_t(normalized >> 24), std::uint8_t(normalized >> 16),
          std::uint8_t(normalized >> 8), std::uint8_t(normalized)};
      gst_buffer_fill(buffer, 4, timestamp.data(), timestamp.size());
    }
    if (header_bytes) {
      const std::uint8_t headers[4]{0, 16, std::uint8_t(payload >> 5), std::uint8_t(payload << 3)};
      gst_buffer_fill(buffer, rtp_header, headers, sizeof(headers));
    }
    gst_buffer_fill(buffer, rtp_header + header_bytes, rtp.data() + rtp_header, payload);
    const std::uint8_t marker =
        format == Format::aac_lc_48000_stereo ? 0x80 | 96 : (rtp[1] & 0x80) | 96;
    gst_buffer_fill(buffer, 1, &marker, 1);
    if (gst_app_src_push_buffer(source, buffer) != GST_FLOW_OK) {
      observation.observe(audio_observation::Kind::decoder_reject, 3, rtp.size());
      add(counts.input_failures);
      return false;
    }
    observation.observe(audio_observation::Kind::decoder_submit,
                        (std::uint64_t(rtp[2]) << 8) | rtp[3],
                        (std::uint64_t(rtp[4]) << 24) | (std::uint64_t(rtp[5]) << 16) |
                            (std::uint64_t(rtp[6]) << 8) | rtp[7],
                        rtp.size(), counts.submitted + 1);
    add(counts.submitted);
    return true;
  }

  std::vector<AudioMixer::Block> take(std::size_t maximum) {
    error_if_any();
    maximum = std::min<std::size_t>(maximum, 32);
    // Leave decoded samples in the bounded appsink until the consumer has room
    // for another small batch. Do not ingest a burst just to discard it here.
    for (unsigned index = 0; index < 16 && samples.size() < (maximum + 4) * block_samples;
         ++index) {
      GstSample* sample = gst_app_sink_try_pull_sample(sink, 0);
      if (!sample) break;
      if (GstBuffer* buffer = gst_sample_get_buffer(sample)) {
        observation.observe(audio_observation::Kind::decoded_pull, GST_BUFFER_PTS(buffer),
                            GST_BUFFER_DURATION(buffer), gst_buffer_get_size(buffer),
                            GST_BUFFER_FLAGS(buffer));
        GstMapInfo mapped{};
        if (gst_buffer_map(buffer, &mapped, GST_MAP_READ)) {
          if (mapped.size % (sizeof(std::int16_t) * AudioMixer::channels) == 0 &&
              mapped.size <= 2 * 1024 * 1024) {
            const auto count = mapped.size / sizeof(std::int16_t);
            for (std::size_t i = 0; i < count; ++i) {
              std::int16_t value;
              std::memcpy(&value, mapped.data + i * sizeof(value), sizeof(value));
              samples.push_back(value);
            }
          } else
            add(counts.invalid_pcm_buffers);
          gst_buffer_unmap(buffer, &mapped);
        } else
          add(counts.invalid_pcm_buffers);
      } else
        add(counts.invalid_pcm_buffers);
      gst_sample_unref(sample);
    }
    if (samples.size() > 64 * block_samples) {
      const auto discarded = samples.size() - 64 * block_samples;
      add(counts.discarded_pcm_frames, discarded / AudioMixer::channels);
      samples.erase(samples.begin(), samples.begin() + discarded);
    }
    std::vector<AudioMixer::Block> blocks;
    for (; blocks.size() < maximum && samples.size() >= block_samples;) {
      AudioMixer::Block block{};
      for (auto& value : block) {
        value = samples.front();
        samples.pop_front();
      }
      blocks.push_back(std::move(block));
    }
    observation.observe(audio_observation::Kind::pcm_pull, blocks.size(),
                        samples.size() / AudioMixer::channels);
    return blocks;
  }

  AudioDecoderDiagnostics diagnostics() const {
    auto result = counts;
    GstStructure* stats = nullptr;
    g_object_get(jitter, "stats", &stats, nullptr);
    if (stats) {
      AudioJitterDiagnostics measured;
      const auto get = [&](const char* name, std::uint64_t& value) {
        guint64 number = 0;
        if (!gst_structure_get_uint64(stats, name, &number)) return false;
        value = number;
        return true;
      };
      if (get("num-pushed", measured.pushed) && get("num-lost", measured.considered_lost) &&
          get("num-late", measured.late) && get("num-duplicates", measured.duplicates) &&
          get("avg-jitter", measured.average_jitter_ns))
        result.jitter = measured;
      gst_structure_free(stats);
    }
    if (sink_drop_property) {
      guint64 dropped = 0;
      g_object_get(sink, "dropped", &dropped, nullptr);
      result.appsink_dropped_buffers = dropped;
    }
    if (result.jitter) {
      const auto& j = *result.jitter;
      observation.observe(audio_observation::Kind::jitter_counts, j.pushed, j.considered_lost,
                          j.late, j.duplicates);
    }
    observation.observe(audio_observation::Kind::decoder_counts, counts.submitted,
                        counts.discarded_pcm_frames, counts.invalid_pcm_buffers,
                        result.appsink_dropped_buffers.value_or(audio_observation::unavailable));
    return result;
  }
};

AudioRtpDecoder::AudioRtpDecoder(unsigned latency_ms, Format format,
                                 audio_observation::Point observation)
    : impl_(std::make_unique<Impl>(latency_ms, format, std::move(observation))) {}
AudioRtpDecoder::~AudioRtpDecoder() = default;
bool AudioRtpDecoder::submit(std::span<const std::uint8_t> rtp) {
  return impl_->submit(rtp);
}
std::vector<AudioMixer::Block> AudioRtpDecoder::take(std::size_t maximum) {
  return impl_->take(maximum);
}
AudioDecoderDiagnostics AudioRtpDecoder::diagnostics() const {
  return impl_->diagnostics();
}

}  // namespace headunit
