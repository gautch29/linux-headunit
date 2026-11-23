// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/video_decoder.hpp>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/video/video-info-dma.h>
#include <gst/allocators/gstdmabuf.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <stdexcept>

namespace headunit::theme::video {
namespace {
void check(bool ok, const char* reason) {
  if (!ok) throw std::runtime_error(reason);
}
constexpr std::uint64_t input_bytes = 8 * 1024 * 1024, input_frames = 8, pending_frames = 64;
using Buffer = std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)>;
using Caps = std::unique_ptr<GstCaps, decltype(&gst_caps_unref)>;
using Sample = std::unique_ptr<GstSample, decltype(&gst_sample_unref)>;
std::uint64_t nanoseconds(std::uint64_t ntp) {
  return (ntp >> 32) * 1000000000ULL + ((ntp & 0xffffffffULL) * 1000000000ULL >> 32);
}
std::uint32_t import_stride(const GstVideoMeta& layout, unsigned plane) {
  const auto* format = gst_video_format_get_info(layout.format);
  check(format && layout.stride[plane] > 0, "invalid native video stride");
  std::uint64_t result = layout.stride[plane];
  if (GST_VIDEO_FORMAT_INFO_IS_TILED(format)) {
    // GStreamer encodes tile counts in VideoMeta, not an EGL pitch. Pi's
    // 1.26 packages backport COLUMN (mode 3) from 1.28; using its ABI value
    // also permits building against unpatched 1.24 headers. Match the Pi
    // gsteglimage conversion: SAND's unparameterized modifier uses column
    // height in lines, independently for the luma and chroma planes.
    constexpr auto column = GST_VIDEO_TILE_MAKE_MODE(3, INDEXED);
    if (format->tile_mode == column)
      result = std::uint64_t(GST_VIDEO_TILE_Y_TILES(layout.stride[plane])) *
               GST_VIDEO_FORMAT_INFO_TILE_HEIGHT(format, plane);
    else if (format->tile_mode == GST_VIDEO_TILE_MODE_LINEAR ||
             format->tile_mode == GST_VIDEO_TILE_MODE_ZFLIPZ_2X2)
      result = std::uint64_t(GST_VIDEO_TILE_X_TILES(layout.stride[plane])) *
               GST_VIDEO_FORMAT_INFO_TILE_STRIDE(format, plane);
    else
      throw std::runtime_error("unsupported native video tile mode");
  }
  check(result > 0 && result <= 0x7fffffff, "native video stride exceeds import limit");
  return std::uint32_t(result);
}
}  // namespace
struct Decoder::Impl {
  DecoderOptions options;
  GstElement* pipeline = nullptr;
  GstElement* source = nullptr;
  GstElement* sink = nullptr;
  GstBus* bus = nullptr;
  std::string name;
  struct SourceTimestamp {
    std::uint64_t ntp, ns;
  };
  // GStreamer needs one PTS per pending access unit. The phone can repeat a
  // source timestamp; retain its exact value separately from the pipeline key.
  std::map<std::uint64_t, SourceTimestamp> timestamps;
  std::optional<std::uint64_t> last_pipeline_pts;
  std::shared_ptr<std::atomic<unsigned>> native_leases = std::make_shared<std::atomic<unsigned>>(0);
  bool finished = false, faulted = false;
  ~Impl() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) gst_object_unref(bus);
    if (pipeline) gst_object_unref(pipeline);
  }
  GstElement* add(const char* factory, const char* label) {
    auto* element = gst_element_factory_make(factory, label);
    check(element, "required video pipeline element unavailable");
    if (!gst_bin_add(GST_BIN(pipeline), element)) {
      gst_object_unref(element);
      throw std::runtime_error("video pipeline insertion failed");
    }
    return element;
  }
  void status() {
    check(!faulted, "video decoder is faulted");
    auto* error = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (error) {
      gst_message_unref(error);
      faulted = true;
      throw std::runtime_error("video decoder pipeline failed");
    }
  }
  void start() {
    check(!(options.metadata_only && options.native_buffers), "conflicting video output policies");
    check(options.max_native_leases >= 1 && options.max_native_leases <= 4,
          "invalid native video lease bound");
    const bool native_output = options.metadata_only || options.native_buffers;
    static std::once_flag once;
    std::call_once(once, [] {
      GError* error = nullptr;
      const bool ok = gst_init_check(nullptr, nullptr, &error);
      if (error) g_error_free(error);
      check(ok, "GStreamer initialization failed");
    });
    check(options.max_width > 0 && options.max_width <= 4096 && options.max_height > 0 &&
              options.max_height <= 2160,
          "invalid video dimension limits");
    check(options.codec == Codec::h264 || options.codec == Codec::h265, "invalid video codec");
    check(!options.codec_data.empty() && options.codec_data.size() <= 65536,
          "invalid video codec data size");
    const bool avc = options.codec == Codec::h264;
    check(options.codec_data[0] == 1 && options.codec_data.size() >= (avc ? 7U : 23U),
          "invalid video codec data record");
    if (!options.decoder.empty()) {
      const std::array allowed{"v4l2slh264dec", "v4l2h264dec", "vah264dec", "avdec_h264",
                               "v4l2slh265dec", "v4l2h265dec", "vah265dec", "avdec_h265"};
      check(std::find(allowed.begin(), allowed.end(), options.decoder) != allowed.end(),
            "unsupported decoder policy");
      name = options.decoder;
    } else {
      const std::array candidates =
          avc ? std::array{"v4l2slh264dec", "v4l2h264dec", "vah264dec", "avdec_h264"}
              : std::array{"v4l2slh265dec", "v4l2h265dec", "vah265dec", "avdec_h265"};
      for (const auto* candidate : candidates)
        if (auto* factory = gst_element_factory_find(candidate)) {
          gst_object_unref(factory);
          name = candidate;
          break;
        }
      check(!name.empty(), "no registered video decoder");
    }
    pipeline = gst_pipeline_new(nullptr);
    check(pipeline, "video pipeline allocation failed");
    source = add("appsrc", "video-input");
    auto* parser = add(avc ? "h264parse" : "h265parse", "video-parser");
    auto* decoder = add(name.c_str(), "video-decoder");
    auto* convert = native_output ? nullptr : add("videoconvert", "video-convert");
    sink = add("appsink", "video-output");
    g_object_set(source, "is-live", TRUE, "do-timestamp", FALSE, "format", GST_FORMAT_TIME, "block",
                 FALSE, "max-bytes", guint64(input_bytes), "max-buffers", guint64(input_frames),
                 "max-time", guint64(0), "emit-signals", FALSE, nullptr);
    Buffer data(gst_buffer_new_allocate(nullptr, options.codec_data.size(), nullptr),
                gst_buffer_unref);
    check(bool(data), "video codec allocation failed");
    gst_buffer_fill(data.get(), 0, options.codec_data.data(), options.codec_data.size());
    Caps caps(gst_caps_new_simple(avc ? "video/x-h264" : "video/x-h265", "stream-format",
                                  G_TYPE_STRING, avc ? "avc" : "hvc1", "alignment", G_TYPE_STRING,
                                  "au", "codec_data", GST_TYPE_BUFFER, data.get(), nullptr),
              gst_caps_unref);
    check(bool(caps), "video caps allocation failed");
    gst_app_src_set_caps(GST_APP_SRC(source), caps.get());
    Caps output(options.native_buffers
                    ? gst_caps_from_string("video/x-raw(memory:DMABuf),format=DMA_DRM")
                : options.metadata_only
                    ? gst_caps_from_string("video/x-raw(ANY)")
                    : gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", nullptr),
                gst_caps_unref);
    check(bool(output), "video output caps allocation failed");
    gst_app_sink_set_caps(GST_APP_SINK(sink), output.get());
    if (native_output) {
      // Stateless V4L2 DMA-buf output requires an explicit VideoMeta-aware
      // consumer. This sink retains/releases the native sample without assuming
      // linear pixels or stripping its stride/plane metadata. Handle allocation
      // on the sink pad without enabling appsink's streaming-thread signals.
      auto* pad = gst_element_get_static_pad(sink, "sink");
      check(pad, "video sink pad unavailable");
      gst_pad_add_probe(
          pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
          [](GstPad*, GstPadProbeInfo* info, gpointer) {
            auto* query = GST_PAD_PROBE_INFO_QUERY(info);
            if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) return GST_PAD_PROBE_OK;
            gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
            // Expose crop support as well as plane metadata: the importer keeps
            // the visible rectangle separate from padded decoder storage.
            gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, nullptr);
            return GST_PAD_PROBE_HANDLED;
          },
          nullptr, nullptr);
      gst_object_unref(pad);
    }
    g_object_set(sink, "sync", FALSE, "async", FALSE, "max-buffers", guint(3), "drop", FALSE,
                 "enable-last-sample", FALSE, "wait-on-eos", FALSE, "emit-signals", FALSE, nullptr);
    check(native_output ? gst_element_link_many(source, parser, decoder, sink, nullptr)
                        : gst_element_link_many(source, parser, decoder, convert, sink, nullptr),
          "video pipeline link failed");
    bus = gst_element_get_bus(pipeline);
    check(bus, "video bus unavailable");
    check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
          "video decoder failed to start");
  }
};
Decoder::Decoder(DecoderOptions options) : impl_(std::make_unique<Impl>()) {
  impl_->options = std::move(options);
  impl_->start();
}
Decoder::~Decoder() = default;
bool Decoder::push(const AirplayVideoPacket& message) {
  auto& h = *impl_;
  h.status();
  check(!h.finished, "video input already ended");
  check(message.kind == AirplayVideoPacket::Kind::frame && message.sender_ntp.has_value(),
        "decoder requires a timestamped video frame");
  check(!message.data.empty() && message.data.size() <= input_bytes, "invalid encoded video size");
  const auto source_pts = nanoseconds(*message.sender_ntp);
  auto pts = source_pts;
  // A parser may synthesize a new timestamp for an exact repeat even after
  // its previous output was consumed. Keep keys unique for the entire decoder
  // generation, while preserving the authenticated source bits separately.
  if (h.last_pipeline_pts && pts <= *h.last_pipeline_pts) {
    check(*h.last_pipeline_pts < GST_CLOCK_TIME_NONE - 1, "video pipeline timestamp exhausted");
    pts = *h.last_pipeline_pts + 1;
  }
  const auto bytes = gst_app_src_get_current_level_bytes(GST_APP_SRC(h.source));
  if (h.timestamps.size() >= pending_frames ||
      gst_app_src_get_current_level_buffers(GST_APP_SRC(h.source)) >= input_frames ||
      bytes >= input_bytes || message.data.size() > input_bytes - bytes)
    return false;
  Buffer buffer(gst_buffer_new_allocate(nullptr, message.data.size(), nullptr), gst_buffer_unref);
  check(bool(buffer), "encoded video allocation failed");
  gst_buffer_fill(buffer.get(), 0, message.data.data(), message.data.size());
  GST_BUFFER_PTS(buffer.get()) = pts;
  GST_BUFFER_DTS(buffer.get()) = GST_CLOCK_TIME_NONE;
  h.timestamps.emplace(pts, Impl::SourceTimestamp{*message.sender_ntp, source_pts});
  h.last_pipeline_pts = pts;
  if (gst_app_src_push_buffer(GST_APP_SRC(h.source), buffer.release()) != GST_FLOW_OK) {
    h.faulted = true;
    throw std::runtime_error("video input rejected by decoder");
  }
  return true;
}
std::optional<DecodedFrame> Decoder::pull() {
  auto& h = *impl_;
  check(!h.options.metadata_only && !h.options.native_buffers,
        "pixel output disabled by decoder policy");
  h.status();
  Sample sample(gst_app_sink_try_pull_sample(GST_APP_SINK(h.sink), 0), gst_sample_unref);
  if (!sample) return {};
  try {
    auto* buffer = gst_sample_get_buffer(sample.get());
    check(buffer, "decoded video has no buffer");
    const auto pts = GST_BUFFER_PTS(buffer);
    const auto found = h.timestamps.find(pts);
    check(pts != GST_CLOCK_TIME_NONE && found != h.timestamps.end(),
          "decoded video lost its source timestamp");
    GstVideoInfo info;
    gst_video_info_init(&info);
    check(gst_video_info_from_caps(&info, gst_sample_get_caps(sample.get())),
          "invalid decoded video caps");
    check(GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_BGRA && info.width > 0 &&
              info.height > 0 && std::uint32_t(info.width) <= h.options.max_width &&
              std::uint32_t(info.height) <= h.options.max_height,
          "decoded video exceeds negotiated dimensions");
    struct Mapping {
      GstVideoFrame frame{};
      bool mapped = false;
      ~Mapping() {
        if (mapped) gst_video_frame_unmap(&frame);
      }
    } mapping;
    mapping.mapped = gst_video_frame_map(&mapping.frame, &info, buffer, GST_MAP_READ);
    check(mapping.mapped, "decoded video mapping failed");
    DecodedFrame result{found->second.ntp,
                        found->second.ns,
                        {std::uint32_t(info.width), std::uint32_t(info.height), {}}};
    result.image.pixels.resize(std::size_t(info.width) * info.height);
    const auto* data =
        static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&mapping.frame, 0));
    const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapping.frame, 0);
    check(stride >= info.width * 4, "invalid decoded video stride");
    for (int y = 0; y < info.height; ++y)
      for (int x = 0; x < info.width; ++x) {
        const auto* p = data + std::size_t(y) * stride + std::size_t(x) * 4;
        // Video is opaque; preserve its RGB channels in the scene's native ARGB32.
        result.image.pixels[std::size_t(y) * info.width + x] =
            0xff000000U | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | p[0];
      }
    h.timestamps.erase(found);
    return result;
  } catch (...) {
    h.faulted = true;
    throw;
  }
}
std::optional<DecodedMetadata> Decoder::pull_metadata() {
  auto& h = *impl_;
  check(h.options.metadata_only, "metadata output disabled by decoder policy");
  h.status();
  Sample sample(gst_app_sink_try_pull_sample(GST_APP_SINK(h.sink), 0), gst_sample_unref);
  if (!sample) return {};
  try {
    auto* buffer = gst_sample_get_buffer(sample.get());
    check(buffer, "decoded video has no buffer");
    const auto pts = GST_BUFFER_PTS(buffer);
    const auto found = h.timestamps.find(pts);
    check(pts != GST_CLOCK_TIME_NONE && found != h.timestamps.end(),
          "decoded video lost its source timestamp");
    // Native DMA-buf caps may use DMA_DRM plus a modifier rather than a CPU
    // GstVideoInfo format. Dimensions are sufficient for this non-rendering
    // diagnostic; neither mapping nor GPU import is implied by accepting caps.
    auto* caps = gst_sample_get_caps(sample.get());
    check(caps && gst_caps_get_size(caps) == 1, "invalid decoded metadata caps");
    const auto* description = gst_caps_get_structure(caps, 0);
    int width = 0, height = 0;
    check(gst_structure_has_name(description, "video/x-raw") &&
              gst_structure_get_int(description, "width", &width) &&
              gst_structure_get_int(description, "height", &height) && width > 0 && height > 0 &&
              std::uint32_t(width) <= h.options.max_width &&
              std::uint32_t(height) <= h.options.max_height,
          "decoded video exceeds negotiated dimensions");
    const DecodedMetadata result{found->second.ntp, found->second.ns, std::uint32_t(width),
                                 std::uint32_t(height)};
    h.timestamps.erase(found);
    return result;
  } catch (...) {
    h.faulted = true;
    throw;
  }
}
std::shared_ptr<const NativeFrame> Decoder::pull_native() {
  auto& h = *impl_;
  check(h.options.native_buffers, "native output disabled by decoder policy");
  h.status();
  if (h.native_leases->load() >= h.options.max_native_leases) return {};
  Sample sample(gst_app_sink_try_pull_sample(GST_APP_SINK(h.sink), 0), gst_sample_unref);
  if (!sample) return {};
  try {
    auto* buffer = gst_sample_get_buffer(sample.get());
    check(buffer, "decoded video has no buffer");
    const auto pts = GST_BUFFER_PTS(buffer);
    const auto found = h.timestamps.find(pts);
    if (pts == GST_CLOCK_TIME_NONE || found == h.timestamps.end())
      throw std::runtime_error(
          "decoded video lost its source timestamp: returned=" + std::to_string(pts) +
          " expected=" +
          (h.timestamps.empty() ? std::string("none")
                                : std::to_string(h.timestamps.begin()->first)));
    auto* caps = gst_sample_get_caps(sample.get());
    GstVideoInfoDmaDrm info;
    gst_video_info_dma_drm_init(&info);
    check(caps && gst_video_is_dma_drm_caps(caps) && gst_video_info_dma_drm_from_caps(&info, caps),
          "decoded video lacks explicit DMA format");
    const auto& vi = info.vinfo;
    check(vi.width > 0 && vi.height > 0 && std::uint32_t(vi.width) <= h.options.max_width &&
              std::uint32_t(vi.height) <= h.options.max_height,
          "decoded video exceeds negotiated dimensions");
    auto* layout = gst_buffer_get_video_meta(buffer);
    if (!layout || layout->n_planes == 0 || layout->n_planes > 4 ||
        layout->width < std::uint32_t(vi.width) || layout->height < std::uint32_t(vi.height) ||
        layout->width > 8192 || layout->height > 8192)
      throw std::runtime_error("invalid native video layout: caps=" + std::to_string(vi.width) +
                               "x" + std::to_string(vi.height) +
                               (layout ? " storage=" + std::to_string(layout->width) + "x" +
                                             std::to_string(layout->height) +
                                             " planes=" + std::to_string(layout->n_planes)
                                       : " missing VideoMeta"));
    auto frame = std::make_shared<NativeFrame>();
    frame->metadata = {found->second.ntp, found->second.ns, std::uint32_t(vi.width),
                       std::uint32_t(vi.height)};
    frame->storage_width = layout->width;
    frame->storage_height = layout->height;
    frame->fourcc = info.drm_fourcc;
    frame->modifier = info.drm_modifier;
    frame->plane_count = layout->n_planes;
    if (auto* crop = gst_buffer_get_video_crop_meta(buffer)) {
      check(crop->width == std::uint32_t(vi.width) && crop->height == std::uint32_t(vi.height),
            "native video crop differs from negotiated size");
      frame->crop_x = crop->x;
      frame->crop_y = crop->y;
    }
    check(std::uint64_t(frame->crop_x) + vi.width <= layout->width &&
              std::uint64_t(frame->crop_y) + vi.height <= layout->height,
          "native video crop exceeds storage");
    switch (vi.colorimetry.matrix) {
      case GST_VIDEO_COLOR_MATRIX_BT601:
        frame->matrix = YuvMatrix::bt601;
        break;
      case GST_VIDEO_COLOR_MATRIX_BT709:
        frame->matrix = YuvMatrix::bt709;
        break;
      case GST_VIDEO_COLOR_MATRIX_BT2020:
        frame->matrix = YuvMatrix::bt2020;
        break;
      default:
        throw std::runtime_error("unsupported native video colour matrix");
    }
    check(vi.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ||
              vi.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235,
          "unknown native video colour range");
    frame->full_range = vi.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    check(!(vi.chroma_site & GST_VIDEO_CHROMA_SITE_ALT_LINE),
          "unsupported alternate native chroma siting");
    frame->chroma_x_cosited = vi.chroma_site & GST_VIDEO_CHROMA_SITE_H_COSITED;
    frame->chroma_y_cosited = vi.chroma_site & GST_VIDEO_CHROMA_SITE_V_COSITED;
    for (unsigned p = 0; p < layout->n_planes; ++p) {
      guint index = 0, length = 0;
      gsize skip = 0;
      check(layout->stride[p] > 0 &&
                gst_buffer_find_memory(buffer, layout->offset[p], 1, &index, &length, &skip) &&
                length == 1,
            "invalid native video plane");
      auto* memory = gst_buffer_peek_memory(buffer, index);
      check(memory && gst_is_dmabuf_memory(memory), "native video plane is not DMA-buf backed");
      gsize offset = 0, maximum = 0;
      const auto size = gst_memory_get_sizes(memory, &offset, &maximum);
      check(skip < size && offset <= maximum && skip < maximum - offset &&
                offset + skip <= 0x7fffffff,
            "native video plane offset exceeds import limit");
      const auto fd = gst_dmabuf_memory_get_fd(memory);
      check(fd >= 0, "invalid native video descriptor");
      frame->planes[p] = {fd, std::uint32_t(offset + skip), import_stride(*layout, p)};
    }
    ++*h.native_leases;
    frame->lifetime = std::shared_ptr<const void>(
        sample.release(), [leases = h.native_leases](const void* value) {
          gst_sample_unref(const_cast<GstSample*>(static_cast<const GstSample*>(value)));
          --*leases;
        });
    h.timestamps.erase(found);
    return frame;
  } catch (...) {
    h.faulted = true;
    throw;
  }
}
void Decoder::finish() {
  auto& h = *impl_;
  h.status();
  if (h.finished) return;
  h.finished = true;
  check(gst_app_src_end_of_stream(GST_APP_SRC(h.source)) == GST_FLOW_OK,
        "video end-of-stream failed");
}
bool Decoder::drained() {
  auto& h = *impl_;
  h.status();
  if (!h.finished || !gst_app_sink_is_eos(GST_APP_SINK(h.sink))) return false;
  if (!h.timestamps.empty()) {
    h.faulted = true;
    throw std::runtime_error("decoder ended with unmatched source frames");
  }
  return true;
}
std::string Decoder::decoder_name() const {
  return impl_->name;
}
}  // namespace headunit::theme::video
