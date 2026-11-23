// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/binary_plist.hpp>
#include <headunit/camera_control.hpp>
#include <headunit/camera_display.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <headunit/theme/gpu_display.hpp>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace c = headunit::camera;
namespace g = headunit::theme::graphics;
namespace s = headunit::theme::script;
namespace v = headunit::theme::video;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
std::uint64_t ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}
const auto overall_deadline = Clock::now() + 54s;
void bounded() {
  check(Clock::now() < overall_deadline, "surface handoff exceeded its total time budget");
}
template <class F>
void until(F done, const char* why) {
  const auto deadline = Clock::now() + 4s;
  while (!done()) {
    bounded();
    check(Clock::now() < deadline, why);
    std::this_thread::sleep_for(5ms);
  }
}
g::Image pixels() {
  return {64, 64, std::vector<std::uint32_t>(4096, 0xff2288dd)};
}

struct PrivateFiles {
  std::filesystem::path directory, request, lease;
  PrivateFiles() {
    char pattern[] = "/tmp/headunit-surface-probe-XXXXXX";
    const auto* path = ::mkdtemp(pattern);
    check(path, "private fixture directory failed");
    directory = path;
    request = directory / "request";
    lease = directory / "surface";
  }
  ~PrivateFiles() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  void set(bool on) {
    const auto temporary = directory / "request-next";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check(fd >= 0, "fixture request creation failed");
    const auto text = on ? std::string_view("on\n") : std::string_view("off\n");
    std::size_t offset = 0;
    bool good = ::fchmod(fd, 0600) == 0;
    while (good && offset < text.size()) {
      const auto written = ::write(fd, text.data() + offset, text.size() - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) {
        good = false;
        break;
      }
      offset += std::size_t(written);
    }
    const bool closed = ::close(fd) == 0;
    check(good && closed && ::rename(temporary.c_str(), request.c_str()) == 0,
          "fixture request replacement failed");
  }
};

// CPU source with an explicit bounded ingress and strict preparation receipts.
// Hidden pumping consumes ingress only; it never offers a frame to the stream.
class Input final : public v::DisplayVideoInput {
 public:
  explicit Input(std::shared_ptr<v::Stream> stream) : stream_(std::move(stream)) {}
  const std::shared_ptr<v::Stream>& stream() const override { return stream_; }
  void request() {
    check(queued < 4, "fixture ingress exhausted");
    ++queued;
  }
  void pump(const v::MediaClock* clock) override {
    if (clock)
      ++mapped_pumps;
    else {
      ++null_pumps;
      drained += queued;
      queued = 0;
    }
  }
  std::optional<std::uint64_t> stage(const v::MediaClock& clock) override {
    ++stage_calls;
    if (!queued) return {};
    const auto at = ns();
    check(stream_->offer(clock.capture_at(at, at).pts_seconds, pixels()),
          "fixture stream exhausted");
    --queued;
    const auto frame = stream_->prepare_next();
    check(bool(frame), "fixture preparation absent");
    outstanding.push_back(frame->token);
    ++staged;
    return frame->token;
  }
  void complete(std::uint64_t token, bool presented) override {
    check(!outstanding.empty() && outstanding.front() == token && stream_->prepared(token),
          "fixture receipt lost or reordered");
    if (presented) {
      stream_->presented(token);
      ++shown;
    } else
      stream_->discard_prepared(token);
    outstanding.pop_front();
    ++completed;
  }
  void suspend() override {
    if (before_suspend) before_suspend();
    retired += outstanding.size();
    outstanding.clear();
    stream_->discard_all();
  }
  std::function<void()> before_suspend;
  std::deque<std::uint64_t> outstanding;
  unsigned queued = 0;
  std::uint64_t null_pumps = 0, mapped_pumps = 0, drained = 0, stage_calls = 0, staged = 0,
                completed = 0, shown = 0, retired = 0;

 private:
  std::shared_ptr<v::Stream> stream_;
};

struct Panel {
  s::Display spec;
  s::Runtime runtime;
  std::shared_ptr<Input> input;
  std::uint64_t clock_samples = 0, loads = 0, starts = 0, hidden_echoes = 0;
  bool loaded = false, started = false;
  std::optional<double> last_pts;
  std::unique_ptr<g::GpuDisplay> display;
  Panel(std::string name, const std::string& socket, std::uint64_t origin,
        std::optional<g::GpuDisplayOptions::SurfaceLease> lease = {})
      : spec{std::move(name), 128, 64},
        runtime(
            [](std::string_view) -> std::string {
              throw std::runtime_error("original fixture has no resources");
            },
            spec) {
    runtime.set_connected(true);
    runtime.bind_log_sink([this](int, std::string_view tag, std::string_view message) {
      check(tag == "fixture", "unexpected fixture log tag");
      if (message == "loaded")
        ++loads;
      else if (message == "started")
        ++starts;
      else if (message == "echo-hidden")
        ++hidden_echoes;
      else
        check(false, "unexpected fixture log event");
    });
    input = std::make_shared<Input>(runtime.add_video_input({"fixture-input", 64, 64}));
    g::GpuDisplayOptions options;
    options.window.display_name = socket;
    options.window.output_name = "headless";
    options.window.title = "Headunit original surface handoff fixture " + spec.identifier;
    options.window.app_id = "org.headunit.surface-probe." + spec.identifier;
    options.frame_interval_ns = 10000000;
    options.scene_completion_timeout_ns = 2000000000;
    options.inputs = {input};
    options.surface_lease = std::move(lease);
    display = std::make_unique<g::GpuDisplay>(
        runtime, spec,
        [this, origin]() -> std::optional<headunit::AirplayClockSample> {
          ++clock_samples;
          const auto at = ns(), elapsed = at - origin;
          // Network synchronization alone is synthetic. Every presentation and
          // correlation is supplied by the real isolated Wayland compositor.
          const auto media =
              ((900 + elapsed / 1000000000) << 32) + ((elapsed % 1000000000) << 32) / 1000000000;
          return headunit::AirplayClockSample{media + (std::uint64_t{2208988800} << 32), at, at,
                                              1000};
        },
        std::move(options));
  }
  s::UiWorker::DisplayTurn step() {
    using Phase = s::UiWorker::DisplayPhase;
    const auto turn = display->turn(started ? Phase::frame : Phase::bootstrap);
    if (turn.defer_startup)
      check(!started && !turn.allow_tasks && !turn.pts && !turn.inline_task_ns,
            "malformed initial deferral");
    if (turn.allow_tasks) {
      check(turn.pts.has_value(), "fixture owner allowed a fabricated nil timestamp");
      last_pts = turn.pts;
      if (!loaded) {
        runtime.tick(turn.pts);
        const auto script = std::string(R"(
          assert(CP.PTS ~= nil)
          CP.log(0,'fixture','loaded')
          local root=CP.DPLayer.create()
          root.frame=CP.DPRect(0,0,128,64)
          root.backgroundColor=CP.DPColor(0,0.3,0,1)
          local video=CP.DPVideoLayer.create('fixture-input')
          video.frame=CP.DPRect(0,0,64,64)
          video:subscribe()
          root:addSublayer(video)
          CP.setRootLayer(')") +
                            spec.identifier + R"(',root)
          CP.DPLuaTask.create():run(function()
            CP.log(0,'fixture','started')
            while true do
              assert(CP.PTS ~= nil)
              local message=CP.receiveMessage()
              if message then
                if not CP.isForegrounded then CP.log(0,'fixture','echo-hidden') end
                assert(CP.sendMessage(message))
              end
              coroutine.yield()
            end
          end)
        )";
        runtime.evaluate(script, "original-surface-handoff-fixture");
        loaded = true;
      } else if (!started) {
        runtime.start_tasks(turn.pts);
        started = true;
      } else
        runtime.tick(turn.pts);
    }
    return turn;
  }
};

headunit::theme::Bytes message(std::int64_t value) {
  return headunit::encode_binary_plist(headunit::PlistValue{
      headunit::PlistValue::Dictionary{{"value", headunit::PlistValue{value}}}});
}
c::Configuration camera_config() {
  c::Configuration result;
  result.enabled = true;
  result.manual_activation = true;
  result.source = c::SourceKind::external;
  result.stream_identifier = "original-camera-fixture";
  result.width = 64;
  result.height = 64;
  result.maximum_age_ms = 1000;
  result.canvas_width = 128;
  result.canvas_height = 64;
  result.destination = {0, 0, 128, 64};
  return result;
}
}  // namespace

int main(int argc, char** argv) try {
  check(argc == 3,
        "usage: theme-surface-handoff-probe MAIN_SOCKET CLUSTER_SOCKET (two isolated headless "
        "compositors)");
  const std::filesystem::path main_socket(argv[1]), cluster_socket(argv[2]);
  check(main_socket.is_absolute() && cluster_socket.is_absolute() && main_socket != cluster_socket,
        "surface handoff needs two distinct absolute isolated Wayland sockets");
  const auto origin = ns();
  PrivateFiles files;
  files.set(true);
  c::ActivationControl control(files.request.string());
  c::CameraSurfaceLease main_lease(files.lease.string()), native_lease(files.lease.string()),
      observer(files.lease.string());
  check(native_lease.acquire_exclusive(), "initial native surface lease unavailable");
  const auto config = camera_config();
  c::Queue camera_queue(config);
  std::unique_ptr<c::NativeDisplay> native;
  const auto start_native = [&] {
    check(native_lease.acquire_exclusive() && !observer.acquire_shared(),
          "native surface did not exclude main ownership");
    c::DisplayOptions options;
    options.window.display_name = main_socket.string();
    options.window.output_name = "headless";
    options.window.app_id = "org.headunit.surface-probe.camera";
    options.gpu_completion_timeout_ns = 2000000000;
    native = std::make_unique<c::NativeDisplay>(config, camera_queue, std::move(options));
  };
  const auto stop_native = [&] {
    native.reset();
    native_lease.release();
  };
  start_native();
  std::uint64_t releases = 0;
  std::shared_ptr<Input> main_input;
  bool release_violation = false, expect_cancellation = true;
  g::GpuDisplayOptions::SurfaceLease lease{
      [&] { return !control.poll() && main_lease.acquire_shared(); },
      [&]() noexcept {
        try {
          if (expect_cancellation && (!main_input || !main_input->outstanding.empty() ||
                                      main_input->stream()->composition_token()))
            release_violation = true;
          main_lease.release();
          ++releases;
          if (!native_lease.acquire_exclusive()) release_violation = true;
          native_lease.release();
        } catch (...) {
          release_violation = true;
          main_lease.release();
          native_lease.release();
        }
      }};
  Panel main("main", main_socket.string(), origin, std::move(lease));
  main_input = main.input;
  main.input->before_suspend = [&] {
    if (main_lease.holds())
      check(!native_lease.acquire_exclusive(),
            "native lease became available before input suspension");
  };
  Panel cluster("cluster", cluster_socket.string(), origin);
  const auto initial = main.step();
  check(initial.defer_startup && !initial.allow_tasks && !initial.pts && !main.loaded &&
            main.display->stats().submissions == 0,
        "denied initial main startup invented presentation or evaluated the entry point");
  const auto cold_message = message(1);
  check(main.runtime.receive(cold_message), "cold main control queue rejected fixture message");
  until(
      [&] {
        main.step();
        cluster.step();
        native->turn();
        return cluster.display->stats().scene_presentations >= 3 &&
               native->statistics().missing_presentations >= 1;
      },
      "cluster or native missing-source UI did not present while main startup was deferred");
  check(!main.runtime.take_outgoing() && main.loads == 0 && main.starts == 0 &&
            main.input->stage_calls == 0,
        "cold hidden main acknowledged input or staged video");

  stop_native();
  files.set(false);
  const auto fresh_bootstrap = main.step();
  check(!fresh_bootstrap.defer_startup && !fresh_bootstrap.allow_tasks && !fresh_bootstrap.pts,
        "restored initial main confused submission with actual presentation");
  until(
      [&] {
        main.step();
        cluster.step();
        return main.started && main.display->stats().scene_presentations >= 2;
      },
      "main initial fresh startup failed");
  check(main.loads == 1 && main.starts == 1 &&
            main.runtime.take_outgoing() == std::optional<headunit::theme::Bytes>(cold_message),
        "deferred main failed to start once and deliver its queued control");
  check(!native_lease.acquire_exclusive(), "native lease overlapped a mapped main surface");

  main.input->request();
  const auto staged_before = main.input->staged;
  until(
      [&] {
        main.step();
        cluster.step();
        return main.input->staged > staged_before;
      },
      "main source did not prepare before handoff");
  check(!main.input->outstanding.empty() && main.display->stats().pending,
        "source handoff had no real in-flight scene");
  const auto cached = main.last_pts;
  const auto accepted = main.display->stats().presentations;
  const auto completed = main.input->completed;
  files.set(true);
  auto hidden = main.step();
  check(hidden.allow_tasks && hidden.pts == cached && !hidden.defer_startup &&
            main.display->stats().surface_withdrawn && !main.display->stats().pending &&
            releases == 1 && !release_violation && main.input->retired >= 1 &&
            main.input->outstanding.empty(),
        "started main failed to withdraw its scene and preserve the last actual PTS");
  start_native();
  const auto hidden_stage_calls = main.input->stage_calls, mapped_pumps = main.input->mapped_pumps;
  const auto hidden_message = message(2);
  main.input->request();
  check(main.runtime.receive(hidden_message), "hidden main control rejected");
  hidden = main.step();
  cluster.step();
  native->turn();
  check(hidden.pts == cached &&
            main.runtime.take_outgoing() == std::optional<headunit::theme::Bytes>(hidden_message) &&
            main.hidden_echoes == 1 && main.input->drained == 1 &&
            main.input->stage_calls == hidden_stage_calls &&
            main.input->mapped_pumps == mapped_pumps && main.input->completed == completed,
        "hidden main stopped controls, mapped a source, or completed an abandoned receipt");

  // Reopen the main window, but withdraw before polling its first actual
  // receipt. The previous accepted PTS remains the control-task timestamp.
  stop_native();
  files.set(false);
  const auto unqualified = main.step();
  check(!unqualified.allow_tasks && !unqualified.pts &&
            main.display->stats().presentations == accepted,
        "unqualified short restoration advanced PTS");
  files.set(true);
  hidden = main.step();
  check(hidden.allow_tasks && hidden.pts == cached &&
            main.display->stats().presentations == accepted && releases == 2,
        "rapid rewithdrawal erased or advanced the last actual presentation time");
  start_native();

  // The short mapped restoration may pump at the synchronized media clock
  // before it qualifies a presentation. Freeze this hidden interval after its
  // own withdrawal; source staging/completion and accepted PTS remain unchanged.
  const auto hidden_mapped_pumps = main.input->mapped_pumps;
  const auto sampled_before = main.clock_samples,
             cluster_before = cluster.display->stats().scene_presentations;
  const auto hidden_until = Clock::now() + 31200ms;
  auto cluster_progress = Clock::now(), next_capture = Clock::now(), next_control = Clock::now();
  auto previous_cluster = cluster_before;
  std::uint64_t control_number = 3, controls = 0;
  while (Clock::now() < hidden_until) {
    bounded();
    const auto at = Clock::now();
    if (at >= next_capture) {
      auto image = pixels();
      const auto captured = ns();
      check(camera_queue.offer_image(image, captured, captured, true),
            "native synthetic capture rejected");
      next_capture = at + 100ms;
    }
    std::optional<headunit::theme::Bytes> sent;
    if (at >= next_control) {
      sent = message(control_number++);
      check(main.runtime.receive(*sent), "bounded hidden control rejected");
      main.input->request();
      next_control = at + 1s;
    }
    hidden = main.step();
    cluster.step();
    native->turn();
    const auto hidden_stats = main.display->stats();
    if (!(hidden.allow_tasks && hidden.pts == cached && !hidden_stats.pending &&
          hidden_stats.presentations == accepted && main.input->stage_calls == hidden_stage_calls &&
          main.input->mapped_pumps == hidden_mapped_pumps && main.input->completed == completed)) {
      // Emit one bounded, content-free row on failure; no per-frame logging.
      std::cerr << "hidden-main invariant monotonic_ns=" << ns()
                << " allow_tasks=" << hidden.allow_tasks
                << " pts_matches=" << (hidden.pts == cached) << " pending=" << hidden_stats.pending
                << " withdrawn=" << hidden_stats.surface_withdrawn
                << " presentations=" << hidden_stats.presentations << "/" << accepted
                << " stage_calls=" << main.input->stage_calls << "/" << hidden_stage_calls
                << " mapped_pumps=" << main.input->mapped_pumps << "/" << hidden_mapped_pumps
                << " completed=" << main.input->completed << "/" << completed
                << " clock_epoch_suspensions=" << hidden_stats.clock_epoch_suspensions
                << " clock_unavailable_suspensions=" << hidden_stats.clock_unavailable_suspensions
                << '\n';
      check(false, "hidden main advanced qualification or staged a source");
    }
    if (sent) {
      check(main.runtime.take_outgoing() == sent,
            "hidden main did not echo its control at the cached PTS");
      ++controls;
    }
    const auto scenes = cluster.display->stats().scene_presentations;
    if (scenes > previous_cluster) {
      previous_cluster = scenes;
      cluster_progress = Clock::now();
    }
    check(Clock::now() - cluster_progress < 3s,
          "independent cluster stopped receiving actual presentations during camera ownership");
    std::this_thread::sleep_for(10ms);
  }
  check(main.clock_samples > sampled_before + 100 &&
            cluster.display->stats().scene_presentations > cluster_before + 20 &&
            native->statistics().fresh_camera_presentations >= 3 &&
            main.hidden_echoes == controls + 1 && main.input->drained == controls + 1,
        "long hidden session lacked clock sampling, control draining, cluster receipts or genuine "
        "native camera receipts");

  stop_native();
  files.set(false);
  const auto resumed_scene = main.display->stats().scene_presentations;
  until(
      [&] {
        main.step();
        cluster.step();
        return main.display->stats().scene_presentations > resumed_scene && main.last_pts &&
               *main.last_pts > *cached;
      },
      "main failed to restore actual presentation after 31 seconds of sampled hidden clock");
  main.input->request();
  const auto shown = main.input->shown;
  until(
      [&] {
        main.step();
        cluster.step();
        return main.input->shown > shown;
      },
      "restored source did not retire on an actual receipt");
  check(main.loads == 1 && main.starts == 1 && cluster.loads == 1 && cluster.starts == 1 &&
            !main.display->stats().faulted && !cluster.display->stats().faulted &&
            main.input->outstanding.empty() && main.display->stats().surface_withdrawals == 2 &&
            main.display->stats().surface_restorations == 3,
        "surface restore reloaded tasks, leaked a preparation or faulted a panel");
  // Exercise final GPU-before-lease teardown while the callback's sources and
  // both private lease owners remain alive.
  expect_cancellation = false;
  main.display.reset();
  check(releases == 3 && !release_violation && native_lease.acquire_exclusive(),
        "final main teardown retained its shared lease");
  native_lease.release();
  std::cout << "GPU surface handoff: deferred startup, independent cluster receipts, ordered "
               "source retirement, hidden controls, rapid rewithdrawal, "
            << "31s sampled clock restoration and native camera receipts passed (isolated headless "
               "outputs; no physical focus/scanout claim)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
