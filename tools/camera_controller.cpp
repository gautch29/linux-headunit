// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_activation_session.hpp>
#include <headunit/camera_control.hpp>
#include <headunit/socket_vehicle.hpp>
#include <cerrno>
#include <charconv>
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
constexpr auto usage =
    "usage: headunit-camera-controller --profile ABS_JSON [--validate-only] [--run-seconds "
    "1..3600]";
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
struct Options {
  std::string profile;
  bool validate = false, help = false;
  unsigned run_seconds = 0;
};
Options arguments(int argc, char** argv) {
  Options result;
  std::set<std::string_view> seen;
  for (int i = 1; i < argc;) {
    const std::string_view option(argv[i++]);
    check(seen.insert(option).second, "duplicate camera controller option");
    if (option == "--help") {
      result.help = true;
      continue;
    }
    if (option == "--validate-only") {
      result.validate = true;
      continue;
    }
    check(i < argc, "camera controller option is missing its value");
    const std::string_view value(argv[i++]);
    if (option == "--profile")
      result.profile = value;
    else if (option == "--run-seconds") {
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), result.run_seconds);
      check(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                result.run_seconds >= 1 && result.run_seconds <= 3600,
            "invalid camera controller run limit");
    } else
      throw std::invalid_argument("unknown camera controller option");
  }
  if (result.help) {
    check(argc == 2, "--help cannot be combined with controller options");
    return result;
  }
  check(!result.profile.empty(), "camera controller requires an explicit configuration");
  check(!result.validate || !result.run_seconds, "validation cannot have a run limit");
  return result;
}
bool safe(const struct stat& s) {
  return S_ISREG(s.st_mode) && (s.st_uid == getuid() || s.st_uid == 0) && !(s.st_mode & 07022) &&
         s.st_size > 0 && s.st_size <= 8192;
}
bool same(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool same_date(const struct stat& a, const struct stat& b) {
#ifdef __APPLE__
  return a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
         a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec;
#else
  return a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
#endif
}
struct File {
  int fd = -1;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
};
std::string configuration(const std::string& name) {
  const std::filesystem::path p(name);
  check(name.size() <= 4000 && name.find('\0') == name.npos && p.is_absolute() &&
            !p.filename().empty() && p.lexically_normal() == p,
        "camera controller profile requires a canonical absolute path");
  for (const auto& part : p)
    check(part != "." && part != "..", "camera controller profile cannot traverse dot components");
  struct stat before{};
  check(!::lstat(name.c_str(), &before) && safe(before), "unsafe camera controller profile");
  File file{::open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  struct stat initial{};
  check(file.fd >= 0 && !::fstat(file.fd, &initial) && safe(initial) && same(initial, before),
        "controller profile changed while opening");
  std::string text(8193, '\0');
  std::size_t count = 0;
  unsigned interrupted = 0;
  while (count < text.size()) {
    const auto got = ::read(file.fd, text.data() + count, text.size() - count);
    if (got < 0 && errno == EINTR && ++interrupted <= 3) continue;
    check(got >= 0, "controller profile read failed");
    if (!got) break;
    count += std::size_t(got);
  }
  struct stat after{};
  check(!::fstat(file.fd, &after) && safe(after) && same(initial, after) &&
            same_date(initial, after) && initial.st_size == after.st_size &&
            count == std::size_t(initial.st_size),
        "controller profile changed while reading");
  text.resize(count);
  return text;
}
void runtime_distinct(const c::ControllerConfiguration& config, const std::string& profile) {
  // The source's ownership lock and the status writer's lock are endpoints too.
  std::vector<std::string> paths = {profile,
                                    config.manual_request_file,
                                    config.vehicle_socket,
                                    config.output_request_file,
                                    config.output_owner_file,
                                    config.status_file,
                                    config.status_file + ".owner"};
  if (!config.vehicle_socket.empty()) paths.push_back(config.vehicle_socket + ".lock");
  for (std::size_t i = 0; i < paths.size(); ++i) {
    if (paths[i].empty()) continue;
    const std::filesystem::path a(paths[i]);
    struct stat pa{}, ia{};
    check(!::lstat(a.parent_path().c_str(), &pa), "controller endpoint parent is missing");
    const bool present = !::lstat(a.c_str(), &ia);
    for (std::size_t j = 0; j < i; ++j) {
      if (paths[j].empty()) continue;
      const std::filesystem::path b(paths[j]);
      struct stat pb{}, ib{};
      check(!::lstat(b.parent_path().c_str(), &pb), "controller endpoint parent is missing");
      check(!same(pa, pb) || a.filename() != b.filename(),
            "controller endpoints alias the same path");
      if (present && !::lstat(b.c_str(), &ib))
        check(!same(ia, ib), "controller endpoints alias the same inode");
    }
  }
}
const char* state(c::ControllerState s) {
  switch (s) {
    case c::ControllerState::idle:
      return "idle";
    case c::ControllerState::waiting_for_owner:
      return "waiting-for-owner";
    case c::ControllerState::starting:
      return "starting";
    case c::ControllerState::active:
      return "active";
    case c::ControllerState::fault:
      return "fault";
  }
  return "fault";
}
void event(c::ControllerState s, c::ControllerFault fault) {
  std::cout << "{\"event\":\"camera_controller_state\",\"state\":\"" << state(s)
            << "\",\"fault\":" << int(fault) << "}\n";
  std::cout.flush();
}
}  // namespace
int main(int argc, char** argv) try {
  const auto options = arguments(argc, argv);
  if (options.help) {
    std::cout << usage << '\n';
    return 0;
  }
  const auto config = c::decode_controller_configuration(configuration(options.profile));
  for (const auto& endpoint :
       {config.manual_request_file, config.vehicle_socket, config.output_request_file,
        config.output_owner_file, config.status_file, config.status_file + ".owner"})
    check(endpoint.empty() || endpoint != options.profile,
          "controller profile collides with an endpoint");
  check(config.vehicle_socket.empty() || config.vehicle_socket + ".lock" != options.profile,
        "controller profile collides with source ownership");
  if (options.validate) {
    std::cout << "{\"configurationValid\":true,\"enabled\":" << (config.enabled ? "true" : "false")
              << "}\n";
    return 0;
  }
  runtime_distinct(config, options.profile);
  c::RequestPublisher output(config.output_request_file, config.output_owner_file);
  c::StatusReader peer(config.status_file);
  c::ActivationSession session(config);
  std::unique_ptr<c::ActivationControl> manual;
  if (config.enabled && config.manual_enabled)
    manual = std::make_unique<c::ActivationControl>(config.manual_request_file);
  std::unique_ptr<headunit::SocketVehicle> vehicle;
  if (config.enabled && config.reverse_enabled)
    vehicle = std::make_unique<headunit::SocketVehicle>(config.vehicle_socket);
  struct sigaction action{};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  check(!::sigaction(SIGINT, &action, nullptr) && !::sigaction(SIGTERM, &action, nullptr) &&
            !::sigaction(SIGHUP, &action, nullptr),
        "controller signal setup failed");
  const auto began = headunit::Clock::now();
  c::ControllerState previous = c::ControllerState::idle;
  c::ControllerFault previous_fault = c::ControllerFault::none;
  bool published_on = false;
  event(previous, previous_fault);
  c::ActivationReason last_request_source = c::ActivationReason::no_request;
  while (!stopping) {
    const auto source_time = headunit::Clock::now();
    if (options.run_seconds && source_time - began >= std::chrono::seconds(options.run_seconds))
      break;
    const auto sample = vehicle ? vehicle->sample(source_time) : headunit::VehicleSample{};
    const auto manual_state = manual ? manual->read() : c::ActivationRequest::off;
    const auto native = peer.poll(config.status_maximum_age_ms);
    // Read liveness before sampling its comparison time. A genuine heartbeat
    // published during file I/O is not a future owner observation.
    const auto time = headunit::Clock::now();
    const auto reverse = c::decide_activation(config, false, sample, time);
    const auto desired = manual_state == c::ActivationRequest::on
                             ? c::decide_activation(config, true, sample, time)
                             : reverse;
    const bool confirmed_off = !config.enabled ||
                               (last_request_source == c::ActivationReason::manual &&
                                manual_state == c::ActivationRequest::off) ||
                               (last_request_source == c::ActivationReason::reverse &&
                                reverse.reason == c::ActivationReason::no_request);
    const auto command = session.turn(desired, native, time, confirmed_off);
    if (command.state == c::ControllerState::idle)
      last_request_source = c::ActivationReason::no_request;
    else if (command.requested ||
             (last_request_source == c::ActivationReason::no_request && desired.requested))
      last_request_source = desired.reason;
    if (command.requested || published_on) output.publish(command.requested, command.deadline_ns);
    published_on = command.requested;
    if (command.state != previous || command.fault != previous_fault) {
      event(command.state, command.fault);
      previous = command.state;
      previous_fault = command.fault;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(config.poll_ms));
  }
  output.publish(false);
  std::cout << "{\"event\":\"camera_controller_stopped\"}\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << std::string_view(error.what()).substr(0, 512) << '\n' << usage << '\n';
  return 1;
}
