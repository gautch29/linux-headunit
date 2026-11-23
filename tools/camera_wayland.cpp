// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera.hpp>
#include <headunit/camera_control.hpp>
#include <headunit/camera_controller_io.hpp>
#include <headunit/camera_display.hpp>
#include <headunit/camera_local_controls.hpp>
#include <headunit/camera_manual_client.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <headunit/display_configuration.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
namespace c = headunit::camera;
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
constexpr auto usage =
    "usage: headunit-camera-wayland --camera-profile ABS_JSON --display-config ABS_CONF "
    "--request-file ABS_REQUEST --surface-lease-file ABS_LEASE [--status-file ABS_STATUS] "
    "[--manual-control-socket ABS_SOCKET] [--validate-only]";
struct Options {
  std::string profile, displays, request, lease, status, manual_socket;
  bool validate_only = false, help = false;
};
Options arguments(int argc, char** argv) {
  Options result;
  std::set<std::string_view> seen;
  for (int i = 1; i < argc;) {
    const std::string_view option(argv[i++]);
    check(seen.insert(option).second, "duplicate native camera option");
    if (option == "--help") {
      result.help = true;
      continue;
    }
    if (option == "--validate-only") {
      result.validate_only = true;
      continue;
    }
    check(i < argc, "native camera option is missing its value");
    const std::string value(argv[i++]);
    if (option == "--camera-profile")
      result.profile = value;
    else if (option == "--display-config")
      result.displays = value;
    else if (option == "--request-file")
      result.request = value;
    else if (option == "--surface-lease-file")
      result.lease = value;
    else if (option == "--status-file")
      result.status = value;
    else if (option == "--manual-control-socket")
      result.manual_socket = value;
    else
      throw std::invalid_argument("unknown native camera option");
  }
  if (result.help) {
    check(argc == 2, "--help cannot be combined with native camera options");
    return result;
  }
  check(!result.profile.empty() && !result.displays.empty() && !result.request.empty() &&
            !result.lease.empty(),
        "native camera requires explicit profile, display, request and surface lease files");
  return result;
}
void path(const std::string& text) {
  const std::filesystem::path value(text);
  check(!text.empty() && text.size() <= 4000 && text.find('\0') == text.npos &&
            value.is_absolute() && !value.filename().empty(),
        "native camera file path must be a bounded absolute file path");
  for (const auto& part : value)
    check(part != "." && part != "..", "native camera file path cannot traverse dot components");
}
bool same(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool configuration_file(const struct stat& s) {
  return S_ISREG(s.st_mode) && (s.st_uid == getuid() || s.st_uid == 0) && !(s.st_mode & 07022) &&
         s.st_size > 0 && s.st_size <= 8192;
}
struct File {
  int descriptor = -1;
  ~File() {
    if (descriptor >= 0) ::close(descriptor);
  }
};
std::string bounded_configuration(const std::string& name) {
  path(name);
  struct stat before{};
  check(!::lstat(name.c_str(), &before) && configuration_file(before),
        "native camera configuration must be a safe regular file of at most 8 KiB");
  File opened{::open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  struct stat initial{};
  check(opened.descriptor >= 0 && !::fstat(opened.descriptor, &initial) &&
            configuration_file(initial) && same(before, initial),
        "native camera configuration changed while opening");
  std::string text(8193, '\0');
  std::size_t count = 0;
  while (count < text.size()) {
    const auto got = ::read(opened.descriptor, text.data() + count, text.size() - count);
    if (got < 0 && errno == EINTR) continue;
    check(got >= 0, "native camera configuration read failed");
    if (!got) break;
    count += std::size_t(got);
  }
  struct stat after{};
  check(!::fstat(opened.descriptor, &after) && configuration_file(after) && same(initial, after) &&
            initial.st_size == after.st_size && count == std::size_t(initial.st_size),
        "native camera configuration changed while reading");
#ifdef __APPLE__
  check(initial.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
            initial.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec,
        "native camera configuration was modified while reading");
#else
  check(initial.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            initial.st_mtim.tv_nsec == after.st_mtim.tv_nsec,
        "native camera configuration was modified while reading");
#endif
  text.resize(count);
  return text;
}
void private_endpoint(const std::string& name, bool surface_lease = false,
                      std::size_t maximum_bytes = c::maximum_activation_request_bytes) {
  path(name);
  const auto parent = std::filesystem::path(name).parent_path();
  struct stat directory{};
  check(!::lstat(parent.c_str(), &directory) && S_ISDIR(directory.st_mode) &&
            directory.st_uid == getuid() && (directory.st_mode & 07777) == 0700,
        "native camera endpoint requires an owned real 0700 parent");
  struct stat endpoint{};
  if (!::lstat(name.c_str(), &endpoint))
    check(S_ISREG(endpoint.st_mode) && endpoint.st_uid == getuid() &&
              (endpoint.st_mode & 07777) == 0600 && endpoint.st_nlink == 1 &&
              endpoint.st_size >= 0 && std::uint64_t(endpoint.st_size) <= maximum_bytes &&
              (!surface_lease || endpoint.st_size == 0),
          "native camera endpoint must be an owned bounded single-link 0600 regular file (surface "
          "lease: empty)");
  else
    check(errno == ENOENT, "native camera endpoint cannot be inspected");
}
void distinct_endpoints(const std::string& request, const std::string& lease) {
  const std::filesystem::path a(request), b(lease);
  struct stat pa{}, pb{}, ia{}, ib{};
  check(!::lstat(a.parent_path().c_str(), &pa) && !::lstat(b.parent_path().c_str(), &pb),
        "native camera endpoint parent cannot be inspected");
  check(!same(pa, pb) || a.filename() != b.filename(),
        "native camera request and lease must use distinct files");
  if (!::lstat(request.c_str(), &ia) && !::lstat(lease.c_str(), &ib))
    check(!same(ia, ib), "native camera endpoints must not alias the same inode");
}
void private_manual_socket(const std::string& name) {
  path(name);
  check(name.size() <= 103 &&
            std::all_of(name.begin(), name.end(),
                        [](unsigned char c) { return c >= 32 && c <= 126; }) &&
            std::filesystem::path(name).lexically_normal().string() == name,
        "native camera manual socket requires a canonical bounded ASCII path");
  struct stat parent{}, endpoint{};
  const auto parent_path = std::filesystem::path(name).parent_path();
  std::error_code error;
  check(std::filesystem::canonical(parent_path, error) == parent_path && !error &&
            !::lstat(parent_path.c_str(), &parent) && S_ISDIR(parent.st_mode) &&
            parent.st_uid == getuid() && (parent.st_mode & 07777) == 0700,
        "native camera manual socket requires an owned real 0700 parent");
  if (!::lstat(name.c_str(), &endpoint))
    check(S_ISSOCK(endpoint.st_mode) && endpoint.st_uid == getuid() &&
              (endpoint.st_mode & 07777) == 0600 && endpoint.st_nlink == 1,
          "native camera manual endpoint must be an owned 0600 socket");
  else
    check(errno == ENOENT, "native camera manual socket cannot be inspected");
}
std::uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void event(std::string_view name) {
  std::cout << "{\"event\":\"" << name << "\"}\n";
  std::cout.flush();
}
struct Heartbeat {
  std::unique_ptr<c::StatusPublisher> publisher;
  std::uint64_t instance = now(), sequence = 0, previous = 0;
  c::ControlPhase phase = c::ControlPhase::stopped;
  explicit Heartbeat(const std::string& path) {
    if (!path.empty()) publisher = std::make_unique<c::StatusPublisher>(path);
  }
  void publish(c::ControlPhase next) {
    if (!publisher) return;
    const auto date = now();
    // The controller permits a 30ms minimum status age. A ten-millisecond
    // heartbeat leaves margin; state changes bypass this cadence immediately.
    if (next == phase && date - previous < 10'000'000) return;
    publisher->publish({instance, ++sequence, date, next});
    previous = date;
    phase = next;
  }
};
struct Active {
  c::Queue frames;
  std::unique_ptr<c::Provider> provider;
  std::unique_ptr<c::NativeDisplay> display;
  std::uint64_t touches = 0;
  bool reported_source_fault = false;
  c::ManualCloseClient& manual;
  c::ManualCloseGesture close_gesture;
  std::uint64_t previous_close;
  Active(const c::Configuration& config, const headunit::DisplayConfiguration& main,
         c::ManualCloseClient& controls)
      : frames(config),
        manual(controls),
        close_gesture(config.canvas_width, config.canvas_height),
        previous_close(manual.status().request_sequence) {
    c::DisplayOptions options;
    options.window.title = "Headunit local camera";
    options.window.app_id = "org.headunit.camera";
    options.window.display_name = main.wayland_display;
    options.window.output_name = main.output_name;
    if (manual.configured())
      options.manual_close_state = [this] {
        const auto result = manual.status();
        return result.request_sequence > previous_close ||
                       result.state == c::ManualCloseState::pending
                   ? result.state
                   : c::ManualCloseState::idle;
      };
    options.window.accept_touch = true;
    display = std::make_unique<c::NativeDisplay>(config, frames, std::move(options));
    provider = c::make_provider(config, frames);
  }
  void turn() {
    if (provider) provider->poll(now(), frames);
    if (provider && !reported_source_fault) {
      if (const auto failure = provider->failure()) {
        std::cerr << "native camera capture failed: " << std::string_view(*failure).substr(0, 512)
                  << '\n';
        event("camera_source_fault");
        reported_source_fault = true;
      }
    }
    check(!reported_source_fault, "native camera capture source failed");
    display->turn();
    for (const auto& touch : display->take_touches()) {
      ++touches;  // Local camera owns all input. No phone HID route exists here.
      if (close_gesture.consume(
              touch, manual.configured() && manual.status().state != c::ManualCloseState::pending))
        (void)manual.request_close();
    }
  }
};
struct WithdrawOnExit {
  std::unique_ptr<Active>& active;
  c::CameraSurfaceLease& lease;
  ~WithdrawOnExit() noexcept {
    // Also runs for heartbeat/file failures outside the inner render catch.
    // Release presentation/EX ownership before a provider destructor can join
    // a slow driver. Retiring providers have no mapped surface or EX lease.
    try {
      if (active) {
        active->display.reset();
        active->frames.discard();
        if (active->provider) active->provider->request_stop();
      }
    } catch (...) { /* EX is still returned before provider teardown below. */
    }
    lease.release();
  }
};
void withdraw(std::unique_ptr<Active>& active, std::unique_ptr<Active>& retiring,
              c::CameraSurfaceLease& lease) {
  if (active) {
    const auto stats = active->display->statistics();
    const auto touches = active->touches;
    // Destroy mapped GPU state before returning main ownership. A slow capture
    // stop stays in one bounded retiring slot, never in this presentation turn.
    active->display.reset();
    active->frames.discard();
    if (active->provider) active->provider->request_stop();
    check(!retiring, "native camera already has a retiring capture owner");
    retiring = std::move(active);
    std::cout << "{\"event\":\"camera_closed\",\"presentations\":" << stats.presentations
              << ",\"freshCameraPresentations\":" << stats.fresh_camera_presentations
              << ",\"missingPresentations\":" << stats.missing_presentations
              << ",\"stalePresentations\":" << stats.stale_presentations
              << ",\"maximumPending\":" << stats.maximum_pending
              << ",\"consumedTouches\":" << touches << "}\n";
    std::cout.flush();
  }
  lease.release();  // Always safe, including an invalidated/replaced lease generation.
}
}  // namespace
int main(int argc, char** argv) try {
  const auto options = arguments(argc, argv);
  if (options.help) {
    std::cout << usage << '\n';
    return 0;
  }
  std::set<std::filesystem::path> distinct;
  std::vector<std::string> names = {options.profile, options.displays, options.request,
                                    options.lease};
  if (!options.status.empty()) {
    names.push_back(options.status);
    names.push_back(options.status + ".owner");
  }
  if (!options.manual_socket.empty()) {
    names.push_back(options.manual_socket);
    names.push_back(options.manual_socket + ".lock");
  }
  for (const auto& name : names) {
    path(name);
    distinct.insert(std::filesystem::path(name).lexically_normal());
  }
  check(distinct.size() == names.size(),
        "native camera configuration and endpoint paths must be distinct");
  const auto camera = c::decode_configuration(bounded_configuration(options.profile));
  const auto displays =
      headunit::parse_display_configuration(bounded_configuration(options.displays));
  const auto& main = displays[0];
  check(camera.panel == c::Panel::main, "native camera appliance supports only the main display");
  check(!main.wayland_display.empty() && !main.output_name.empty(),
        "native camera requires explicit named main Wayland display and output");
  private_endpoint(options.request);
  private_endpoint(options.lease, true);
  distinct_endpoints(options.request, options.lease);
  if (!options.status.empty()) {
    private_endpoint(options.status, false, 128);
    private_endpoint(options.status + ".owner", true);
    for (const auto& endpoint : {options.request, options.lease, options.status + ".owner"})
      distinct_endpoints(options.status, endpoint);
    for (const auto& endpoint : {options.request, options.lease})
      distinct_endpoints(options.status + ".owner", endpoint);
  }
  if (!options.manual_socket.empty()) {
    (void)c::manual_close_frame(camera.canvas_width, camera.canvas_height);
    private_manual_socket(options.manual_socket);
    for (const auto& endpoint : names)
      if (endpoint != options.manual_socket) distinct_endpoints(options.manual_socket, endpoint);
  }
  c::ActivationControl request(options.request);
  if (options.validate_only) {
    std::cout << "{\"configurationValid\":true,\"enabled\":"
              << (camera.enabled && camera.manual_activation ? "true" : "false")
              << ",\"wayland\":\"" << main.wayland_display << "\",\"output\":\"" << main.output_name
              << "\",\"width\":" << main.size.width << ",\"height\":" << main.size.height << "}\n";
    return 0;
  }
  c::CameraSurfaceLease lease(options.lease);
  Heartbeat heartbeat(options.status);
  c::ManualCloseClient manual(options.manual_socket.empty() ? std::nullopt
                                                            : std::optional(options.manual_socket));
  struct sigaction action{};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  check(!::sigaction(SIGINT, &action, nullptr) && !::sigaction(SIGTERM, &action, nullptr) &&
            !::sigaction(SIGHUP, &action, nullptr),
        "native camera signal setup failed");
  std::unique_ptr<Active> active, retiring;
  bool seen_request = false, waiting = false, faulted = false;
  WithdrawOnExit cleanup{active, lease};
  event("camera_idle");
  heartbeat.publish(c::ControlPhase::idle);
  while (!stopping) {
    if (retiring && (!retiring->provider || retiring->provider->stopped())) retiring.reset();
    const bool wanted = request.poll();
    if (!wanted) {
      withdraw(active, retiring, lease);
      faulted = false;
      if (seen_request || waiting) event("camera_idle");
      seen_request = false;
      waiting = false;
    } else if (!camera.enabled || !camera.manual_activation) {
      if (!seen_request) event("camera_request_disabled");
      seen_request = true;
    } else if (!faulted) {
      seen_request = true;
      try {
        if (!active) {
          if (retiring) {
            if (!waiting) event("camera_waiting_for_capture");
            waiting = true;
          } else if (!lease.acquire_exclusive()) {
            if (!waiting) event("camera_waiting_for_main");
            waiting = true;
          } else if (request.poll() && !stopping) {
            active = std::make_unique<Active>(camera, main, manual);
            waiting = false;
            event("camera_opened");
          } else
            lease.release();
        }
        if (active) {
          // Cold GPU/provider setup can consume a managed request's entire
          // lease. Never submit its first frame under an expired request.
          if (!request.poll() || stopping) {
            withdraw(active, retiring, lease);
            waiting = false;
          } else {
            check(lease.holds(), "native camera lost its surface lease");
            active->turn();
          }
        }
      } catch (const std::exception& error) {
        withdraw(active, retiring, lease);
        faulted = true;
        waiting = false;
        std::cerr << "native camera request failed: "
                  << std::string_view(error.what()).substr(0, 512) << '\n';
        event(
            "camera_fault");  // Require a real off -> on edge before retrying this failed request.
      }
    }
    const auto phase = faulted || (wanted && (!camera.enabled || !camera.manual_activation))
                           ? c::ControlPhase::fault
                       : !wanted ? c::ControlPhase::idle
                       : active && active->display->statistics().presentations
                           ? c::ControlPhase::active
                           : c::ControlPhase::waiting;
    heartbeat.publish(phase);
    std::this_thread::sleep_for(active ? std::chrono::milliseconds(2)
                                       : std::chrono::milliseconds(10));
  }
  withdraw(active, retiring, lease);
  heartbeat.publish(c::ControlPhase::stopped);
  retiring.reset();
  event("camera_stopped");
  return 0;
} catch (const std::exception& error) {
  std::cerr << std::string_view(error.what()).substr(0, 512) << '\n' << usage << '\n';
  return 1;
}
