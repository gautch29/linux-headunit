// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/gst_preview_output.hpp>
#include <gst/app/gstappsrc.h>
#include <gst/check/gsttestclock.h>
#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using headunit::GstPreviewOutput;
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
struct Capture {
  std::atomic<unsigned> count{0}, finalized{0};
  std::atomic<bool> block_finalization{false}, finalization_entered{false},
      release_finalization{false};
  std::mutex mutex;
  std::vector<GstClockTime> pts;
};
GstElement* sink(Capture& capture, bool bin = false) {
  auto* result = gst_element_factory_make("fakesink", nullptr);
  check(result, "missing fake output");
  g_object_set(result, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE,
               "signal-handoffs", TRUE, nullptr);
  g_signal_connect(result, "handoff",
                   G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
                     auto& c = *static_cast<Capture*>(data);
                     {
                       std::lock_guard lock(c.mutex);
                       c.pts.push_back(GST_BUFFER_PTS(buffer));
                     }
                     ++c.count;
                   }),
                   &capture);
  g_object_weak_ref(
      G_OBJECT(result),
      +[](gpointer data, GObject*) {
        auto& c = *static_cast<Capture*>(data);
        if (c.block_finalization) {
          c.finalization_entered = true;
          const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
          while (!c.release_finalization && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++c.finalized;
      },
      &capture);
  if (!bin) return result;
  auto* branch = gst_bin_new(nullptr);
  check(gst_bin_add(GST_BIN(branch), result), "cannot add output child");
  auto* pad = gst_element_get_static_pad(result, "sink");
  auto* ghost = gst_ghost_pad_new("sink", pad);
  gst_object_unref(pad);
  check(gst_element_add_pad(branch, ghost), "cannot expose output bin pad");
  return branch;
}
struct Fixture {
  GstElement *pipeline = nullptr, *source = nullptr, *valve = nullptr, *decoder = nullptr;
  GstClock* controlled = nullptr;
  std::unique_ptr<GstPreviewOutput> output;
  std::atomic<unsigned> decoded{0};
  explicit Fixture(bool h264 = false, bool test_clock = true) {
    GError* error = nullptr;
    const char* description =
        h264 ? "appsrc name=source is-live=true format=time block=false max-bytes=2097152 ! "
               "h264parse ! avdec_h264 name=decoder ! queue max-size-buffers=3 ! valve name=gate "
               "drop=true"
             : "appsrc name=source is-live=true format=time block=false max-bytes=2097152 ! queue "
               "max-size-buffers=3 ! valve name=gate drop=true";
    pipeline = gst_parse_launch(description, &error);
    check(pipeline && !error, "cannot build output fixture");
    source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    valve = gst_bin_get_by_name(GST_BIN(pipeline), "gate");
    if (h264) {
      decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
      auto* pad = gst_element_get_static_pad(decoder, "src");
      gst_pad_add_probe(
          pad, GST_PAD_PROBE_TYPE_BUFFER,
          +[](GstPad*, GstPadProbeInfo*, gpointer data) {
            ++*static_cast<std::atomic<unsigned>*>(data);
            return GST_PAD_PROBE_OK;
          },
          &decoded, nullptr);
      gst_object_unref(pad);
    }
    auto* caps = gst_caps_from_string(h264 ? "video/x-h264,stream-format=byte-stream,alignment=au"
                                           : "application/x-headunit-fixture");
    g_object_set(source, "caps", caps, nullptr);
    gst_caps_unref(caps);
    if (test_clock) {
      controlled = gst_test_clock_new_with_start_time(100 * GST_SECOND);
      gst_pipeline_use_clock(GST_PIPELINE(pipeline), controlled);
    }
    output = std::make_unique<GstPreviewOutput>(pipeline, valve);
  }
  ~Fixture() {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    output.reset();
    if (decoder) gst_object_unref(decoder);
    gst_object_unref(source);
    gst_object_unref(valve);
    gst_object_unref(pipeline);
    if (controlled) gst_object_unref(controlled);
  }
  void play() {
    check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
          "fixture cannot play");
    GstState actual = GST_STATE_NULL;
    check(
        gst_element_get_state(pipeline, &actual, nullptr, GST_SECOND) == GST_STATE_CHANGE_SUCCESS &&
            actual == GST_STATE_PLAYING,
        "fixture did not reach PLAYING");
  }
  void advance() {
    if (controlled) gst_test_clock_advance_time(GST_TEST_CLOCK(controlled), GST_MSECOND);
  }
  void push(GstClockTime pts, const std::vector<std::uint8_t>& bytes = {42}) {
    auto* buffer = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
    gst_buffer_fill(buffer, 0, bytes.data(), bytes.size());
    GST_BUFFER_PTS(buffer) = pts;
    check(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK,
          "source rejected fixture buffer");
  }
  void prime() {
    const auto count = output->stats().forwarded;
    push(output->arrival_time());
    wait([&] { return output->stats().forwarded > count; }, "drain did not observe stream/segment");
    advance();
  }
  void eos() {
    check(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK, "fixture EOS rejected");
    expect_eos();
  }
  void expect_eos() {
    auto* bus = gst_element_get_bus(pipeline);
    auto* message = gst_bus_timed_pop_filtered(
        bus, GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message) gst_message_unref(message);
    gst_object_unref(bus);
    check(ok, "hidden output did not complete EOS");
  }
};
void freshness_and_clock() {
  Capture first, second;
  Fixture f;
  f.play();
  f.prime();
  const auto equal = f.output->arrival_time();
  f.output->restore(sink(first));
  check(!f.output->stats().draining, "restore kept drain selected");
  const auto old = f.output->stats().rejected;
  f.push(equal - 1);
  f.push(equal);
  f.push(GST_CLOCK_TIME_NONE);
  wait([&] { return f.output->stats().rejected == old + 3; },
       "old/equal/missing PTS escaped admission");
  check(first.count == 0, "old video entered a new output cache");
  f.advance();
  f.push(f.output->arrival_time());
  wait([&] { return first.count == 1; }, "new running-time sample was not admitted");
  f.output->drain();
  check(first.finalized == 1, "old sink survived successful withdrawal");
  f.prime();
  f.output->restore(sink(second));
  f.advance();
  f.push(f.output->arrival_time());
  wait([&] { return second.count == 1; }, "second generation did not resume");
  const auto base = gst_element_get_base_time(f.pipeline);
  const auto before_change = f.output->arrival_time();
  gst_element_set_base_time(f.pipeline, base + 1);
  f.advance();
  f.push(before_change);
  wait([&] { return f.output->stats().barrier_retired; },
       "base-time change retained freshness authority");
  const auto rejected = f.output->stats().rejected;
  gst_element_set_base_time(f.pipeline, base);
  f.advance();
  f.push(before_change);
  wait([&] { return f.output->stats().rejected > rejected; },
       "clock retirement did not remain sticky");
  check(second.count == 1, "retired epoch admitted video");
  f.output->drain();
  check(second.finalized == 1, "second sink survived drain");
  rejects([&] { f.output->restore(sink(second)); }, "drain renewed a retired decoder's authority");
  check(second.finalized == 2, "retired restore candidate leaked");
  f.output->drain();
  f.eos();
}
void stream_identity_retirement() {
  for (unsigned variation = 0; variation < 3; ++variation) {
    Capture capture;
    Fixture f;
    f.play();
    f.prime();
    f.output->restore(sink(capture));
    auto* pad = gst_element_get_static_pad(f.valve, "src");
    auto* original = gst_pad_get_sticky_event(pad, GST_EVENT_STREAM_START, 0);
    check(original, "fixture has no stream identity");
    const gchar* name = nullptr;
    gst_event_parse_stream_start(original, &name);
    auto* changed = gst_event_new_stream_start(variation == 0 ? "different-decoder-stream" : name);
    gst_event_set_seqnum(changed, gst_event_get_seqnum(original));
    guint group = 0;
    const bool has_group = gst_event_parse_group_id(original, &group);
    if (has_group || variation == 1)
      gst_event_set_group_id(changed, variation == 1 ? group + 1 : group);
    GstStreamFlags flags = GST_STREAM_FLAG_NONE;
    gst_event_parse_stream_flags(original, &flags);
    gst_event_set_stream_flags(changed, variation == 2 ? GST_STREAM_FLAG_SELECT : flags);
    gst_event_unref(original);
    check(gst_pad_push_event(pad, changed), "changed identity could not reach gate");
    gst_object_unref(pad);
    check(f.output->stats().barrier_retired && f.output->stats().decoder_epoch_retired,
          "reused sequence number hid a changed stream identity");
    const auto old = f.output->stats().rejected;
    f.push(GST_MSECOND);
    wait([&] { return f.output->stats().rejected > old; },
         "changed stream admitted retained video");
    check(capture.count == 0, "new stream identity entered the old display");
    f.output->drain();
    f.eos();
  }
}
void eos_during_replacement() {
  Capture capture;
  Fixture f;
  f.output->start(sink(capture));
  f.play();
  f.prime();
  capture.block_finalization = true;
  std::exception_ptr failure;
  std::jthread replacement([&] {
    try {
      f.output->drain();
    } catch (...) {
      failure = std::current_exception();
    }
  });
  struct Release {
    Capture& capture;
    ~Release() { capture.release_finalization = true; }
  } release{capture};
  wait([&] { return capture.finalization_entered.load(); },
       "replacement never reached closed-gate interval");
  gboolean closed = FALSE;
  g_object_get(f.valve, "drop", &closed, nullptr);
  check(closed, "EOS fixture was not in the closed-gate interval");
  check(gst_app_src_end_of_stream(GST_APP_SRC(f.source)) == GST_FLOW_OK,
        "replacement EOS rejected by source");
  auto* input = gst_element_get_static_pad(f.valve, "sink");
  wait(
      [&] {
        auto* end = gst_pad_get_sticky_event(input, GST_EVENT_EOS, 0);
        if (end) gst_event_unref(end);
        return end != nullptr;
      },
      "EOS did not arrive while valve was closed");
  gst_object_unref(input);
  capture.release_finalization = true;
  replacement.join();
  if (failure) std::rethrow_exception(failure);
  check(capture.finalized == 1 && f.output->stats().draining,
        "EOS replacement retained its old output");
  f.expect_eos();
  // EOS is now also sticky on the valve source. A second new output must still
  // receive it, with no new buffer, synthetic flush or fabricated stream ID.
  f.output->drain();
  f.expect_eos();
}
void flush_during_replacement() {
  Capture capture;
  Fixture f;
  f.output->start(sink(capture));
  f.play();
  f.prime();
  const auto old = f.output->arrival_time();
  auto* source_pad = gst_element_get_static_pad(f.valve, "src");
  auto* original = gst_pad_get_sticky_event(source_pad, GST_EVENT_SEGMENT, 0);
  gst_object_unref(source_pad);
  check(original, "flush fixture has no original segment");
  capture.block_finalization = true;
  std::exception_ptr failure;
  std::jthread replacement([&] {
    try {
      f.output->drain();
    } catch (...) {
      failure = std::current_exception();
    }
  });
  struct Release {
    Capture& capture;
    ~Release() { capture.release_finalization = true; }
  } release{capture};
  wait([&] { return capture.finalization_entered.load(); },
       "flush replacement never closed its gate");
  auto* input = gst_element_get_static_pad(f.valve, "sink");
  check(gst_pad_send_event(input, gst_event_new_flush_start()), "hidden flush start rejected");
  check(gst_pad_send_event(input, gst_event_new_flush_stop(FALSE)), "hidden flush stop rejected");
  check(gst_pad_send_event(input, original), "unchanged hidden segment rejected");
  gst_object_unref(input);
  check(f.output->stats().decoder_epoch_retired, "closed valve hid a decoder flush");
  capture.release_finalization = true;
  replacement.join();
  if (failure) std::rethrow_exception(failure);
  const auto count = f.output->stats().forwarded;
  f.push(old);
  wait([&] { return f.output->stats().forwarded > count; },
       "flushed output could not continue draining");
  rejects([&] { f.output->restore(sink(capture)); },
          "same segment renewed a flushed decoder epoch");
  check(capture.finalized == 2, "flushed restore candidate leaked");
  f.output->drain();
  f.eos();
}
void segment_retirement() {
  for (unsigned variation = 0; variation < 2; ++variation) {
    Capture capture;
    Fixture f;
    f.play();
    f.prime();
    f.output->restore(sink(capture));
    auto* pad = gst_element_get_static_pad(f.valve, "src");
    auto* original = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0);
    check(original, "fixture has no time segment");
    const GstSegment* observed = nullptr;
    gst_event_parse_segment(original, &observed);
    GstSegment segment = *observed;
    if (variation == 1) ++segment.base;
    auto* changed = gst_event_new_segment(&segment);
    if (variation == 1) gst_event_set_seqnum(changed, gst_event_get_seqnum(original));
    gst_event_unref(original);
    check(gst_pad_push_event(pad, changed), "new segment could not reach gate");
    gst_object_unref(pad);
    check(f.output->stats().barrier_retired && f.output->stats().decoder_epoch_retired,
          "new segment identity/mapping retained old return authority");
    f.advance();
    const auto dropped = f.output->stats().rejected;
    f.push(GST_MSECOND);
    wait([&] { return f.output->stats().rejected > dropped; },
         "discontinuous segment admitted frame");
    check(capture.count == 0, "segment discontinuity entered output");
    f.output->drain();
    check(capture.finalized == 1, "segment output not finalized");
    f.eos();
  }
}
void retained_sink_and_bus() {
  Capture capture, next;
  Fixture f;
  auto* branch = sink(capture, true);
  gst_object_ref_sink(branch);
  // Keep an explicit external reference after start consumes its own reference.
  gst_object_ref(branch);
  f.output->start(branch);
  f.play();
  auto* bus = gst_element_get_bus(f.pipeline);
  GError* error =
      g_error_new_literal(GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "original output error");
  auto* message = gst_message_new_error(GST_OBJECT(branch), error, "retained error evidence");
  const auto serial = gst_message_get_seqnum(message);
  g_error_free(error);
  check(gst_bus_post(bus, message), "cannot post output error");
  auto* unrelated =
      gst_message_new_application(GST_OBJECT(f.source), gst_structure_new_empty("unrelated"));
  const auto unrelated_serial = gst_message_get_seqnum(unrelated);
  check(gst_bus_post(bus, unrelated), "cannot post unrelated event");
  rejects([&] { f.output->drain(); }, "withdrawal ignored an externally retained output");
  check(capture.finalized == 0, "external output lifetime not respected");
  message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  check(message && GST_MESSAGE_SRC(message) == GST_OBJECT(f.pipeline) &&
            gst_message_get_seqnum(message) == serial,
        "output error was lost or retained the obsolete sink");
  gchar* detail = nullptr;
  error = nullptr;
  gst_message_parse_error(message, &error, &detail);
  check(error && std::string(error->message) == "original output error" &&
            std::string(detail) == "retained error evidence",
        "original output error payload changed");
  g_error_free(error);
  g_free(detail);
  gst_message_unref(message);
  message = gst_bus_pop_filtered(bus, GST_MESSAGE_APPLICATION);
  check(message && gst_message_get_seqnum(message) == unrelated_serial &&
            GST_MESSAGE_SRC(message) == GST_OBJECT(f.source),
        "unrelated bus message changed");
  gst_message_unref(message);
  gst_object_unref(bus);
  gst_object_unref(branch);
  f.output->drain();
  check(capture.finalized == 1, "nested old sink was not finalized before successful return");
  f.prime();
  f.output->restore(sink(next));
  f.advance();
  f.push(f.output->arrival_time());
  wait([&] { return next.count == 1; }, "retained-output recovery did not resume");
  f.output->drain();
  check(next.finalized == 1, "last output retained");
  f.eos();
}
void repeated_generations() {
  Capture capture;
  Fixture f;
  f.play();
  f.prime();
  for (unsigned i = 0; i < 24; ++i) {
    f.output->restore(sink(capture, true));
    f.advance();
    f.push(f.output->arrival_time());
    wait([&] { return capture.count == i + 1; }, "repeated output did not receive a new frame");
    f.output->drain();
    check(capture.finalized == i + 1, "generation retained a sink");
    f.prime();
  }
  check(f.output->stats().generation == 49, "unexpected output generation count");
  f.eos();
}
void rejected_restore() {
  Capture capture;
  Fixture f;
  rejects([&] { f.output->restore(sink(capture)); }, "restore invented a PLAYING clock/segment");
  check(capture.finalized == 1, "rejected candidate leaked");
  f.output->drain();
  f.play();
  f.prime();
  rejects([&] { GstPreviewOutput too_late(f.pipeline, f.valve); },
          "construction allowed active streaming callbacks");
  rejects([&] { f.output->start(sink(capture)); }, "late startup bypassed freshness barrier");
  check(capture.finalized == 2, "late initial output leaked");
  f.eos();
}
void changed_clock() {
  Capture capture;
  Fixture f;
  f.play();
  f.prime();
  f.output->restore(sink(capture));
  auto* replacement = gst_test_clock_new_with_start_time(gst_clock_get_time(f.controlled));
  check(gst_element_set_clock(f.pipeline, replacement), "cannot change fixture clock");
  const auto count = f.output->stats().rejected;
  f.push(GST_MSECOND);
  wait([&] { return f.output->stats().rejected > count; }, "new clock reused old barrier");
  check(f.output->stats().barrier_retired && capture.count == 0,
        "new clock identity admitted a frame");
  f.output->drain();
  check(capture.finalized == 1, "clock-change sink retained");
  f.eos();
  gst_object_unref(replacement);
}
void hidden_epoch_change() {
  Capture capture;
  Fixture f;
  f.play();
  f.prime();
  const auto old = f.output->arrival_time();
  // A later base moves the new running-time floor backward. An old queued
  // sample could otherwise look newer than that floor after camera return.
  gst_test_clock_advance_time(GST_TEST_CLOCK(f.controlled), GST_SECOND);
  gst_element_set_base_time(f.pipeline, gst_element_get_base_time(f.pipeline) + GST_SECOND);
  rejects([&] { f.output->restore(sink(capture)); }, "hidden base change admitted old queued PTS");
  check(f.output->stats().decoder_epoch_retired && capture.finalized == 1,
        "hidden decoder epoch was renewed or candidate retained");
  f.output->drain();
  const auto count = f.output->stats().forwarded;
  f.push(old);
  wait([&] { return f.output->stats().forwarded > count; },
       "retired hidden decoder stopped draining");
  rejects([&] { f.output->restore(sink(capture)); }, "second drain revived the old decoder epoch");
  check(capture.count == 0 && capture.finalized == 2, "retired sample entered a restored output");
  f.output->drain();
  f.eos();
}
void arrival_epoch_change() {
  Capture capture;
  Fixture f;
  f.play();
  // Timestamp encoded input before any decoded stream/segment has arrived.
  // Its queued PTS cannot be rebound to a reset base at first decoded output.
  const auto old = f.output->arrival_time();
  gst_test_clock_advance_time(GST_TEST_CLOCK(f.controlled), GST_SECOND);
  gst_element_set_base_time(f.pipeline, gst_element_get_base_time(f.pipeline) + GST_SECOND);
  rejects([&] { (void)f.output->arrival_time(); },
          "arrival stamping renewed the first encoded frame's epoch");
  f.push(old);
  wait([&] { return f.output->stats().forwarded > 0; }, "initial retired frame did not drain");
  rejects([&] { f.output->restore(sink(capture)); }, "first decoded frame rebound old queued time");
  check(capture.finalized == 1 && capture.count == 0 && f.output->stats().decoder_epoch_retired,
        "old arrival epoch gained a display");
  f.output->drain();
  f.eos();
}
std::uint64_t integer(const std::vector<std::uint8_t>& bytes, std::size_t at, unsigned count) {
  check(at + count <= bytes.size(), "truncated H.264 fixture");
  std::uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i)
    value |= std::uint64_t(bytes[at + i]) << (8 * i);
  return value;
}
void h264_dependencies(const char* path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  check(bytes.size() > 5 && bytes.size() <= 2097152 &&
            std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 5) ==
                std::vector<std::uint8_t>{'U', 'H', 'V', 'P', 1},
        "invalid generated packet fixture");
  Capture capture;
  Fixture f(true, false);
  f.play();
  std::vector<std::uint8_t> configuration;
  unsigned frames = 0, resumes = 0;
  for (std::size_t at = 5; at < bytes.size();) {
    const auto kind = integer(bytes, at, 1), length = integer(bytes, at + 9, 4);
    at += 13;
    check(at + length <= bytes.size(), "truncated generated packet");
    if (kind == 1) {
      // AVCC SPS/PPS -> Annex B for the actual production parser/decoder.
      const auto end = at + length;
      check(length >= 7, "invalid fixture AVCC");
      std::size_t cursor = at + 6;
      auto nals = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
          check(cursor + 2 <= end, "truncated parameter length");
          const unsigned size = (unsigned(bytes[cursor]) << 8) | bytes[cursor + 1];
          cursor += 2;
          check(cursor + size <= end, "truncated parameter set");
          configuration.insert(configuration.end(), {0, 0, 0, 1});
          configuration.insert(configuration.end(), bytes.begin() + cursor,
                               bytes.begin() + cursor + size);
          cursor += size;
        }
      };
      nals(bytes[at + 5] & 31);
      check(cursor < end, "missing PPS count");
      nals(bytes[cursor++]);
      check(cursor == end, "extra fixture AVCC data");
    } else {
      check(kind == 0 && !configuration.empty(), "fixture frame precedes configuration");
      std::vector<std::uint8_t> au;
      if (frames == 0) au = configuration;
      const auto end = at + length;
      unsigned idr = 0, dependent = 0;
      for (std::size_t cursor = at; cursor < end;) {
        check(end - cursor >= 4, "truncated NAL length");
        unsigned size = 0;
        for (unsigned i = 0; i < 4; ++i)
          size = (size << 8) | bytes[cursor + i];
        cursor += 4;
        check(size && cursor + size <= end, "invalid fixture NAL");
        idr += (bytes[cursor] & 31) == 5;
        dependent += (bytes[cursor] & 31) == 1;
        au.insert(au.end(), {0, 0, 0, 1});
        au.insert(au.end(), bytes.begin() + cursor, bytes.begin() + cursor + size);
        cursor += size;
      }
      check(frames == 0 ? idr > 0 : (idr == 0 && dependent > 0),
            "fixture does not require retained H.264 references");
      if (frames == 3 || frames == 8 || frames == 13) {
        f.output->restore(sink(capture));
        ++resumes;
      }
      f.push(f.output->arrival_time(), au);
      ++frames;
      wait([&] { return f.decoded == frames; }, "dependent frame failed after output replacement");
      if (frames == 7 || frames == 12 || frames == 17) {
        wait([&] { return capture.count == resumes * 4; },
             "restored output missed dependent frames");
        f.output->drain();
        check(capture.finalized == resumes, "decoded output survived drain");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    at += length;
  }
  check(frames == 18 && resumes == 3 && f.decoded == 18 && capture.count == 12,
        "continuous H.264 decoding/output replacement counts differ");
  f.eos();
  std::cout << "18/18 H.264 frames decoded; three dependent-frame display restorations passed\n";
}
}  // namespace
int main(int argc, char** argv) try {
  gst_init(nullptr, nullptr);
  freshness_and_clock();
  segment_retirement();
  retained_sink_and_bus();
  repeated_generations();
  rejected_restore();
  changed_clock();
  hidden_epoch_change();
  stream_identity_retirement();
  eos_during_replacement();
  arrival_epoch_change();
  flush_during_replacement();
  std::cout << "11 decoded-output lifecycle groups passed\n";
  if (argc == 2)
    h264_dependencies(argv[1]);
  else
    check(argc == 1, "usage: preview-output-tests [GENERATED_UHVP_FIXTURE]");
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
