// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/standard_preview.hpp>
#include <headunit/gst_preview_output.hpp>
#include <headunit/gst_video_preview.hpp>
#include <headunit/theme/scene.hpp>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/check/gsttestclock.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

using Preview = headunit::StandardPreview;
using Worker = headunit::StandardPreviewWorker;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
template <class F>
void wait(F ready, const char* why) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!ready()) {
    check(std::chrono::steady_clock::now() < deadline, why);
    std::this_thread::sleep_for(1ms);
  }
}
// Public GStreamer signal introspection only; no private preview fields or
// production hooks. Reference identities prove the parser/decoder are retained.
struct PipelineTrace {
  GstElement *pipeline = nullptr, *source = nullptr, *parser = nullptr, *decoder = nullptr;
  guint signal = 0;
  gulong hook = 0;
  unsigned parsers = 0, decoders = 0;
  PipelineTrace() {
    auto* bin_class = g_type_class_ref(GST_TYPE_BIN);
    signal = g_signal_lookup("element-added", GST_TYPE_BIN);
    g_type_class_unref(bin_class);
    check(signal, "GStreamer bin signal introspection unavailable");
    hook = g_signal_add_emission_hook(
        signal, 0,
        +[](GSignalInvocationHint*, guint count, const GValue* values, gpointer data) -> gboolean {
          if (count != 2) return TRUE;
          auto& trace = *static_cast<PipelineTrace*>(data);
          auto* parent = GST_ELEMENT(g_value_get_object(&values[0]));
          auto* child = GST_ELEMENT(g_value_get_object(&values[1]));
          if (!parent || !child || !GST_IS_PIPELINE(parent)) return TRUE;
          auto* factory = gst_element_get_factory(child);
          if (!factory) return TRUE;
          const auto* kind = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
          if (!trace.pipeline && g_strcmp0(kind, "appsrc") == 0 &&
              g_strcmp0(GST_OBJECT_NAME(child), "source") == 0)
            trace.pipeline = GST_ELEMENT(gst_object_ref(parent));
          if (trace.pipeline != parent) return TRUE;
          if (g_strcmp0(kind, "appsrc") == 0 && !trace.source)
            trace.source = GST_ELEMENT(gst_object_ref(child));
          if (g_strcmp0(kind, "h264parse") == 0) {
            ++trace.parsers;
            if (!trace.parser) trace.parser = GST_ELEMENT(gst_object_ref(child));
          }
          if (g_strcmp0(kind, "avdec_h264") == 0) {
            ++trace.decoders;
            if (!trace.decoder) trace.decoder = GST_ELEMENT(gst_object_ref(child));
          }
          return TRUE;
        },
        this, nullptr);
    check(hook, "cannot observe actual preview pipeline identities");
  }
  ~PipelineTrace() {
    if (hook) g_signal_remove_emission_hook(signal, hook);
    for (auto* object : {source, parser, decoder, pipeline})
      if (object) gst_object_unref(object);
  }
  void before_worker() const {
    check(pipeline && source && parser && decoder && parsers == 1 && decoders == 1,
          "production Standard controller did not retain one parser/decoder");
    GstState actual = GST_STATE_NULL;
    gst_element_get_state(pipeline, &actual, nullptr, 0);
    check(actual <= GST_STATE_READY, "production worker constructed after PLAYING");
  }
  void unchanged_and_playing() const {
    check(parsers == 1 && decoders == 1 && GST_OBJECT_PARENT(source) == GST_OBJECT(pipeline) &&
              GST_OBJECT_PARENT(parser) == GST_OBJECT(pipeline) &&
              GST_OBJECT_PARENT(decoder) == GST_OBJECT(pipeline),
          "camera lifecycle replaced original H.264 source/parser/decoder");
    for (auto* object : {parser, decoder}) {
      GstState actual = GST_STATE_NULL;
      check(gst_element_get_state(object, &actual, nullptr, 0) == GST_STATE_CHANGE_SUCCESS &&
                actual == GST_STATE_PLAYING,
            "camera lifecycle stopped retained H.264 reference state");
    }
  }
};
struct FakeWorker {
  Worker::Status state;
  Worker::Options options;
  bool visible = true;
  std::optional<std::uint64_t> floor, last;
  std::optional<headunit::GstPreviewFrames::Frame> latest;
  std::deque<headunit::TouchInput> touches;
  std::optional<headunit::TouchInput> delivered;
  mutable std::mutex event_mutex;
  std::vector<std::string> events;
  unsigned arms = 0, destructions = 0;
  bool acknowledge_next_show = false;
  PipelineTrace* trace = nullptr;
  void event(std::string value) {
    std::lock_guard lock(event_mutex);
    events.push_back(std::move(value));
  }
  std::size_t index(const std::string& value) const {
    std::lock_guard lock(event_mutex);
    const auto found = std::find(events.begin(), events.end(), value);
    return std::size_t(found - events.begin());
  }
  void acknowledge(Worker::Generation generation, bool show = true) {
    state.observed_monotonic_ns = headunit::presentation_monotonic_ns();
    state.window = show ? generation : 0;
    state.window_ready = show;
    if (!show) state.retired = std::max(state.retired, generation);
    event("ack-" + std::to_string(generation));
  }
  void cancel() {
    touches.clear();
    if (delivered && delivered->pressed) touches.push_back({false, delivered->x, delivered->y});
  }
  void touch(headunit::TouchInput report) {
    if (!state.touch_ready || state.source_suspended || !visible) return;
    if (touches.size() == 16)
      cancel();
    else
      touches.push_back(report);
  }
  void receipt(Worker::Generation generation) {
    state.observed_monotonic_ns = headunit::presentation_monotonic_ns();
    check(latest.has_value(), "fake worker has no source to present");
    if (generation != state.requested || !visible || !state.source_armed || state.source_suspended)
      return;
    ++state.presented;
    state.presented_arrival_running_ns = latest->arrival_running_ns;
    state.touch_ready = true;
  }
};
FakeWorker* active_worker = nullptr;
struct CurrentWorker {
  explicit CurrentWorker(FakeWorker& worker) {
    check(!active_worker, "overlapping controller fixtures");
    active_worker = &worker;
  }
  ~CurrentWorker() { active_worker = nullptr; }
};
}  // namespace

// Explicit synchronous test double. Production StandardPreview and real
// GstPreviewOutput/GstPreviewFrames/GStreamer run; worker/GPU/Wayland do not.
// Manual readiness and receipt are scheduling inputs, not GPU qualification.
namespace headunit {
struct StandardPreviewWorker::Impl {
  FakeWorker* fake = nullptr;
};
StandardPreviewWorker::StandardPreviewWorker(Options options) : impl_(std::make_unique<Impl>()) {
  check(active_worker, "controller created a worker outside its explicit fixture");
  impl_->fake = active_worker;
  auto& fake = *impl_->fake;
  fake.options = std::move(options);
  fake.visible = fake.options.initially_visible;
  if (fake.trace) fake.trace->before_worker();
  if (!fake.visible) fake.state.retired = 1;
  fake.event(fake.visible ? "construct-visible" : "construct-hidden");
}
StandardPreviewWorker::~StandardPreviewWorker() {
  ++impl_->fake->destructions;
}
StandardPreviewWorker::Generation StandardPreviewWorker::request_window(bool show) {
  auto& fake = *impl_->fake;
  fake.state.observed_monotonic_ns = presentation_monotonic_ns();
  check(!fake.state.faulted, "faulted fake worker cannot accept requests");
  ++fake.state.requested;
  fake.visible = show;
  fake.cancel();
  fake.latest.reset();
  fake.floor.reset();
  fake.last.reset();
  fake.state.window_ready = false;
  fake.state.source_armed = false;
  fake.state.source_suspended = false;
  fake.state.touch_ready = false;
  fake.state.presented_arrival_running_ns.reset();
  fake.event(std::string(show ? "show-" : "hide-") + std::to_string(fake.state.requested));
  if (show && fake.acknowledge_next_show) {
    fake.acknowledge_next_show = false;
    fake.acknowledge(fake.state.requested);
  }
  return fake.state.requested;
}
bool StandardPreviewWorker::suspend_source(Generation generation) {
  auto& fake = *impl_->fake;
  if (generation != fake.state.requested || fake.state.faulted) return false;
  fake.state.source_suspended = true;
  fake.state.source_armed = false;
  fake.state.touch_ready = false;
  fake.latest.reset();
  fake.cancel();
  fake.event("suspend-" + std::to_string(generation));
  return true;
}
StandardPreviewWorker::Status StandardPreviewWorker::status() const {
  return impl_->fake->state;
}
bool StandardPreviewWorker::arm_source(Generation generation, std::optional<std::uint64_t> floor) {
  auto& fake = *impl_->fake;
  if (fake.state.faulted || fake.state.source_suspended || !fake.visible ||
      generation != fake.state.requested || generation != fake.state.window ||
      !fake.state.window_ready || fake.state.source_armed)
    return false;
  check(floor.has_value() || generation == 1, "return source lacks original arrival floor");
  ++fake.arms;
  fake.floor = floor;
  fake.state.source_armed = true;
  fake.event("arm-" + std::to_string(generation));
  return true;
}
bool StandardPreviewWorker::offer(Generation generation, GstPreviewFrames::Frame frame) {
  auto& fake = *impl_->fake;
  if (fake.state.faulted || fake.state.source_suspended || !fake.visible ||
      !fake.state.source_armed || generation != fake.state.requested ||
      (fake.floor && frame.arrival_running_ns <= *fake.floor) ||
      (fake.last && frame.arrival_running_ns <= *fake.last)) {
    ++fake.state.rejected;
    return false;
  }
  check(frame.image && frame.image->width == fake.options.width &&
            frame.image->height == fake.options.height &&
            frame.image->pixels.size() == std::size_t(fake.options.width) * fake.options.height,
        "controller offered mismatched immutable decoded pixels");
  if (fake.latest) ++fake.state.coalesced;
  fake.last = frame.arrival_running_ns;
  fake.latest = std::move(frame);
  ++fake.state.accepted;
  ++fake.state.submitted;
  fake.event("offer-" + std::to_string(generation));
  return true;
}
int StandardPreviewWorker::input_fd() const noexcept {
  return 42;
}
unsigned StandardPreviewWorker::drain_touches(const std::function<bool(TouchInput)>& sender) {
  auto& fake = *impl_->fake;
  unsigned count = 0;
  check(bool(sender), "controller passed an empty touch sender");
  while (!fake.touches.empty()) {
    const auto report = fake.touches.front();
    if (!sender(report)) break;
    fake.touches.pop_front();
    fake.delivered = report;
    ++count;
  }
  return count;
}
}  // namespace headunit

namespace {
struct Fixture {
  FakeWorker worker;
  CurrentWorker current{worker};
  std::filesystem::path directory, request;
  std::shared_ptr<const headunit::camera::ActivationControl> camera;
  bool native_page = false;
  GstElement *pipeline = nullptr, *source = nullptr, *valve = nullptr;
  GstClock* clock = nullptr;
  std::unique_ptr<headunit::GstPreviewOutput> output;
  std::unique_ptr<Preview> preview;
  std::atomic<unsigned> sink_finalized{0};
  explicit Fixture(bool camera_on = false, bool page = false) : native_page(page) {
    char pattern[] = "/tmp/headunit-standard-controller-XXXXXX";
    const auto* created = ::mkdtemp(pattern);
    check(created, "camera fixture directory unavailable");
    directory = std::filesystem::canonical(created);
    request = directory / (native_page ? "home.request" : "camera.request");
    activate(camera_on);
    if (!native_page)
      camera = std::make_shared<headunit::camera::ActivationControl>(request.string());
    GError* error = nullptr;
    pipeline = gst_parse_launch(
        "appsrc name=source is-live=true format=time block=false max-bytes=2097152 ! "
        "video/x-raw,format=BGRx,width=320,height=240,framerate=0/1,pixel-aspect-ratio=1/1,"
        "interlace-mode=progressive,multiview-mode=mono,views=1 ! valve name=gate drop=true",
        &error);
    check(pipeline && !error, "controller GStreamer fixture unavailable");
    source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    valve = gst_bin_get_by_name(GST_BIN(pipeline), "gate");
    auto* caps = gst_caps_from_string(
        "video/x-raw,format=BGRx,width=320,height=240,framerate=0/1,pixel-aspect-ratio=1/1,"
        "interlace-mode=progressive,multiview-mode=mono,views=1");
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    clock = gst_test_clock_new_with_start_time(100 * GST_SECOND);
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
    output = std::make_unique<headunit::GstPreviewOutput>(pipeline, valve);
    Preview::Options options;
    options.presenter.width = 320;
    options.presenter.height = 240;
    options.presenter.window.output_name = "fixture-main";
    if (native_page)
      options.presenter.native_page_control = std::make_shared<headunit::MainSurfaceControl>(
          request.string(), (directory / "main.lease").string());
    else {
      options.presenter.surface_lease_file = (directory / "main.lease").string();
      options.camera_control = camera;
    }
    preview = std::make_unique<Preview>(*output, std::move(options));
  }
  ~Fixture() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    preview.reset();
    output.reset();
    if (source) gst_object_unref(source);
    if (valve) gst_object_unref(valve);
    if (pipeline) gst_object_unref(pipeline);
    if (clock) gst_object_unref(clock);
    camera.reset();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  void activate(bool on) {
    const int fd =
        ::open(request.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    check(fd >= 0, "cannot write private camera fixture request");
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const std::string text =
        on ? (native_page ? "on " + std::to_string(now + 1000000000) + "\n" : "on\n") : "off\n";
    const auto written = ::write(fd, text.data(), text.size());
    const int result = ::close(fd);
    check(written == ssize_t(text.size()) && result == 0,
          "private camera fixture request incomplete");
  }
  void play() {
    check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
          "controller fixture cannot play");
    GstState actual = GST_STATE_NULL;
    check(
        gst_element_get_state(pipeline, &actual, nullptr, GST_SECOND) == GST_STATE_CHANGE_SUCCESS &&
            actual == GST_STATE_PLAYING,
        "controller fixture did not reach PLAYING");
  }
  void advance() { gst_test_clock_advance_time(GST_TEST_CLOCK(clock), GST_MSECOND); }
  void push(std::optional<std::uint64_t> pts = {}) {
    const auto before = output->stats();
    auto* bytes = gst_buffer_new_allocate(nullptr, 320 * 240 * 4, nullptr);
    check(bytes, "controller raw fixture allocation failed");
    GST_BUFFER_PTS(bytes) = pts ? *pts : output->arrival_time();
    check(gst_app_src_push_buffer(GST_APP_SRC(source), bytes) == GST_FLOW_OK,
          "controller raw input rejected");
    wait(
        [&] {
          const auto now = output->stats();
          return now.forwarded + now.rejected > before.forwarded + before.rejected;
        },
        "controller raw input did not reach actual gate");
  }
  void offered(std::uint64_t count) {
    wait(
        [&] {
          preview->pump();
          return worker.state.accepted >= count;
        },
        "actual decoded image did not reach fake worker");
  }
  void initial() {
    play();
    worker.acknowledge(1);
    preview->pump();
    advance();
    push();
    offered(1);
    worker.receipt(1);
    check(preview->status().presenter.touch_ready,
          "manual initial receipt did not qualify fixture touch");
  }
  bool faults() {
    try {
      preview->pump();
    } catch (const std::exception&) {
      return preview->status().stage == Preview::Stage::faulted;
    }
    return preview->status().stage == Preview::Stage::faulted;
  }
  GstElement* sink() {
    auto* children = gst_bin_iterate_recurse(GST_BIN(pipeline));
    GValue value = G_VALUE_INIT;
    GstElement* result = nullptr;
    while (gst_iterator_next(children, &value) == GST_ITERATOR_OK) {
      if (GST_IS_APP_SINK(g_value_get_object(&value))) {
        result = GST_ELEMENT(g_value_dup_object(&value));
        g_value_reset(&value);
        break;
      }
      g_value_reset(&value);
    }
    if (G_IS_VALUE(&value)) g_value_unset(&value);
    gst_iterator_free(children);
    return result;
  }
  void watch_sink() {
    auto* child = sink();
    check(child, "initial controller output has no appsink");
    g_object_weak_ref(
        G_OBJECT(child),
        +[](gpointer data, GObject*) {
          auto& f = *static_cast<Fixture*>(data);
          ++f.sink_finalized;
          f.worker.event("sink-finalized");
        },
        this);
    gst_object_unref(child);
  }
  void eos() {
    check(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK,
          "controller fixture EOS rejected");
    auto* bus = gst_element_get_bus(pipeline);
    auto* message = gst_bus_timed_pop_filtered(
        bus, GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message) gst_message_unref(message);
    gst_object_unref(bus);
    check(ok, "controller fixture did not complete EOS");
  }
};

void initial_window_and_receipt() {
  Fixture f;
  check(!f.output->stats().draining && f.preview->input_fd() == 42,
        "initial controller output/readiness missing");
  f.play();
  f.advance();
  f.push();
  f.preview->pump();
  check(f.worker.arms == 0 && f.worker.state.accepted == 0 && !f.worker.state.touch_ready,
        "initial frame or pending window ACK armed source/touch");
  f.worker.acknowledge(1);
  f.preview->pump();
  check(f.worker.arms == 1 && !f.worker.floor,
        "initial source incorrectly requires a camera-return floor");
  f.offered(1);
  check(!f.worker.state.touch_ready, "decoded image alone qualified fixture touch");
  f.worker.touch({true, 50, 60});
  check(f.preview->drain_touches([](headunit::TouchInput) { return true; }) == 0,
        "unpresented source accepted input");
  f.worker.receipt(1);
  f.worker.touch({true, 70, 80});
  check(f.preview->drain_touches(
            [](headunit::TouchInput report) { return report.pressed && report.x == 70; }) == 1,
        "presented fixture source did not delegate touch");
}

void delayed_ack_and_exact_return_floor() {
  Fixture f;
  f.initial();
  f.watch_sink();
  f.activate(true);
  f.preview->pump();
  check(f.preview->status().camera_active && f.preview->status().stage == Preview::Stage::hidden &&
            f.output->stats().draining && f.sink_finalized == 1,
        "camera entry retained decoded output");
  check(f.worker.index("suspend-1") < f.worker.index("sink-finalized") &&
            f.worker.index("sink-finalized") < f.worker.index("hide-2"),
        "camera entry did not suspend, drain, then retire window");
  const auto offered = f.worker.state.accepted;
  f.advance();
  f.push();
  f.preview->pump();
  check(f.worker.state.accepted == offered, "hidden drain forwarded image to presenter");
  f.activate(false);
  f.preview->pump();
  const auto generation = f.preview->status().window_generation;
  check(generation == 3 && f.output->stats().draining && !f.worker.state.source_armed,
        "return bypassed matching window ACK");
  f.worker.acknowledge(1);
  f.preview->pump();
  check(f.output->stats().draining && f.worker.arms == 1, "stale window ACK resumed old source");
  f.worker.acknowledge(generation);
  f.preview->pump();
  const auto floor = f.output->stats().arrival_floor;
  check(floor && f.worker.floor == floor && f.worker.arms == 2 && !f.worker.state.touch_ready,
        "controller did not pin the actual restore running-time floor");
  const auto rejected = f.output->stats().rejected;
  f.push(*floor);
  f.push(*floor - 1);
  f.preview->pump();
  check(f.output->stats().rejected >= rejected + 2 && f.worker.state.accepted == offered,
        "old/equal arrival reached returned presenter");
  f.advance();
  f.push();
  f.offered(offered + 1);
  check(!f.worker.state.touch_ready, "fresh arrival enabled touch before matching fixture receipt");
  f.worker.receipt(generation);
  check(f.preview->status().presenter.touch_ready, "matching fresh fixture receipt failed");
}

void native_page_uses_original_output_boundary() {
  Fixture f(false, true);
  f.initial();
  f.activate(true);
  f.preview->pump();
  check(!f.preview->status().camera_active && f.preview->status().native_page_active &&
            f.preview->status().stage == Preview::Stage::hidden && f.output->stats().draining,
        "native Home did not withdraw through the Standard decoder drain");
  const auto offered = f.worker.state.accepted;
  f.advance();
  f.push();
  f.preview->pump();
  check(f.worker.state.accepted == offered && !f.worker.state.touch_ready,
        "native Home retained decoded presentation or touch");
  f.activate(false);
  f.preview->pump();
  const auto returned = f.preview->status().window_generation;
  check(returned == 3 && f.output->stats().draining && !f.worker.state.source_armed,
        "native Home return bypassed the original window ACK");
  f.worker.acknowledge(returned);
  f.preview->pump();
  const auto floor = f.output->stats().arrival_floor;
  check(!f.preview->status().native_page_active && floor && f.worker.floor == floor,
        "native Home restore lost the original arrival boundary");
  f.push(*floor);
  f.preview->pump();
  check(f.worker.state.accepted == offered, "native Home return replayed a pre-return frame");
  f.advance();
  f.push();
  f.offered(offered + 1);
  f.worker.receipt(returned);
  check(f.preview->status().presenter.touch_ready,
        "native Home return did not restore fresh-frame touch");
}

void rapid_requests_and_reactivation() {
  Fixture f;
  f.initial();
  f.activate(true);
  f.preview->pump();
  f.activate(false);
  f.preview->pump();
  check(f.preview->status().window_generation == 3, "rapid return request missing");
  f.activate(true);
  f.preview->pump();
  check(f.preview->status().window_generation == 4 && f.output->stats().draining,
        "reactivation before ACK skipped retirement generation");
  f.worker.acknowledge(3);
  f.preview->pump();
  check(!f.worker.state.source_armed && !f.worker.state.touch_ready && f.worker.arms == 1,
        "late returned window ACK revived camera-covered source");
  f.activate(false);
  f.preview->pump();
  f.worker.acknowledge(5);
  f.preview->pump();
  check(f.preview->status().window_generation == 5 && f.worker.floor && f.worker.arms == 2,
        "rapid on/off failed to restore only the final generation");
}

void initially_hidden_without_segment() {
  Fixture f(true);
  check(!f.worker.options.initially_visible && f.output->stats().draining,
        "initial camera created a phone window or decoded output");
  f.play();
  f.activate(false);
  f.preview->pump();
  f.worker.acknowledge(2);
  f.preview->pump();
  check(f.preview->status().window_generation == 2 && f.worker.arms == 0 &&
            f.output->stats().draining,
        "initial hidden return fabricated stream/segment readiness");
  f.advance();
  f.push();
  f.preview->pump();
  check(f.worker.arms == 1 && f.worker.floor == f.output->stats().arrival_floor && f.worker.floor,
        "observed original decoder did not arm hidden-start return");
  f.advance();
  f.push();
  f.offered(1);
  check(!f.worker.state.touch_ready, "hidden-start return fabricated presentation");
  f.worker.receipt(2);
  check(f.worker.state.touch_ready, "hidden-start matching fixture receipt failed");
}

void held_touch_release_backpressure() {
  Fixture f;
  f.initial();
  f.worker.touch({true, 121, 82});
  check(f.preview->drain_touches([](headunit::TouchInput report) { return report.pressed; }) == 1,
        "fixture held press was not delivered");
  f.activate(true);
  f.preview->pump();
  unsigned attempts = 0;
  check(f.preview->drain_touches([&](headunit::TouchInput report) {
    ++attempts;
    check(!report.pressed && report.x == 121 && report.y == 82,
          "cancellation lost delivered coordinates");
    return false;
  }) == 0 && attempts == 1,
        "backpressured cancellation was not attempted");
  f.preview->pump();
  f.worker.touch({true, 1, 2});
  std::vector<headunit::TouchInput> sent;
  check(f.preview->drain_touches([&](headunit::TouchInput report) {
    sent.push_back(report);
    return true;
  }) == 1 && sent.size() == 1 &&
            !sent.front().pressed && sent.front().x == 121,
        "camera transition lost retained release or forwarded hidden press");
  check(f.preview->drain_touches([](headunit::TouchInput) { return true; }) == 0,
        "release duplicated after acceptance");
}

void drain_failure_retires_window() {
  Fixture f;
  f.initial();
  f.worker.touch({true, 231, 192});
  check(f.preview->drain_touches([](headunit::TouchInput report) { return report.pressed; }) == 1,
        "fault fixture held contact was not delivered");
  auto* retained = f.sink();
  check(retained, "drain-failure fixture lacks sink");
  f.activate(true);
  const bool faulted = f.faults();
  check(faulted && !f.worker.visible && !f.worker.state.source_armed && !f.worker.state.touch_ready,
        "failed sink finalization retained phone window/source/touch");
  check(f.worker.index("suspend-1") < f.worker.index("hide-2"),
        "drain failure bypassed source suspension/window retirement");
  unsigned rejected = 0;
  check(f.preview->drain_touches([&](headunit::TouchInput report) {
    ++rejected;
    check(!report.pressed && report.x == 231 && report.y == 192,
          "fault cancellation lost delivered contact");
    return false;
  }) == 0 && rejected == 1,
        "faulted controller lost a backpressured cancellation");
  // The containing receiver can stop decoding while retaining its touch owner
  // until the connection queue accepts that original release.
  gst_element_set_state(f.pipeline, GST_STATE_NULL);
  check(f.preview->input_fd() == 42 && f.worker.destructions == 0,
        "optional fault/pipeline stop destroyed cancellation owner");
  check(f.preview->drain_touches([](headunit::TouchInput report) {
    return !report.pressed && report.x == 231 && report.y == 192;
  }) == 1 && f.preview->drain_touches([](headunit::TouchInput) { return true; }) == 0,
        "faulted controller failed to retry/retire its retained release exactly once");
  gst_object_unref(retained);
}

void hidden_eos_cannot_qualify_return() {
  Fixture f;
  f.initial();
  f.activate(true);
  f.preview->pump();
  f.advance();
  f.push();
  f.eos();
  f.activate(false);
  f.preview->pump();
  const auto returned = f.preview->status().window_generation;
  f.worker.acknowledge(returned);
  wait([&] { return f.faults(); }, "hidden EOS return did not retire optional presenter");
  check(!f.worker.visible && !f.worker.state.touch_ready && !f.worker.latest,
        "EOS replay qualified stale source or touch");
}

void decoder_clock_retirement() {
  for (bool hidden : {false, true}) {
    Fixture f;
    f.initial();
    if (hidden) {
      f.activate(true);
      f.preview->pump();
    }
    const auto old = f.output->arrival_time();
    gst_element_set_base_time(f.pipeline, gst_element_get_base_time(f.pipeline) + 1);
    f.advance();
    f.push(old);
    check(f.output->stats().decoder_epoch_retired,
          "actual decoder epoch did not retire after changed base time");
    // A hidden camera owner can stay active independently; trying to restore
    // its phone view must fail permanently rather than repin the decoder.
    if (hidden) f.activate(false);
    check(f.faults() && !f.worker.visible && !f.worker.state.touch_ready,
          "retired decoder clock kept optional source/window active");
  }
}

void presenter_fault_retires_decoded_output() {
  Fixture f;
  f.initial();
  f.watch_sink();
  f.worker.state.faulted = true;
  check(f.faults() && f.output->stats().draining && f.sink_finalized == 1,
        "presenter fault retained actual decoded-output cache");
}

#ifdef HEADUNIT_HAVE_STANDARD_GPU
std::vector<headunit::AirplayVideoPacket> packets(const char* path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
  check(bytes.size() > 5 && bytes.size() <= 2097152 &&
            std::equal(bytes.begin(), bytes.begin() + 5,
                       std::vector<std::uint8_t>{'U', 'H', 'V', 'P', 1}.begin()),
        "invalid generated UHVP controller fixture");
  auto integer = [&](std::size_t at, unsigned count) {
    check(at <= bytes.size() && count <= bytes.size() - at, "truncated generated packet field");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
      value |= std::uint64_t(bytes[at + i]) << (8 * i);
    return value;
  };
  std::vector<headunit::AirplayVideoPacket> result;
  unsigned frames = 0, configs = 0;
  for (std::size_t at = 5; at < bytes.size();) {
    const auto kind = integer(at, 1), timestamp = integer(at + 1, 8), length = integer(at + 9, 4);
    at += 13;
    check(kind <= 1 && at <= bytes.size() && length <= bytes.size() - at && length > 0,
          "invalid generated packet record");
    if (kind == 1) {
      check(result.empty(), "generated configuration is not first");
      ++configs;
    } else {
      check(configs == 1, "generated dependent frame precedes configuration");
      unsigned idr = 0, dependent = 0;
      for (std::size_t nal = at; nal < at + length;) {
        check(at + length - nal >= 4, "truncated generated NAL field");
        unsigned size = 0;
        for (unsigned i = 0; i < 4; ++i)
          size = (size << 8) | bytes[nal + i];
        nal += 4;
        check(size && size <= at + length - nal, "invalid generated NAL size");
        idr += (bytes[nal] & 31) == 5;
        dependent += (bytes[nal] & 31) == 1;
        nal += size;
      }
      check(frames == 0 ? idr > 0 : (idr == 0 && dependent > 0),
            "generated fixture does not require retained H.264 references");
      ++frames;
    }
    result.push_back(
        {kind ? headunit::AirplayVideoPacket::Kind::config
              : headunit::AirplayVideoPacket::Kind::frame,
         timestamp,
         {bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + length)}});
    at += length;
  }
  check(configs == 1 && frames == 18 && result.size() == 19,
        "generated fixture is not one config plus 18 frames");
  return result;
}

struct EncodedBlock {
  GstPad* pad = nullptr;
  gulong probe = 0;
  std::atomic<bool> entered{false};
  std::atomic<std::uint64_t> pts{GST_CLOCK_TIME_NONE};
  std::atomic<std::uint64_t> dts{GST_CLOCK_TIME_NONE};
  explicit EncodedBlock(GstElement* source) {
    pad = gst_element_get_static_pad(source, "src");
    check(pad, "actual encoded source has no pad");
    probe = gst_pad_add_probe(
        pad, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER),
        +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
          auto& block = *static_cast<EncodedBlock*>(data);
          block.pts = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info));
          block.dts = GST_BUFFER_DTS(GST_PAD_PROBE_INFO_BUFFER(info));
          block.entered = true;
          return GST_PAD_PROBE_OK;
        },
        this, nullptr);
    check(probe, "cannot hold actual pre-return encoded input");
  }
  ~EncodedBlock() {
    release();
    if (pad) gst_object_unref(pad);
  }
  void release() {
    if (probe) {
      gst_pad_remove_probe(pad, probe);
      probe = 0;
    }
  }
};

struct BufferTimestampTrace {
  GstPad* pad = nullptr;
  gulong probe = 0;
  std::atomic<std::uint64_t> pts{GST_CLOCK_TIME_NONE};
  std::atomic<std::uint64_t> dts{GST_CLOCK_TIME_NONE};
  std::atomic<unsigned> count{0};
  explicit BufferTimestampTrace(GstElement* element) {
    pad = gst_element_get_static_pad(element, "src");
    check(pad, "actual H.264 element lacks its source pad");
    probe = gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
          auto& trace = *static_cast<BufferTimestampTrace*>(data);
          trace.pts = GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info));
          trace.dts = GST_BUFFER_DTS(GST_PAD_PROBE_INFO_BUFFER(info));
          ++trace.count;
          return GST_PAD_PROBE_OK;
        },
        this, nullptr);
    check(probe, "cannot observe actual H.264 timestamp propagation");
  }
  ~BufferTimestampTrace() {
    if (probe) gst_pad_remove_probe(pad, probe);
    if (pad) gst_object_unref(pad);
  }
};

void production_h264_controller(const char* path) {
  const auto encoded = packets(path);
  FakeWorker worker;
  CurrentWorker current(worker);
  char pattern[] = "/tmp/headunit-standard-h264-XXXXXX";
  const auto* created = ::mkdtemp(pattern);
  check(created, "actual decoded controller directory unavailable");
  struct Directory {
    std::filesystem::path path;
    ~Directory() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } directory{created};
  const auto request = directory.path / "camera.request";
  auto activate = [&](bool on) {
    const int fd =
        ::open(request.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    check(fd >= 0, "actual controller camera request unavailable");
    const char* text = on ? "on\n" : "off\n";
    const int size = on ? 3 : 4;
    const auto written = ::write(fd, text, size);
    const int closed = ::close(fd);
    check(written == size && closed == 0, "actual controller camera request incomplete");
  };
  activate(false);
  auto camera = std::make_shared<headunit::camera::ActivationControl>(request.string());
  PipelineTrace trace;
  worker.trace = &trace;
  headunit::MainPreviewOptions options;
  options.wayland_display = "fixture-wayland";
  options.output_name = "fixture-main";
  options.camera_control = camera;
  options.camera_lease_file = (directory.path / "main.lease").string();
  auto preview = std::make_unique<headunit::GstVideoPreview>("wayland-gpu", 320, 240, "avdec_h264",
                                                             "", options);
  BufferTimestampTrace source_stamps(trace.source), parsed_stamps(trace.parser),
      decoded_stamps(trace.decoder);
  check(worker.arms == 0 && !worker.state.touch_ready && worker.options.initially_visible,
        "production preview armed source before initial window ACK");
  preview->submit(encoded.front());
  worker.acknowledge(1);
  auto pump = [&] {
    (void)preview->drain_touches([](headunit::TouchInput) { return true; });
  };
  pump();
  check(worker.arms == 1 && !worker.floor,
        "production initial source arm uses fabricated return floor");
  auto exact_arrival = [&](unsigned frames) {
    wait([&] { return decoded_stamps.count.load() == frames; },
         "actual decoded timestamp trace did not progress");
    check(source_stamps.count == frames && parsed_stamps.count == frames &&
              GST_CLOCK_TIME_IS_VALID(source_stamps.pts.load()) &&
              source_stamps.dts == source_stamps.pts && parsed_stamps.pts == source_stamps.pts &&
              parsed_stamps.dts == source_stamps.pts && decoded_stamps.pts == source_stamps.pts,
          "actual H.264 pipeline inferred a different original arrival timestamp");
  };
  auto send = [&](unsigned index, std::optional<std::uint64_t> offered = {}) {
    preview->submit(encoded.at(index + 1));
    wait(
        [&] {
          pump();
          return preview->decoded_frames() == index + 1 &&
                 (!offered || worker.state.accepted >= *offered);
        },
        "retained production H.264 decoder/source did not progress");
    if (offered)
      check(worker.state.accepted == *offered,
            "production presenter accepted unexpected old decoded arrival");
    exact_arrival(index + 1);
    trace.unchanged_and_playing();
  };
  for (unsigned index = 0; index < 4; ++index)
    send(index, index + 1);
  check(!worker.state.touch_ready, "actual decoding alone qualified production input");
  check(!preview->main_presentation_status().qualified,
        "actual decoding alone published main readiness");
  worker.receipt(1);
  check(preview->main_presentation_status().qualified,
        "matching actual source/fake receipt did not qualify readiness");

  // This buffer receives its genuine production arrival PTS before appsrc
  // queues it. Hold it upstream while the output drains and later restores.
  {
    EncodedBlock held(trace.source);
    preview->submit(encoded.at(5));
    wait([&] { return held.entered.load(); }, "encoded pre-return fixture did not block");
    check(GST_CLOCK_TIME_IS_VALID(held.pts.load()) && preview->decoded_frames() == 4,
          "held encoded fixture lacks its original arrival timestamp");
    activate(true);
    pump();
    check(worker.state.requested == 2 && !worker.visible && !worker.state.touch_ready,
          "production camera entry retained phone window/input");
    check(!preview->main_presentation_status().qualified,
          "camera-hidden main was advertised as usable Home return");
    trace.unchanged_and_playing();
    activate(false);
    pump();
    check(worker.state.requested == 3 && !worker.state.source_armed,
          "production return bypassed delayed window ACK");
    worker.acknowledge(1);
    pump();
    check(worker.arms == 1, "production stale ACK armed a return");
    worker.acknowledge(3);
    pump();
    check(worker.arms == 2 && worker.floor && held.pts.load() < *worker.floor &&
              !worker.state.touch_ready,
          "production return failed to pin a later original pipeline floor");
    held.release();
    wait(
        [&] {
          pump();
          return preview->decoded_frames() == 5;
        },
        "pre-return dependent H.264 frame did not decode");
    exact_arrival(5);
  }
  // A subsequent dependent frame serially follows the held one. Its single
  // accepted offer proves the old encoded arrival never became a display source.
  send(5, 5);
  check(worker.latest && worker.latest->arrival_running_ns > *worker.floor &&
            !worker.state.touch_ready,
        "pre-return encoded timestamp was relabelled or enabled touch");
  worker.receipt(3);
  check(worker.state.touch_ready, "fresh returned source lacks matching fake receipt");
  for (unsigned index = 6; index < 9; ++index)
    send(index, index);

  activate(true);
  pump();
  check(worker.state.requested == 4 && !worker.visible, "second production camera entry missing");
  const auto accepted = worker.state.accepted;
  for (unsigned index = 9; index < 12; ++index)
    send(index);
  check(worker.state.accepted == accepted, "hidden dependent frames entered production presenter");
  // Model the new window becoming ready during request_window(): the first
  // frame delivered after camera-off must keep its entry-time arrival stamp,
  // even when submit() itself pumps the complete restoration. A pad block lets
  // us compare that real production PTS with the newly pinned return floor.
  {
    EncodedBlock held(trace.source);
    worker.acknowledge_next_show = true;
    activate(false);
    preview->submit(encoded.at(13));
    wait([&] { return held.entered.load(); }, "camera-off first-submit frame did not block");
    check(worker.state.requested == 5 && worker.arms == 3 && worker.floor &&
              GST_CLOCK_TIME_IS_VALID(held.pts.load()) && held.pts.load() <= *worker.floor &&
              preview->decoded_frames() == 12 && worker.state.accepted == accepted &&
              !worker.state.touch_ready,
          "camera-off submit relabelled an existing frame after its restoration floor");
    trace.unchanged_and_playing();
    held.release();
    wait(
        [&] {
          pump();
          return preview->decoded_frames() == 13;
        },
        "camera-off first-submit dependent frame did not decode");
    exact_arrival(13);
  }
  check(worker.state.accepted == accepted && !worker.state.touch_ready,
        "camera-off first-submit pre-restore source was offered or qualified input");
  for (unsigned index = 13; index < 18; ++index)
    send(index, accepted + index - 12);
  check(preview->decoded_frames() == 18 && trace.parsers == 1 && trace.decoders == 1,
        "camera handoff lost H.264 references or replaced original decoder");
  worker.receipt(5);
  worker.touch({true, 212, 143});
  check(preview->drain_touches([](headunit::TouchInput report) { return report.pressed; }) == 1,
        "production owner did not deliver held fixture contact");
  auto* injected = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
                                       "synthetic retained controller error");
  auto* error_message =
      gst_message_new_error(GST_OBJECT(trace.decoder), injected, "generated controller fixture");
  g_error_free(injected);
  check(gst_element_post_message(trace.decoder, error_message),
        "actual decoder bus rejected fixture error");
  bool bus_error = false;
  try {
    pump();
  } catch (const std::exception& error) {
    bus_error = std::string(error.what()) == "video preview pipeline reported an error";
  }
  check(bus_error && preview->decoded_frames() == 18 && worker.destructions == 0,
        "production owner missed a real bus error without another encoded submission");
  check(preview->finish() == 18, "production H.264 EOS lost decoded frames");
  preview->retire();
  GstState actual = GST_STATE_PLAYING;
  gst_element_get_state(trace.pipeline, &actual, nullptr, 0);
  check(actual == GST_STATE_NULL && worker.destructions == 0 && preview->input_fd() == 42,
        "production retire lost cancellation owner before connection teardown");
  unsigned rejected = 0;
  check(preview->drain_touches([&](headunit::TouchInput report) {
    ++rejected;
    check(!report.pressed && report.x == 212 && report.y == 143,
          "production retired cancellation changed contact");
    return false;
  }) == 0 && rejected == 1,
        "production retire lost rejected contact release");
  check(preview->drain_touches([](headunit::TouchInput report) {
    return !report.pressed && report.x == 212 && report.y == 143;
  }) == 1 && preview->drain_touches([](headunit::TouchInput) { return true; }) == 0,
        "production stopped pipeline failed to retain/retry its original cancellation");
  bool retired_input = false;
  try {
    preview->submit(encoded.at(1));
  } catch (const std::exception&) {
    retired_input = true;
  }
  check(retired_input, "retired production preview accepted another encoded input");
  preview.reset();
  check(worker.destructions == 1, "production teardown did not finalize its retained worker");
  std::cout << "18/18 dependent H.264 frames decoded through production Standard preview; camera "
               "returns, old arrival exclusion and retained cancellation passed\n";
}

void production_readiness_terminal(const char* path) {
  const auto encoded = packets(path);
  for (bool clock_loss : {false, true}) {
    FakeWorker worker;
    CurrentWorker current(worker);
    char pattern[] = "/tmp/headunit-standard-readiness-XXXXXX";
    const auto* created = ::mkdtemp(pattern);
    check(created, "readiness fixture directory unavailable");
    struct Directory {
      std::filesystem::path path;
      ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } folder{std::filesystem::canonical(created)};
    const auto request = folder.path / "home.request";
    auto activate = [&](bool on) {
      // Match ActivationControl's existing C++ clock on this platform. Darwin
      // steady_clock and CLOCK_MONOTONIC differ; wire status always uses the
      // latter, and this fixture makes no Linux request-clock qualification.
      const auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
      const auto text = on ? "on " + std::to_string(now + 900000000) + "\n" : std::string("off\n");
      const int fd =
          ::open(request.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
      check(fd >= 0, "readiness Home request unavailable");
      const auto written = ::write(fd, text.data(), text.size());
      const auto closed = ::close(fd);
      check(written == ssize_t(text.size()) && closed == 0, "readiness Home request incomplete");
    };
    activate(false);
    headunit::MainPreviewOptions options;
    options.wayland_display = "fixture-wayland";
    options.output_name = "fixture-main";
    options.native_page_control = std::make_shared<headunit::MainSurfaceControl>(
        request.string(), (folder.path / "main.lease").string());
    PipelineTrace trace;
    worker.trace = &trace;
    headunit::GstVideoPreview preview("wayland-gpu", 320, 240, "avdec_h264", "", options);
    check(!preview.main_presentation_status().qualified,
          "startup readiness qualified without stream or receipt");
    preview.submit(encoded.front());
    worker.acknowledge(1);
    bool retain_release = false;
    auto pump = [&] {
      (void)preview.drain_touches(
          [&](headunit::TouchInput report) { return report.pressed || !retain_release; });
    };
    pump();
    for (unsigned index = 0; index < 4; ++index) {
      preview.submit(encoded.at(index + 1));
      wait(
          [&] {
            pump();
            return preview.decoded_frames() == index + 1 && worker.state.accepted == index + 1;
          },
          "readiness decoder failed progress");
    }
    check(!preview.main_presentation_status().qualified,
          "decode counters qualified readiness without receipt");
    worker.receipt(1);
    check(preview.main_presentation_status().qualified,
          "source-correlated fixture receipt failed readiness");
    worker.touch({true, 111, 112});
    check(preview.drain_touches([](headunit::TouchInput value) { return value.pressed; }) == 1,
          "readiness held press failed");
    retain_release = true;
    activate(true);
    pump();
    const auto home = preview.main_presentation_status();
    check(home.home && home.healthy && home.qualified,
          "healthy retained decoder lost Home eligibility");
    trace.unchanged_and_playing();
    if (clock_loss) {
      auto* clock = gst_element_get_clock(trace.pipeline);
      check(clock, "actual pipeline clock unavailable");
      check(gst_element_post_message(trace.pipeline,
                                     gst_message_new_clock_lost(GST_OBJECT(trace.pipeline), clock)),
            "clock loss message rejected");
      gst_object_unref(clock);
    } else
      check(gst_app_src_end_of_stream(GST_APP_SRC(trace.source)) == GST_FLOW_OK,
            "readiness actual EOS rejected");
    wait(
        [&] { return !preview.main_presentation_status().qualified && worker.state.requested > 2; },
        "hidden EOS/clock loss retained published readiness");
    GstState actual = GST_STATE_PLAYING;
    gst_element_get_state(trace.pipeline, &actual, nullptr, 0);
    check(actual == GST_STATE_NULL && !worker.visible && !worker.state.touch_ready &&
              worker.destructions == 0,
          "terminal readiness failed pipeline/window retirement or destroyed release owner");
    unsigned rejected = 0;
    check(preview.drain_touches([&](headunit::TouchInput report) {
      ++rejected;
      check(!report.pressed && report.x == 111 && report.y == 112,
            "terminal readiness changed cancellation");
      return false;
    }) == 0 && rejected == 1,
          "terminal readiness lost rejected cancellation");
    check(preview.drain_touches([](headunit::TouchInput report) { return !report.pressed; }) == 1,
          "terminal readiness failed late cancellation delivery");
    check(!preview.main_presentation_status().qualified, "retired readiness revived");
  }
  std::cout << "Standard readiness: actual retained H.264 Home, EOS and clock-loss retirement "
               "passed (explicit fake GPU receipts)\n";
}

void production_return_diagnostics(const char* path) {
  const auto encoded = packets(path);
  const auto* original = std::getenv("HEADUNIT_STANDARD_DIAGNOSTICS");
  struct Environment {
    std::optional<std::string> original;
    ~Environment() {
      if (original)
        ::setenv("HEADUNIT_STANDARD_DIAGNOSTICS", original->c_str(), 1);
      else
        ::unsetenv("HEADUNIT_STANDARD_DIAGNOSTICS");
    }
  } environment{original ? std::optional<std::string>(original) : std::nullopt};
  for (bool enabled : {false, true}) {
    if (enabled)
      ::setenv("HEADUNIT_STANDARD_DIAGNOSTICS", "1", 1);
    else
      ::unsetenv("HEADUNIT_STANDARD_DIAGNOSTICS");
    std::ostringstream output;
    struct Capture {
      std::streambuf* original;
      ~Capture() { std::cerr.rdbuf(original); }
    } capture{std::cerr.rdbuf(output.rdbuf())};
    FakeWorker worker;
    CurrentWorker current(worker);
    char pattern[] = "/tmp/headunit-standard-diagnostic-XXXXXX";
    const auto* created = ::mkdtemp(pattern);
    check(created, "return diagnostic fixture directory unavailable");
    struct Directory {
      std::filesystem::path path;
      ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } folder{std::filesystem::canonical(created)};
    const auto request = folder.path / "home.request";
    auto activate = [&](bool on) {
      const auto now = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
      const auto text = on ? "on " + std::to_string(now + 900000000) + "\n" : std::string("off\n");
      const int fd =
          ::open(request.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
      check(fd >= 0, "return diagnostic Home request unavailable");
      check(::write(fd, text.data(), text.size()) == ssize_t(text.size()),
            "return diagnostic Home request incomplete");
      check(::close(fd) == 0, "return diagnostic Home request close failed");
    };
    activate(false);
    headunit::MainPreviewOptions options;
    options.wayland_display = "fixture-wayland";
    options.output_name = "fixture-main";
    options.native_page_control = std::make_shared<headunit::MainSurfaceControl>(
        request.string(), (folder.path / "main.lease").string());
    headunit::GstVideoPreview preview("wayland-gpu", 320, 240, "avdec_h264", "", options);
    auto pump = [&] {
      (void)preview.drain_touches([](headunit::TouchInput) { return true; });
    };
    preview.submit(encoded.front());
    worker.acknowledge(1);
    pump();
    preview.submit(encoded.at(1));
    wait(
        [&] {
          pump();
          return worker.state.accepted == 1 && preview.decoded_frames() == 1;
        },
        "diagnostic startup did not decode actual H.264");
    worker.receipt(1);
    check(preview.main_presentation_status().qualified,
          "diagnostic startup lacked fixture receipt");
    check(output.str().empty(), "return diagnostic emitted before observing Home");
    activate(true);
    pump();
    worker.acknowledge(worker.state.requested, false);
    check(preview.main_presentation_status().home, "return diagnostic did not observe Home");
    const auto home_log = output.str();
    for (unsigned i = 0; i < 100; ++i)
      (void)preview.main_presentation_status();
    check(output.str() == home_log, "steady Home consumed periodic diagnostic budget");
    activate(false);
    pump();
    check(!preview.main_presentation_status().qualified && !worker.state.window_ready,
          "return qualified before window ACK");
    worker.acknowledge(worker.state.requested);
    pump();
    check(!preview.main_presentation_status().qualified && worker.floor &&
              worker.state.source_armed && !worker.state.touch_ready,
          "return diagnostics changed returned-generation freshness admission");
    const auto returned_log = output.str();
    std::this_thread::sleep_for(270ms);
    (void)preview.main_presentation_status();
    if (enabled) {
      check(output.str().size() > returned_log.size(),
            "unready return lacked periodic diagnostic counters");
      const auto text = output.str();
      check(
          text.find("stage=returning nativeHome=0") != std::string::npos &&
              text.find("windowReady=1 sourceArmed=1 sourceSuspended=0 touchReady=0") !=
                  std::string::npos &&
              text.find("encodedIngress=1") != std::string::npos &&
              text.find(" decoded=1 ") != std::string::npos &&
              text.substr(text.rfind("Standard return diagnostic ")).find(" sourceFloor=none") ==
                  std::string::npos,
          "trace failed to distinguish delayed window ACK from armed return without fresh source");
    }
    preview.submit(encoded.at(2));
    wait(
        [&] {
          pump();
          return worker.state.accepted == 2 && preview.decoded_frames() == 2;
        },
        "diagnostic return failed fresh actual decode");
    worker.receipt(worker.state.requested);
    check(preview.main_presentation_status().qualified,
          "diagnostic return lacked fresh fixture receipt");
    const auto ready_log = output.str();
    std::this_thread::sleep_for(270ms);
    (void)preview.main_presentation_status();
    check(output.str() == ready_log, "ready return consumed periodic diagnostic budget");
    if (enabled) {
      const auto line = ready_log.substr(ready_log.rfind("Standard return diagnostic "));
      auto number = [&](const char* key) {
        const auto field = std::string(" ") + key + "=";
        const auto at = line.find(field);
        check(at != std::string::npos, "return trace lacks an observation/provenance field");
        return std::stoull(line.substr(at + field.size()));
      };
      check(number("encodedIngress") == 2 && number("decoded") == 2 && number("accepted") == 2 &&
                number("presented") == 2 && number("ingressMonoNs") > 0 &&
                number("ingressMonoNs") <= number("mono_ns") &&
                number("ingressRunningNs") > number("sourceFloor") &&
                number("presentedArrivalNs") == number("ingressRunningNs"),
            "return trace lost actual ingress date, fresh floor or source-correlated receipt");
    }
    for (unsigned i = 0; i < 40; ++i) {
      activate(true);
      pump();
      worker.acknowledge(worker.state.requested, false);
      (void)preview.main_presentation_status();
      activate(false);
      pump();
      (void)preview.main_presentation_status();
      worker.acknowledge(worker.state.requested);
      pump();
      (void)preview.main_presentation_status();
    }
    const auto text = output.str();
    check(enabled ? std::count(text.begin(), text.end(), '\n') == 64 : text.empty(),
          "return diagnostic was enabled by default or refilled its lifetime event budget");
  }
  std::cout << "Standard return diagnostics: default-off, window/source distinctions, periodic "
               "unready counters and lifetime64 cap passed (actual H.264; fixture receipts)\n";
}
#endif
}  // namespace

int main(int argc, char** argv) {
  try {
    gst_init(nullptr, nullptr);
    initial_window_and_receipt();
    delayed_ack_and_exact_return_floor();
    rapid_requests_and_reactivation();
    native_page_uses_original_output_boundary();
    initially_hidden_without_segment();
    held_touch_release_backpressure();
    drain_failure_retires_window();
    hidden_eos_cannot_qualify_return();
    decoder_clock_retirement();
    presenter_fault_retires_decoded_output();
    std::cout << "Standard controller: 10 actual GStreamer/fake-presenter groups passed\n";
    check(argc == 1 || argc == 2, "usage: standard-controller-tests [GENERATED_UHVP_FIXTURE]");
    if (argc == 2) {
#ifdef HEADUNIT_HAVE_STANDARD_GPU
      production_h264_controller(argv[1]);
      production_readiness_terminal(argv[1]);
      production_return_diagnostics(argv[1]);
#else
      throw std::runtime_error(
          "generated production preview fixture needs HEADUNIT_HAVE_STANDARD_GPU");
#endif
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Standard controller: " << error.what() << '\n';
    return 1;
  }
}
