// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/gst_preview_frames.hpp>
#include <headunit/theme/scene.hpp>

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <atomic>
#include <stdexcept>
#include <utility>

namespace headunit {
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct Element {
  GstElement* value = nullptr;
  explicit Element(GstElement* element) : value(element) {
    if (value && g_object_is_floating(value)) gst_object_ref_sink(value);
  }
  ~Element() {
    if (value) gst_object_unref(value);
  }
  Element(const Element&) = delete;
  Element& operator=(const Element&) = delete;
  GstElement* release() { return std::exchange(value, nullptr); }
};
struct Sample {
  GstSample* value = nullptr;
  ~Sample() {
    if (value) gst_sample_unref(value);
  }
};
struct Mapping {
  GstVideoFrame frame{};
  bool mapped = false;
  ~Mapping() {
    if (mapped) gst_video_frame_unmap(&frame);
  }
};
struct Pad {
  GstPad* value = nullptr;
  ~Pad() {
    if (value) gst_object_unref(value);
  }
};
struct Caps {
  GstCaps* value = nullptr;
  ~Caps() {
    if (value) gst_caps_unref(value);
  }
};
struct Value {
  GValue value = G_VALUE_INIT;
  explicit Value(GType type) { g_value_init(&value, type); }
  ~Value() { g_value_unset(&value); }
};
struct SegmentProvenance {
  std::atomic<bool> observed{false};
};
}  // namespace

struct GstPreviewFrames::Impl {
  std::uint32_t max_width, max_height;
  GstElement* owned = nullptr;
  GWeakRef sink{};
  std::optional<std::uint64_t> last_running;

  Impl(std::uint32_t width, std::uint32_t height) : max_width(width), max_height(height) {
    require(width > 0 && width <= 4096 && height > 0 && height <= 2160,
            "invalid preview decoded-image bounds");
    gst_init(nullptr, nullptr);
    g_weak_ref_init(&sink, nullptr);
    try {
      Element bin(gst_bin_new(nullptr));
      Element convert(gst_element_factory_make("videoconvert", nullptr));
      Element app(gst_element_factory_make("appsink", nullptr));
      require(bin.value && convert.value && app.value,
              "preview output adapter plugins unavailable");
      g_object_set(app.value, "sync", FALSE, "async", FALSE, "max-buffers", guint(3), "drop", TRUE,
                   "enable-last-sample", FALSE, "wait-on-eos", FALSE, "emit-signals", FALSE,
                   nullptr);
      Caps caps{gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRx",
                                    "interlace-mode", G_TYPE_STRING, "progressive", "views",
                                    G_TYPE_INT, 1, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                                    "multiview-mode", G_TYPE_STRING, "mono", nullptr)};
      require(caps.value, "cannot allocate preview decoded-image caps");
      auto* description = gst_caps_get_structure(caps.value, 0);
      if (width == 1)
        gst_structure_set(description, "width", G_TYPE_INT, 1, nullptr);
      else
        gst_structure_set(description, "width", GST_TYPE_INT_RANGE, 1, int(width), nullptr);
      if (height == 1)
        gst_structure_set(description, "height", G_TYPE_INT, 1, nullptr);
      else
        gst_structure_set(description, "height", GST_TYPE_INT_RANGE, 1, int(height), nullptr);
      // NONE is GStreamer's unspecified mode, represented by absence of this
      // caps field (not the invalid string "none"). Unspecified normal input
      // intersects this mono constraint and remains negotiable.
      Value flags(GST_TYPE_VIDEO_MULTIVIEW_FLAGSET);
      gst_value_set_flagset(&flags.value, GST_VIDEO_MULTIVIEW_FLAGS_NONE, ~guint(0));
      gst_structure_set_value(description, "multiview-flags", &flags.value);
      gst_app_sink_set_caps(GST_APP_SINK(app.value), caps.value);
      Pad sink_pad{gst_element_get_static_pad(app.value, "sink")};
      require(sink_pad.value, "preview appsink has no input pad");
      // GstBaseSink synthesizes a default TIME segment if input illegally
      // precedes its segment event. Do not let that acquire arrival authority.
      // The pad owns this small independent state; it captures no adapter or sink.
      auto provenance = std::make_unique<SegmentProvenance>();
      const auto probe = gst_pad_add_probe(
          sink_pad.value,
          static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
                                       GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                                       GST_PAD_PROBE_TYPE_EVENT_FLUSH),
          [](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
            auto& state = *static_cast<SegmentProvenance*>(data);
            const auto type = GST_PAD_PROBE_INFO_TYPE(info);
            if (type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
              switch (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info))) {
                case GST_EVENT_SEGMENT:
                  state.observed = true;
                  break;
                case GST_EVENT_STREAM_START:
                case GST_EVENT_FLUSH_START:
                case GST_EVENT_FLUSH_STOP:
                  state.observed = false;
                  break;
                default:
                  break;
              }
            }
            if (type & GST_PAD_PROBE_TYPE_BUFFER_LIST) return GST_PAD_PROBE_DROP;
            if ((type & GST_PAD_PROBE_TYPE_BUFFER) && !state.observed) return GST_PAD_PROBE_DROP;
            return GST_PAD_PROBE_OK;
          },
          provenance.get(), [](gpointer data) { delete static_cast<SegmentProvenance*>(data); });
      require(probe != 0, "cannot observe preview segment provenance");
      provenance.release();
      // Nonfloating element owners keep one reference while the bin takes its
      // own. All locals release theirs before construction returns.
      require(gst_bin_add(GST_BIN(bin.value), convert.value), "cannot add preview converter");
      require(gst_bin_add(GST_BIN(bin.value), app.value), "cannot add preview appsink");
      require(gst_element_link(convert.value, app.value), "cannot link preview decoded output");
      Pad input{gst_element_get_static_pad(convert.value, "sink")};
      require(input.value, "preview converter has no sink pad");
      Pad ghost{gst_ghost_pad_new("sink", input.value)};
      require(ghost.value, "cannot expose preview output sink pad");
      if (g_object_is_floating(ghost.value)) gst_object_ref_sink(ghost.value);
      require(gst_element_add_pad(bin.value, ghost.value), "cannot attach preview output sink pad");
      g_weak_ref_set(&sink, app.value);
      owned = bin.release();
    } catch (...) {
      g_weak_ref_clear(&sink);
      throw;
    }
  }
  ~Impl() {
    if (owned) gst_object_unref(owned);
    g_weak_ref_clear(&sink);
  }
  Element acquire() const {
    return Element(GST_ELEMENT(g_weak_ref_get(const_cast<GWeakRef*>(&sink))));
  }
};

GstPreviewFrames::GstPreviewFrames(std::uint32_t width, std::uint32_t height)
    : impl_(std::make_unique<Impl>(width, height)) {}
GstPreviewFrames::~GstPreviewFrames() = default;

GstElement* GstPreviewFrames::release_output() {
  require(impl_->owned, "preview decoded output already transferred");
  return std::exchange(impl_->owned, nullptr);
}

bool GstPreviewFrames::closed() const {
  const auto sink = impl_->acquire();
  return sink.value == nullptr;
}
bool GstPreviewFrames::eos() const {
  const auto sink = impl_->acquire();
  if (!sink.value) return false;
  GstState actual = GST_STATE_NULL;
  gst_element_get_state(sink.value, &actual, nullptr, 0);
  // appsink also reports is_eos when it has not started. Keep that distinct
  // from an actual end of a live, drained stream.
  return actual >= GST_STATE_PAUSED && gst_app_sink_is_eos(GST_APP_SINK(sink.value));
}

std::optional<GstPreviewFrames::Frame> GstPreviewFrames::pull() {
  auto& h = *impl_;
  const auto sink = h.acquire();
  if (!sink.value) return {};
  // Clear appsink's independent preroll reference as well. It is not a second
  // presentation candidate and must not keep the first startup image alive.
  {
    Sample preroll{gst_app_sink_try_pull_preroll(GST_APP_SINK(sink.value), 0)};
  }
  Sample sample{gst_app_sink_try_pull_sample(GST_APP_SINK(sink.value), 0)};
  if (!sample.value) return {};
  auto* buffer = gst_sample_get_buffer(sample.value);
  const auto* segment = gst_sample_get_segment(sample.value);
  require(buffer && GST_BUFFER_PTS_IS_VALID(buffer),
          "preview decoded output lacks original arrival PTS");
  require(segment && segment->format == GST_FORMAT_TIME && segment->rate == 1.0 &&
              segment->applied_rate == 1.0,
          "preview decoded output lacks a unit-rate TIME segment");
  const auto running =
      gst_segment_to_running_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
  require(GST_CLOCK_TIME_IS_VALID(running), "preview decoded arrival is outside its TIME segment");
  if (h.last_running && running <= *h.last_running) return {};

  auto* caps = gst_sample_get_caps(sample.value);
  GstVideoInfo info;
  gst_video_info_init(&info);
  require(caps && gst_caps_is_fixed(caps) && gst_caps_get_size(caps) == 1 &&
              gst_video_info_from_caps(&info, caps),
          "preview decoded output has invalid raw-video caps");
  require(GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_BGRx &&
              GST_VIDEO_INFO_N_PLANES(&info) == 1 &&
              GST_VIDEO_INFO_INTERLACE_MODE(&info) == GST_VIDEO_INTERLACE_MODE_PROGRESSIVE &&
              GST_VIDEO_INFO_VIEWS(&info) == 1 &&
              (GST_VIDEO_INFO_MULTIVIEW_MODE(&info) == GST_VIDEO_MULTIVIEW_MODE_NONE ||
               GST_VIDEO_INFO_MULTIVIEW_MODE(&info) == GST_VIDEO_MULTIVIEW_MODE_MONO) &&
              GST_VIDEO_INFO_MULTIVIEW_FLAGS(&info) == GST_VIDEO_MULTIVIEW_FLAGS_NONE &&
              GST_VIDEO_INFO_PAR_N(&info) == 1 && GST_VIDEO_INFO_PAR_D(&info) == 1 &&
              info.width > 0 && info.height > 0 && std::uint32_t(info.width) <= h.max_width &&
              std::uint32_t(info.height) <= h.max_height,
          "preview decoded output exceeds its BGRx image contract");
  if (const auto* crop = gst_buffer_get_video_crop_meta(buffer)) {
    require(crop->x == 0 && crop->y == 0 && crop->width == std::uint32_t(info.width) &&
                crop->height == std::uint32_t(info.height),
            "preview decoded output requires an unsupported crop");
  }
  if (const auto* layout = gst_buffer_get_video_meta(buffer)) {
    // GstVideoFrame's mapper asserts some caps/meta invariants. Validate them
    // first so malformed decoder metadata fails through our normal error path.
    require(layout->format == GST_VIDEO_FORMAT_BGRx && layout->n_planes == 1 &&
                layout->width >= std::uint32_t(info.width) &&
                layout->height >= std::uint32_t(info.height) && layout->width <= 4096 &&
                layout->height <= 2160 && layout->stride[0] > 0 &&
                std::size_t(layout->stride[0]) >= std::size_t(info.width) * 4,
            "preview decoded metadata disagrees with its caps or stride");
    const auto total = gst_buffer_get_size(buffer);
    const std::size_t row = std::size_t(info.width) * 4;
    require(layout->offset[0] <= total && row <= total - layout->offset[0] &&
                std::size_t(info.height - 1) <=
                    (total - layout->offset[0] - row) / std::size_t(layout->stride[0]),
            "preview decoded metadata addresses absent storage");
  }
  Mapping mapped;
  mapped.mapped = gst_video_frame_map(&mapped.frame, &info, buffer, GST_MAP_READ);
  require(mapped.mapped, "preview decoded output mapping failed");
  require(GST_VIDEO_FRAME_FORMAT(&mapped.frame) == GST_VIDEO_FORMAT_BGRx &&
              GST_VIDEO_FRAME_N_PLANES(&mapped.frame) == 1 &&
              GST_VIDEO_FRAME_WIDTH(&mapped.frame) == info.width &&
              GST_VIDEO_FRAME_HEIGHT(&mapped.frame) == info.height &&
              !GST_VIDEO_FRAME_FLAG_IS_SET(&mapped.frame, GST_VIDEO_FRAME_FLAG_MULTIPLE_VIEW),
          "preview decoded metadata changed its negotiated layout");
  const auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&mapped.frame, 0));
  const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped.frame, 0);
  const std::size_t row_bytes = std::size_t(info.width) * 4;
  require(data && stride > 0 && std::size_t(stride) >= row_bytes,
          "preview decoded output has an invalid stride");
  // For the one packed plane, validate the full addressed span against the
  // real map. GstVideoMeta can move its origin and pad every decoded row.
  const auto origin = reinterpret_cast<std::uintptr_t>(data);
  const auto base = reinterpret_cast<std::uintptr_t>(mapped.frame.map[0].data);
  const auto bytes = mapped.frame.map[0].size;
  require(mapped.frame.map[0].data && origin >= base && origin - base <= bytes,
          "preview decoded plane is outside its mapped storage");
  const auto available = bytes - (origin - base);
  const std::size_t rows = std::size_t(info.height - 1);
  require(row_bytes <= available && rows <= (available - row_bytes) / std::size_t(stride),
          "preview decoded rows exceed their mapped storage");

  auto image = std::make_shared<theme::graphics::Image>();
  image->width = std::uint32_t(info.width);
  image->height = std::uint32_t(info.height);
  image->pixels.resize(std::size_t(info.width) * std::size_t(info.height));
  for (int y = 0; y < info.height; ++y)
    for (int x = 0; x < info.width; ++x) {
      const auto* p = data + std::size_t(y) * std::size_t(stride) + std::size_t(x) * 4;
      image->pixels[std::size_t(y) * std::size_t(info.width) + std::size_t(x)] =
          0xff000000U | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | p[0];
    }
  h.last_running = running;
  return Frame{running, std::move(image)};
}

}  // namespace headunit
