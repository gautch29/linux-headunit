// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Original synthetic clock/scene, real production GpuDisplay and Wayland
// feedback. May run ONLY inside native_home_wayland_test.py's private runtime.
// Does not use a phone, asset, live compositor, Bluetooth or physical output.
#include <headunit/theme/gpu_display.hpp>
#include <headunit/main_surface_control.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace g = headunit::theme::graphics;
namespace s = headunit::theme::script;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
template <class Options>
void set_reveal(Options& options, bool reveal) {
  if constexpr (requires { options.reveal_underlay_while_waiting = reveal; })
    options.reveal_underlay_while_waiting = reveal;
  else
    check(!reveal, "this exact target build has no transparent-bootstrap policy");
}
std::uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}
std::string environment(const char* key) {
  const auto* value = std::getenv(key);
  check(value, "missing private fixture environment");
  return value;
}
std::string quoted(std::string value) {
  if (value.size() > 512) value.resize(512);
  std::string result = "\"";
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') result += '\\';
    result += c >= 32 && c < 127 ? char(c) : '?';
  }
  return result + '"';
}
struct Capsule {
  std::filesystem::path path;
  int directory = -1;
  Capsule() {
    path = environment("XDG_RUNTIME_DIR");
    check(::getuid() != 0 && ::getuid() == ::geteuid(), "GPU fixture requires a nonroot user");
    check(path.is_absolute() && std::filesystem::canonical(path) == path &&
              path.filename() == "runtime" &&
              path.parent_path().filename().string().starts_with("hu-home-wl-"),
          "not an isolated fixture runtime");
    struct stat info{};
    check(::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::getuid() &&
              (info.st_mode & 07777) == 0700,
          "unsafe GPU fixture runtime");
    directory = ::open(path.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    check(directory >= 0, "cannot pin GPU fixture runtime");
    const auto token = environment("HEADUNIT_HOME_FIXTURE_TOKEN");
    check(token.size() == 32 && token.find_first_not_of("0123456789abcdef") == token.npos,
          "invalid fixture token");
    const int fd =
        ::openat(directory, ".fixture-token", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    check(fd >= 0, "no fixture token");
    char actual[33]{};
    const bool valid = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                       info.st_uid == ::getuid() && info.st_nlink == 1 &&
                       (info.st_mode & 07777) == 0600 && info.st_size == 32 &&
                       ::read(fd, actual, 33) == 32;
    ::close(fd);
    check(valid && token == actual, "unsafe or mismatched fixture token");
    check(environment("WLR_BACKENDS") == "headless" && environment("WLR_RENDERER") == "gles2",
          "actual GPU fixture requires its own headless GLES2 compositor");
    const auto display = environment("WAYLAND_DISPLAY");
    check(display.starts_with("wayland-") && display.find('/') == display.npos &&
              ::fstatat(directory, display.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
              S_ISSOCK(info.st_mode) && info.st_uid == ::getuid(),
          "not an owned fixture Wayland endpoint");
  }
  ~Capsule() {
    if (directory >= 0) ::close(directory);
  }
  void publish(const std::string& value) {
    check(value.size() <= 2048, "oversized GPU fixture status");
    const int fd = ::openat(directory, "gpu-status.tmp",
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    check(fd >= 0, "cannot create private GPU status");
    const auto written = ::write(fd, value.data(), value.size());
    const int closed = ::close(fd);
    if (written != std::ptrdiff_t(value.size()) || closed != 0) {
      ::unlinkat(directory, "gpu-status.tmp", 0);
      throw std::runtime_error("cannot write GPU fixture status");
    }
    struct stat info{};
    if (::fstatat(directory, "gpu-status.json", &info, AT_SYMLINK_NOFOLLOW) == 0)
      check(S_ISREG(info.st_mode) && info.st_uid == ::getuid() && info.st_nlink == 1 &&
                (info.st_mode & 07777) == 0600 && info.st_size <= 2048,
            "unsafe previous GPU fixture status");
    check(::renameat(directory, "gpu-status.tmp", directory, "gpu-status.json") == 0,
          "cannot replace GPU fixture status");
  }
  bool stopped() {
    const int fd = ::openat(directory, "gpu-stop", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat info{};
    char value[6]{};
    const bool valid = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                       info.st_uid == ::getuid() && info.st_nlink == 1 &&
                       (info.st_mode & 07777) == 0600 && info.st_size == 5 &&
                       ::read(fd, value, 6) == 5;
    ::close(fd);
    check(valid && std::string_view(value, 5) == "stop\n", "invalid GPU fixture stop");
    return true;
  }
};
}  // namespace

int main(int argc, char** argv) {
  ::umask(0077);
  std::unique_ptr<Capsule> capsule;
  std::unique_ptr<s::Runtime> runtime;
  std::shared_ptr<headunit::MainSurfaceControl> handoff;
  std::unique_ptr<g::GpuDisplay> display;
  double last_pts = 0;
  std::uint64_t return_turn_started_ns = 0, return_turn_ns = 0;
  std::uint64_t restored_observed_ns = 0, return_scene_observed_ns = 0;
  std::uint64_t return_scene_floor = 0;
  std::string phase = "constructing";
  const auto origin = now_ns();
  const auto status = [&](const std::string& error = {}) {
    const auto stats = display ? display->stats() : g::GpuDisplayStats{};
    std::ostringstream out;
    out << "{\"phase\":" << quoted(phase) << ",\"elapsedNs\":" << now_ns() - origin
        << ",\"submissions\":" << stats.submissions << ",\"presentations\":" << stats.presentations
        << ",\"scenePresentations\":" << stats.scene_presentations
        << ",\"sceneCompositions\":" << stats.scene_compositions
        << ",\"surfaceWithdrawn\":" << (stats.surface_withdrawn ? "true" : "false")
        << ",\"surfaceWithdrawals\":" << stats.surface_withdrawals
        << ",\"surfaceRestorations\":" << stats.surface_restorations
        << ",\"returnTurnStartedNs\":" << return_turn_started_ns
        << ",\"returnTurnNs\":" << return_turn_ns
        << ",\"restoredObservedNs\":" << restored_observed_ns
        << ",\"returnSceneObservedNs\":" << return_scene_observed_ns << ",\"lastPts\":" << last_pts
        << ",\"pending\":" << (stats.pending ? "true" : "false") << ",\"error\":" << quoted(error)
        << "}\n";
    capsule->publish(out.str());
    std::cerr << out.str();
  };
  try {
    check(argc == 5 || (argc == 6 && std::string_view(argv[5]) == "handoff"),
          "usage: native-home-gpu-probe APP_ID REVEAL(0/1) QUEUE(1/2) DEFER(0/1) [handoff]");
    const std::string id = argv[1], reveal = argv[2], queue = argv[3], defer = argv[4];
    check(id == "org.headunit.ultra.main" && (reveal == "0" || reveal == "1") &&
              (queue == "1" || queue == "2") && (defer == "0" || defer == "1"),
          "invalid exact GPU fixture options");
    capsule = std::make_unique<Capsule>();
    status();
    const s::Display spec{"original-home-gpu-fixture", 1920, 720};
    runtime = std::make_unique<s::Runtime>(
        [](std::string_view) -> std::string {
          throw std::runtime_error("fixture has no resources");
        },
        spec);
    auto clock = [&]() -> std::optional<headunit::AirplayClockSample> {
      const auto at = now_ns(), elapsed = at - origin;
      const auto ntp = ((std::uint64_t{2208989700} + elapsed / 1000000000) << 32) +
                       ((elapsed % 1000000000) << 32) / 1000000000;
      return headunit::AirplayClockSample{ntp, at, at, 1000};  // Explicit synthetic network clock.
    };
    g::GpuDisplayOptions options;
    options.window.title = "Original Home actual GPU fixture";
    options.window.app_id = id;
    options.window.output_name = "HEADLESS-1";
    options.window.display_name = environment("WAYLAND_DISPLAY");
    options.window.opaque_background = true;
    options.window.max_pending_presentations = unsigned(queue[0] - '0');
    options.window.defer_scene_fence = defer == "1";
    set_reveal(options, reveal == "1");
    if (argc == 6) {
      handoff = std::make_shared<headunit::MainSurfaceControl>(
          (capsule->path / "home-surface.request").string(),
          (capsule->path / "home-surface.lease").string());
      options.surface_lease =
          g::GpuDisplayOptions::SurfaceLease{[handoff] { return handoff->renderer_acquire(); },
                                             [handoff] {
                                               handoff->renderer_release();
                                             }};
    }
    display = std::make_unique<g::GpuDisplay>(*runtime, spec, clock, options);
    phase = "bootstrap";
    status();
    s::UiWorker::DisplayTurn turn;
    auto reported = now_ns(), previous = reported;
    std::uint64_t bootstrap_active_ns = 0;
    while (true) {
      turn = display->turn(s::UiWorker::DisplayPhase::bootstrap);
      const auto at = now_ns();
      if (!turn.defer_startup) bootstrap_active_ns += at - previous;
      previous = at;
      if (now_ns() - reported >= 100000000) {
        status();
        reported = now_ns();
      }
      if (turn.allow_tasks) break;
      check(bootstrap_active_ns < 5000000000, "real GPU bootstrap did not qualify a presentation");
      check(now_ns() - origin < 30000000000, "bounded GPU bootstrap fixture exceeded its run");
      std::this_thread::sleep_for(1ms);
    }
    check(turn.pts.has_value() && display->stats().presentations > 0, "bootstrap invented PTS");
    last_pts = *turn.pts;
    runtime->tick(turn.pts);
    runtime->evaluate(R"(
      local root = CP.DPLayer.create()
      root.frame = CP.DPRect(0,0,1920,720)
      root.backgroundColor = CP.DPColor(38/255,130/255,70/255,1)
      CP.setRootLayer(CP.enumerateOutputStreams()[1].identifier,root)
    )");
    runtime->start_tasks(turn.pts);
    phase = "rendering";
    status();
    while (!capsule->stopped()) {
      const auto before = display->stats();
      const auto turn_started = now_ns();
      turn = display->turn();
      const auto turn_ended = now_ns();
      const auto after = display->stats();
      if (before.surface_withdrawn && !after.surface_withdrawn) {
        return_turn_started_ns = turn_started;
        return_turn_ns = turn_ended - turn_started;
        restored_observed_ns = turn_ended;
        return_scene_observed_ns = 0;
        return_scene_floor = before.scene_presentations;
        status();
      }
      if (restored_observed_ns && !return_scene_observed_ns && !after.surface_withdrawn &&
          after.scene_visible && after.scene_presentations > return_scene_floor) {
        // Owner observation immediately after a genuinely qualified scene
        // receipt, never a submit/redraw timestamp or inherited hidden PTS.
        return_scene_observed_ns = turn_ended;
        status();
      }
      if (turn.pts) last_pts = *turn.pts;
      if (turn.allow_tasks) runtime->tick(turn.pts);
      if (now_ns() - reported >= 100000000) {
        status();
        reported = now_ns();
      }
      check(now_ns() - origin < 30000000000, "GPU fixture exceeded its bounded run");
      std::this_thread::sleep_for(2ms);
    }
    status();
    display.reset();
    phase = "stopped";
    status();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "failure during " << phase << ": " << error.what() << '\n';
    if (capsule) {
      try {
        phase = "fault";
        status(error.what());
      } catch (...) {
      }
    }
    return 1;
  }
}
