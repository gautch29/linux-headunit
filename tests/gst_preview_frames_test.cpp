// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/gst_preview_frames.hpp>
#include <headunit/gst_preview_output.hpp>
#include <headunit/theme/scene.hpp>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using headunit::GstPreviewFrames;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F fn, const char* why) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, why);
}
template <class F>
void wait(F ready, const char* why) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!ready()) {
    check(std::chrono::steady_clock::now() < until, why);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
struct Fixture {
  GstElement *pipeline = nullptr, *source = nullptr, *sink = nullptr, *valve = nullptr;
  std::unique_ptr<headunit::GstPreviewOutput> gate;
  GstPreviewFrames frames{16, 16};
  std::atomic<unsigned> received{0}, released{0};
  unsigned submitted = 0;
  explicit Fixture(bool replaceable = false) {
    pipeline = gst_pipeline_new(nullptr);
    source = gst_element_factory_make("appsrc", nullptr);
    check(pipeline && source, "preview frames fixture unavailable");
    g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE, "do-timestamp",
                 FALSE, "max-bytes", guint64(65536), "handle-segment-change", TRUE, nullptr);
    check(gst_bin_add(GST_BIN(pipeline), source), "cannot add fixture source");
    auto* output = frames.release_output();
    auto* children = gst_bin_iterate_elements(GST_BIN(output));
    GValue value = G_VALUE_INIT;
    while (gst_iterator_next(children, &value) == GST_ITERATOR_OK) {
      if (GST_IS_APP_SINK(g_value_get_object(&value)))
        sink = GST_ELEMENT(g_value_dup_object(&value));
      g_value_reset(&value);
    }
    if (G_IS_VALUE(&value)) g_value_unset(&value);
    gst_iterator_free(children);
    check(sink, "adapter did not expose its appsink child");
    g_object_set(sink, "emit-signals", TRUE, nullptr);
    g_signal_connect(sink, "new-sample",
                     G_CALLBACK(+[](GstElement*, gpointer data) -> GstFlowReturn {
                       ++static_cast<Fixture*>(data)->received;
                       return GST_FLOW_OK;
                     }),
                     this);
    if (replaceable) {
      valve = gst_element_factory_make("valve", nullptr);
      check(valve && gst_bin_add(GST_BIN(pipeline), valve), "cannot add replaceable fixture valve");
      check(gst_element_link(source, valve), "cannot link fixture valve");
      gate = std::make_unique<headunit::GstPreviewOutput>(pipeline, valve);
      gate->start(output);
    } else {
      check(gst_bin_add(GST_BIN(pipeline), output), "cannot add adapter output");
      // release_output transfers a nonfloating owner; the bin owns a separate
      // reference. Fixture does not keep a redundant output-owner reference.
      gst_object_unref(output);
      check(gst_element_link(source, output), "cannot link adapter output");
    }
  }
  ~Fixture() {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (sink) gst_object_unref(sink);
    gate.reset();
    gst_object_unref(pipeline);
  }
  void play() {
    check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
          "fixture could not play");
    GstState actual = GST_STATE_NULL;
    check(
        gst_element_get_state(pipeline, &actual, nullptr, GST_SECOND) == GST_STATE_CHANGE_SUCCESS &&
            actual == GST_STATE_PLAYING,
        "preview frames fixture not PLAYING");
  }
  void remove_next_pts() {
    auto* pad = gst_element_get_static_pad(sink, "sink");
    check(pad, "appsink has no input pad");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad*, GstPadProbeInfo* info, gpointer) {
          auto* bytes = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
          GST_PAD_PROBE_INFO_DATA(info) = bytes;
          GST_BUFFER_PTS(bytes) = GST_CLOCK_TIME_NONE;
          return GST_PAD_PROBE_REMOVE;
        },
        nullptr, nullptr);
    gst_object_unref(pad);
  }
  void omit_segment() {
    auto* input = gst_element_get_static_pad(sink, "sink");
    check(input, "appsink has no input pad");
    auto* pad = gst_pad_get_peer(input);
    gst_object_unref(input);
    check(pad, "appsink has no upstream converter pad");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        +[](GstPad*, GstPadProbeInfo* info, gpointer) {
          return GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_SEGMENT
                     ? GST_PAD_PROBE_DROP
                     : GST_PAD_PROBE_OK;
        },
        nullptr, nullptr);
    gst_object_unref(pad);
  }
  bool replace_caps(unsigned width, unsigned height, const char* format) {
    auto* description = caps(width, height, format);
    const bool accepted = apply_caps(description);
    gst_caps_unref(description);
    return accepted;
  }
  bool apply_caps(GstCaps* description) {
    auto* pad = gst_element_get_static_pad(sink, "sink");
    const bool accepted = gst_pad_send_event(pad, gst_event_new_caps(description));
    gst_object_unref(pad);
    return accepted;
  }
  void direct(GstBuffer* bytes) {
    auto* pad = gst_element_get_static_pad(sink, "sink");
    const auto result = gst_pad_chain(pad, bytes);
    gst_object_unref(pad);
    check(result == GST_FLOW_OK, "appsink rejected direct validation buffer flow");
  }
  void invalidate_next_layout(unsigned variant) {
    auto* pad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad*, GstPadProbeInfo* info, gpointer data) {
          auto* bytes = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
          GST_PAD_PROBE_INFO_DATA(info) = bytes;
          auto* meta = gst_buffer_get_video_meta(bytes);
          if (!meta) return GST_PAD_PROBE_REMOVE;
          switch (GPOINTER_TO_UINT(data)) {
            case 0:
              meta->stride[0] = 8;
              break;
            case 1:
              meta->stride[0] = 1024;
              break;
            case 2:
              meta->stride[0] = -20;
              break;
            case 3:
              meta->height = 1;
              break;
            case 4: {
              auto* crop = gst_buffer_add_video_crop_meta(bytes);
              crop->x = 1;
              crop->y = 0;
              crop->width = 2;
              crop->height = 2;
              break;
            }
            case 5:
              meta->format = GST_VIDEO_FORMAT_RGBx;
              break;
            case 6:
              meta->offset[0] = gst_buffer_get_size(bytes) + 1;
              break;
            case 7:
              meta->flags = GST_VIDEO_FRAME_FLAG_MULTIPLE_VIEW;
              break;
            default:
              break;
          }
          return GST_PAD_PROBE_REMOVE;
        },
        GUINT_TO_POINTER(variant), nullptr);
    gst_object_unref(pad);
  }
  static GstCaps* caps(unsigned width = 3, unsigned height = 2, const char* format = "BGRx") {
    return gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, format, "width", G_TYPE_INT,
                               int(width), "height", G_TYPE_INT, int(height), "framerate",
                               GST_TYPE_FRACTION, 0, 1, "interlace-mode", G_TYPE_STRING,
                               "progressive", nullptr);
  }
  static GstBuffer* buffer(GstClockTime pts, unsigned tag = 0, bool padded = false) {
    const std::size_t offset = padded ? 8 : 0, stride = padded ? 20 : 12;
    auto* result = gst_buffer_new_allocate(nullptr, offset + stride * 2, nullptr);
    GstMapInfo mapped{};
    check(gst_buffer_map(result, &mapped, GST_MAP_WRITE), "cannot fill raw fixture pixels");
    for (std::size_t i = 0; i < mapped.size; ++i)
      mapped.data[i] = 0xde;
    for (unsigned y = 0; y < 2; ++y)
      for (unsigned x = 0; x < 3; ++x) {
        auto* p = mapped.data + offset + y * stride + x * 4;
        p[0] = std::uint8_t(10 + x + tag);
        p[1] = std::uint8_t(20 + y);
        p[2] = 30;
        p[3] = 1;
      }
    gst_buffer_unmap(result, &mapped);
    if (padded) {
      gsize offsets[GST_VIDEO_MAX_PLANES] = {offset, 0, 0, 0};
      gint strides[GST_VIDEO_MAX_PLANES] = {gint(stride), 0, 0, 0};
      check(gst_buffer_add_video_meta_full(result, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRx,
                                           3, 2, 1, offsets, strides),
            "cannot describe padded raw fixture");
    }
    GST_BUFFER_PTS(result) = pts;
    return result;
  }
  void push(GstClockTime pts, unsigned tag = 0, bool padded = false,
            const GstSegment* segment = nullptr, bool await_sink = true) {
    auto* description = caps();
    auto* bytes = buffer(pts, tag, padded);
    gst_mini_object_weak_ref(
        GST_MINI_OBJECT(bytes),
        +[](gpointer data, GstMiniObject*) { ++static_cast<Fixture*>(data)->released; }, this);
    auto* sample = gst_sample_new(bytes, description, segment, nullptr);
    check(sample, "cannot create raw fixture sample");
    const auto result = gst_app_src_push_sample(GST_APP_SRC(source), sample);
    gst_sample_unref(sample);
    gst_caps_unref(description);
    gst_buffer_unref(bytes);
    check(result == GST_FLOW_OK, "raw fixture push failed");
    ++submitted;
    if (await_sink)
      wait([&] { return received.load() == submitted; },
           "raw fixture sample did not reach bounded sink");
  }
  void finish() {
    check(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK,
          "raw fixture EOS rejected");
    auto* bus = gst_element_get_bus(pipeline);
    auto* message = gst_bus_timed_pop_filtered(
        bus, GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    check(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS,
          "raw fixture did not finish EOS");
    gst_message_unref(message);
    gst_object_unref(bus);
  }
};

void bounds_and_ownership() {
  rejects([] { GstPreviewFrames adapter(0, 1); }, "zero width allowed");
  rejects([] { GstPreviewFrames adapter(1, 0); }, "zero height allowed");
  rejects([] { GstPreviewFrames adapter(4097, 2160); }, "oversized width allowed");
  rejects([] { GstPreviewFrames adapter(4096, 2161); }, "oversized height allowed");
  GstPreviewFrames adapter(1, 1);
  check(!adapter.closed() && !adapter.eos() && !adapter.pull(), "new owned adapter is unavailable");
  auto* output = adapter.release_output();
  check(output && !g_object_is_floating(output), "adapter transferred an invalid owner");
  rejects([&] { adapter.release_output(); }, "adapter transferred twice");
  gst_object_unref(output);
  check(adapter.closed() && !adapter.eos() && !adapter.pull(),
        "weak sink retained uninstalled output");
  GstPreviewFrames maximum(4096, 2160);
}

void pixels_and_segment() {
  Fixture f;
  f.play();
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = 10 * GST_SECOND;
  segment.stop = 20 * GST_SECOND;
  segment.base = 2 * GST_SECOND;
  f.push(11 * GST_SECOND, 0, true, &segment);
  auto first = f.frames.pull();
  check(first && first->arrival_running_ns == 3 * GST_SECOND,
        "unit-rate segment did not preserve exact running time");
  check(first->image->width == 3 && first->image->height == 2 && first->image->pixels.size() == 6,
        "padded video dimensions changed");
  for (unsigned y = 0; y < 2; ++y)
    for (unsigned x = 0; x < 3; ++x)
      check(first->image->pixels[y * 3 + x] == (0xff1e0000U | ((20 + y) << 8) | (10 + x)),
            "padded BGRx rows or opaque RGB channels changed");
  const auto preserved = first->image->pixels;
  f.push(12 * GST_SECOND, 7, true);
  auto next = f.frames.pull();
  check(next && first->image != next->image && next->image->pixels != preserved &&
            first->image->pixels == preserved,
        "later decoded frame mutated immutable image backing");
  wait([&] { return f.released >= 1; },
       "immutable image retained an earlier GstBuffer/sample after the next pull");
  check(!f.frames.pull() && !f.frames.closed() && !f.frames.eos(),
        "empty live queue was mistaken for closure/EOS");
  f.finish();
  check(f.frames.eos() && !f.frames.closed(), "drained EOS confused weak closure");
}

void bounded_queue_and_order() {
  Fixture f;
  f.play();
  for (unsigned i = 1; i <= 40; ++i)
    f.push(i * GST_MSECOND, i);
  check(f.released >= 36,
        "producer retained unbounded decoded buffers outside its three-sample queue");
  std::vector<GstPreviewFrames::Frame> retained;
  while (auto frame = f.frames.pull())
    retained.push_back(std::move(*frame));
  check(retained.size() == 3 && retained[0].arrival_running_ns == 38 * GST_MSECOND &&
            retained[1].arrival_running_ns == 39 * GST_MSECOND &&
            retained[2].arrival_running_ns == 40 * GST_MSECOND,
        "producer retained more than the newest three samples");
  // appsink itself may cache its most recent pulled GstSample. The adapter
  // owns no sample; the sink's bounded framework cache dies on output drain.
  wait([&] { return f.released >= 39; }, "consumed samples remained unbounded after image copy");
  f.push(40 * GST_MSECOND);
  check(!f.frames.pull(), "equal arrival stamp was renumbered");
  f.push(39 * GST_MSECOND);
  check(!f.frames.pull(), "backward arrival stamp was accepted");
  f.push(41 * GST_MSECOND);
  auto fresh = f.frames.pull();
  check(fresh && fresh->arrival_running_ns == 41 * GST_MSECOND,
        "discarded repeats advanced source order");
  f.finish();
}

void malformed_timestamps() {
  Fixture f;
  f.play();
  // GstBaseSrc supplies zero for a first unclocked missing PTS. Remove it at
  // the sink input instead, exercising the real adapter sample-validation path.
  f.remove_next_pts();
  f.push(0);
  rejects([&] { f.frames.pull(); }, "missing arrival timestamp accepted");
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.rate = 2.0;
  f.push(10 * GST_MSECOND, 0, false, &segment);
  rejects([&] { f.frames.pull(); }, "non-unit segment rate accepted");
  segment.rate = 1.0;
  segment.applied_rate = 2.0;
  f.push(20 * GST_MSECOND, 0, false, &segment);
  rejects([&] { f.frames.pull(); }, "non-unit applied rate accepted");
  segment.applied_rate = 1.0;
  segment.start = 100 * GST_MSECOND;
  f.push(99 * GST_MSECOND, 0, false, &segment);
  rejects([&] { f.frames.pull(); }, "pre-segment source timestamp accepted");
}

void missing_segment() {
  struct Warnings {
    std::atomic<unsigned> expected{0}, unexpected{0};
  } warnings;
  const auto handler = g_log_set_handler(
      "GStreamer", G_LOG_LEVEL_WARNING,
      +[](const gchar*, GLogLevelFlags, const gchar* message, gpointer data) {
        auto& state = *static_cast<Warnings*>(data);
        if (g_strstr_len(message, -1, "Got data flow before segment event"))
          ++state.expected;
        else
          ++state.unexpected;
      },
      &warnings);
  struct Handler {
    guint id;
    ~Handler() { g_log_remove_handler("GStreamer", id); }
  } cleanup{handler};
  Fixture f;
  f.omit_segment();
  f.play();
  f.push(GST_MSECOND, 0, false, nullptr, false);
  f.finish();
  check(!f.frames.pull() && f.received == 0 && warnings.expected > 0 && warnings.unexpected == 0,
        "missing segment acquired synthesized running-time authority");
}

void non_time_segment() {
  Fixture f;
  g_object_set(f.source, "format", GST_FORMAT_BYTES, "handle-segment-change", FALSE, nullptr);
  f.play();
  f.push(GST_MSECOND);
  rejects([&] { f.frames.pull(); }, "non-TIME segment acquired arrival authority");
}

void malformed_caps_and_storage() {
  for (unsigned variant = 0; variant < 3; ++variant) {
    Fixture f;
    f.play();
    f.push(GST_MSECOND);
    check(bool(f.frames.pull()), "caps fixture did not prime");
    check(!f.replace_caps(variant == 1 ? 17 : 3, variant == 2 ? 17 : 2,
                          variant == 0 ? "RGBx" : "BGRx"),
          "appsink negotiated a wrong decoded format or oversized caps");
    check(!f.frames.pull(), "rejected caps admitted another decoded frame");
  }
  for (unsigned variant = 0; variant < 8; ++variant) {
    Fixture f;
    f.play();
    f.invalidate_next_layout(variant);
    f.push(GST_MSECOND, 0, true);
    rejects([&] { f.frames.pull(); }, "invalid decoded stride/storage/crop accepted");
  }
  auto logical_caps = [](unsigned variant) {
    auto* caps = Fixture::caps();
    auto* description = gst_caps_get_structure(caps, 0);
    const char* mode = variant == 0   ? "left"
                       : variant == 1 ? "right"
                       : variant == 2 ? "side-by-side"
                       : variant == 3 ? "top-bottom"
                                      : "mono";
    gst_structure_set(description, "multiview-mode", G_TYPE_STRING, mode, "views", G_TYPE_INT,
                      variant == 6 ? 2 : 1, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                      variant == 5 ? 2 : 1, 1, nullptr);
    GValue flags = G_VALUE_INIT;
    g_value_init(&flags, GST_TYPE_VIDEO_MULTIVIEW_FLAGSET);
    const guint bits = variant == 4   ? GST_VIDEO_MULTIVIEW_FLAGS_LEFT_FLIPPED
                       : variant == 7 ? GST_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT
                       : variant == 8 ? GST_VIDEO_MULTIVIEW_FLAGS_MIXED_MONO
                                      : GST_VIDEO_MULTIVIEW_FLAGS_NONE;
    gst_value_set_flagset(&flags, bits, ~guint(0));
    gst_structure_set_value(description, "multiview-flags", &flags);
    g_value_unset(&flags);
    return caps;
  };
  for (unsigned variant = 0; variant < 9; ++variant) {
    Fixture f;
    f.play();
    f.push(GST_MSECOND);
    check(bool(f.frames.pull()), "logical-pixel fixture did not prime");
    auto* description = logical_caps(variant);
    check(!f.apply_caps(description), "appsink negotiated non-mono/non-square logical pixels");
    // Relax only the test appsink's negotiation policy to reach the independent
    // production pull() validator with an actual GstSample of the wrong shape.
    auto* permissive = gst_caps_new_empty_simple("video/x-raw");
    gst_app_sink_set_caps(GST_APP_SINK(f.sink), permissive);
    gst_caps_unref(permissive);
    check(f.apply_caps(description), "permissive fixture did not admit test sample caps");
    f.direct(Fixture::buffer(2 * GST_MSECOND));
    rejects([&] { f.frames.pull(); },
            "sample validator accepted non-mono/non-square logical pixels");
    gst_caps_unref(description);
  }
  // NONE is unspecified/absent, not a literal caps mode called "none". Both
  // this default and explicit MONO keep the same full square-pixel contract.
  for (const auto* mode : {static_cast<const char*>(nullptr), "mono"}) {
    Fixture f;
    f.play();
    f.push(GST_MSECOND);
    check(bool(f.frames.pull()), "mono acceptance fixture did not prime");
    auto* description = logical_caps(9);
    auto* shape = gst_caps_get_structure(description, 0);
    if (!mode) {
      gst_structure_remove_field(shape, "multiview-mode");
      // Normal negotiation fixes unspecified input to mono (other groups).
      // Reach the parser's NONE sample mode explicitly without weakening the
      // production sink caps constraint.
      auto* permissive = gst_caps_new_empty_simple("video/x-raw");
      gst_app_sink_set_caps(GST_APP_SINK(f.sink), permissive);
      gst_caps_unref(permissive);
    }
    GstVideoInfo info;
    gst_video_info_init(&info);
    check(gst_video_info_from_caps(&info, description) &&
              GST_VIDEO_INFO_MULTIVIEW_MODE(&info) ==
                  (mode ? GST_VIDEO_MULTIVIEW_MODE_MONO : GST_VIDEO_MULTIVIEW_MODE_NONE),
          "normal NONE/MONO fixture does not carry its intended parsed mode");
    check(f.apply_caps(description), "normal NONE/MONO caps were rejected");
    f.direct(Fixture::buffer(2 * GST_MSECOND));
    check(bool(f.frames.pull()), "normal explicit NONE/MONO sample was rejected");
    gst_caps_unref(description);
  }
}

void drain_finalizes_weak_sink() {
  Fixture f(true);
  f.play();
  f.push(f.gate->arrival_time(), 2, true);
  auto image = f.frames.pull();
  check(bool(image), "replaceable adapter did not produce a frame");
  gst_object_unref(f.sink);
  f.sink = nullptr;
  f.gate->drain();
  check(f.frames.closed() && !f.frames.pull() && image->image->pixels.size() == 6,
        "weak adapter or retained image blocked output finalization");
  f.finish();
}
}  // namespace

int main() {
  try {
    gst_init(nullptr, nullptr);
    bounds_and_ownership();
    pixels_and_segment();
    bounded_queue_and_order();
    malformed_timestamps();
    missing_segment();
    non_time_segment();
    malformed_caps_and_storage();
    drain_finalizes_weak_sink();
    std::cout << "preview frame adapter: 8 lifecycle groups passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "preview frame adapter: " << error.what() << '\n';
    return 1;
  }
}
