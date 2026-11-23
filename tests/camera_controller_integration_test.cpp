// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Actual fork/exec controller, private files, HCS1 status and HVO2 datagrams.
// The companion status publisher is a fixture, not a native GPU/camera receipt.
#include <headunit/camera_activation.hpp>
#include <headunit/camera_control.hpp>
#include <headunit/camera_controller_io.hpp>
#include <headunit/socket_vehicle.hpp>
#include <headunit/vehicle_observation_wire.hpp>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
namespace c = headunit::camera;
using namespace std::chrono_literals;
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
std::uint64_t now_ns() {
  return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           headunit::Clock::now().time_since_epoch())
                           .count());
}
headunit::Clock::time_point date(std::uint64_t ns) {
  return headunit::Clock::time_point{
      std::chrono::duration_cast<headunit::Clock::duration>(std::chrono::nanoseconds(ns))};
}
std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::string bytes(8193, '\0');
  input.read(bytes.data(), std::streamsize(bytes.size()));
  const auto count = input.gcount();
  check(count <= 8192, "fixture output exceeded bound");
  bytes.resize(std::size_t(count));
  return bytes;
}
void private_text(const fs::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), std::streamsize(text.size()));
  output.close();
  check(bool(output), "fixture file write failed");
  check(::chmod(path.c_str(), 0600) == 0, "fixture file permissions failed");
}
std::optional<std::uint64_t> deadline(const std::string& text) {
  if (text == "off\n" || text.empty()) return {};
  check(text.size() <= c::maximum_activation_request_bytes && text.starts_with("on ") &&
            text.ends_with('\n'),
        "controller emitted indefinite or malformed ON");
  const auto value = std::string_view(text).substr(3, text.size() - 4);
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  check(!value.empty() && value.front() >= '1' && value.front() <= '9' &&
            parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result,
        "controller emitted noncanonical deadline");
  return result;
}
struct Process {
  pid_t pid = -1;
  std::optional<int> exit_status;
  Process(const std::string& binary, const fs::path& profile, const fs::path& log,
          bool validation = false) {
    const auto profile_name = profile.string(), log_name = log.string();
    pid = ::fork();
    check(pid >= 0, "controller fixture fork failed");
    if (!pid) {
      const int output = ::open(log_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (output < 0 || ::dup2(output, STDOUT_FILENO) < 0 || ::dup2(output, STDERR_FILENO) < 0)
        ::_exit(126);
      ::close(output);
      if (validation)
        ::execl(binary.c_str(), binary.c_str(), "--profile", profile_name.c_str(),
                "--validate-only", static_cast<char*>(nullptr));
      else
        ::execl(binary.c_str(), binary.c_str(), "--profile", profile_name.c_str(), "--run-seconds",
                "5", static_cast<char*>(nullptr));
      ::_exit(127);
    }
  }
  ~Process() {
    if (!exit_status) {
      (void)::kill(pid, SIGKILL);
      reap();
    }
  }
  bool exited() {
    if (exit_status) return true;
    int status = 0;
    const auto result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      exit_status = status;
      return true;
    }
    if (result < 0 && errno != EINTR) throw std::runtime_error("controller fixture wait failed");
    return false;
  }
  void reap() noexcept {
    int status = 0;
    for (unsigned retry = 0; retry < 64; ++retry) {
      const auto result = ::waitpid(pid, &status, 0);
      if (result == pid) {
        exit_status = status;
        return;
      }
      if (result < 0 && errno == EINTR) continue;
      return;
    }
  }
  void killed() {
    check(!exited() && ::kill(pid, SIGKILL) == 0, "controller kill fixture failed");
    reap();
  }
  void stop_for_observation() {
    check(!exited() && ::kill(pid, SIGSTOP) == 0, "controller observation stop failed");
    int status = 0;
    pid_t result;
    do {
      result = ::waitpid(pid, &status, WUNTRACED);
    } while (result < 0 && errno == EINTR);
    if (result == pid && !WIFSTOPPED(status)) exit_status = status;
    check(result == pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP,
          "controller observation stop was not acknowledged");
  }
  bool success() const {
    return exit_status && WIFEXITED(*exit_status) && WEXITSTATUS(*exit_status) == 0;
  }
};
struct Fixture {
  fs::path folder, profile, manual_path, manual_owner, request_path, owner_path, status_path,
      socket_path, log_path;
  c::ControllerConfiguration configuration;
  std::unique_ptr<c::StatusPublisher> peer;
  std::unique_ptr<c::RequestPublisher> manual;
  std::unique_ptr<c::ActivationControl> output;
  std::unique_ptr<Process> process;
  std::uint64_t instance = 0, sequence = 0, last_peer_observation = 0;
  bool heartbeats = true;
  c::ControlPhase phase = c::ControlPhase::idle;
  std::function<void()> extra;
  unsigned log_generation = 0;
  fs::path journey_reports;
  explicit Fixture() {
    char path[] = "/tmp/hu-cc-XXXXXX";
    const auto* created = ::mkdtemp(path);
    check(created, "controller private runtime creation failed");
    folder = fs::canonical(created);
    check(::chmod(folder.c_str(), 0700) == 0, "private runtime permissions failed");
    profile = folder / "profile.json";
    manual_path = folder / "manual.request";
    manual_owner = folder / "manual.owner";
    request_path = folder / "camera.request";
    owner_path = folder / "controller.owner";
    status_path = folder / "native.status";
    socket_path = folder / "vehicle.sock";
    configuration.enabled = true;
    configuration.manual_enabled = true;
    configuration.manual_request_file = manual_path.string();
    configuration.vehicle_socket = socket_path.string();
    configuration.output_request_file = request_path.string();
    configuration.output_owner_file = owner_path.string();
    configuration.status_file = status_path.string();
    configuration.request_lease_ms = 90;
    configuration.status_maximum_age_ms = 120;
    configuration.startup_timeout_ms = 250;
    configuration.poll_ms = 10;
    configuration.reverse_maximum_age_ms = 250;
  }
  ~Fixture() {
    process.reset();
    output.reset();
    manual.reset();
    peer.reset();
    if (!journey_reports.empty()) {
      try {
        std::array<fs::path, 2> copies{journey_reports / "runtime-1",
                                       journey_reports / "runtime-2"};
        for (const auto& copy : copies) {
          check(fs::create_directory(copy), "journey report directory already exists");
          for (const auto& item : fs::directory_iterator(folder)) {
            if (!fs::is_regular_file(item.symlink_status())) continue;
            const auto destination = copy / item.path().filename();
            check(fs::copy_file(item.path(), destination), "journey report copy failed");
            check(read_file(item.path()) == read_file(destination),
                  "journey report copy changed original bytes");
          }
          check(fs::exists(copy / "journey.log") && fs::exists(copy / "child-1.log"),
                "journey reports are incomplete");
        }
      } catch (const std::exception& error) {
        std::cerr << "journey retention failed; originals preserved at " << folder << ": "
                  << error.what() << '\n';
        return;
      }
      // The lane retains and hashes both copies and the original runtime before
      // removing it. A destructor cannot certify the lane's complete receipts.
      return;
    }
    std::error_code error;
    fs::remove_all(folder, error);
  }
  void save() {
    c::validate_controller_configuration(configuration);
    const auto flag = [](bool value) {
      return value ? "true" : "false";
    };
    const auto quote = [](const std::string& value) {
      return '"' + value + '"';
    };
    private_text(
        profile,
        "{\"schemaVersion\":1,\"enabled\":" + std::string(flag(configuration.enabled)) +
            ",\"manual\":{\"enabled\":" + flag(configuration.manual_enabled) +
            ",\"requestFile\":" + quote(configuration.manual_request_file) +
            "},\"reverse\":{\"enabled\":" + flag(configuration.reverse_enabled) + ",\"basis\":\"" +
            (configuration.reverse_basis == c::ReverseBasis::engaged_gear ? "engaged-gear"
                                                                          : "selector") +
            "\",\"maximumAgeMs\":" + std::to_string(configuration.reverse_maximum_age_ms) +
            ",\"allowSimulation\":" + flag(configuration.allow_simulation) +
            "},\"vehicleSocket\":" + quote(configuration.vehicle_socket) +
            ",\"output\":{\"requestFile\":" + quote(configuration.output_request_file) +
            ",\"ownerFile\":" + quote(configuration.output_owner_file) +
            ",\"statusFile\":" + quote(configuration.status_file) +
            "},\"timing\":{\"requestLeaseMs\":" + std::to_string(configuration.request_lease_ms) +
            ",\"statusMaximumAgeMs\":" + std::to_string(configuration.status_maximum_age_ms) +
            ",\"startupTimeoutMs\":" + std::to_string(configuration.startup_timeout_ms) +
            ",\"pollMs\":" + std::to_string(configuration.poll_ms) + "}}\n");
    (void)c::decode_controller_configuration(read_file(profile));
  }
  void start_peer() {
    peer = std::make_unique<c::StatusPublisher>(status_path.string());
    instance = now_ns();
    sequence = 0;
    heartbeat();
  }
  void heartbeat() {
    if (!peer || !heartbeats) return;
    last_peer_observation = now_ns();
    peer->publish({instance, ++sequence, last_peer_observation, phase});
  }
  void manual_on(bool on) {
    if (!manual)
      manual = std::make_unique<c::RequestPublisher>(manual_path.string(), manual_owner.string());
    // Input renewals express a continuing independent manual request. They do
    // not refresh native status or the original vehicle acquisition timestamp.
    manual->publish(on, on ? now_ns() + 900'000'000ULL : 0);
  }
  void launch(const std::string& binary, bool validation = false) {
    save();
    log_path = folder / ("child-" + std::to_string(++log_generation) + ".log");
    process = std::make_unique<Process>(binary, profile, log_path, validation);
    output = std::make_unique<c::ActivationControl>(request_path.string());
  }
  bool on() const { return output->poll(); }
  bool off() const { return read_file(request_path) == "off\n" && !on(); }
  void step() {
    heartbeat();
    if (extra) extra();
  }
  void wait(const std::function<bool()>& done, std::chrono::milliseconds duration,
            const char* message, bool allow_exit = false) {
    const auto until = headunit::Clock::now() + duration;
    do {
      step();
      if (done()) return;
      if (!allow_exit && process && process->exited())
        throw std::runtime_error(std::string(message) + ": child exited: " + read_file(log_path));
      std::this_thread::sleep_for(10ms);
    } while (headunit::Clock::now() < until);
    throw std::runtime_error(std::string(message) + ": " + read_file(log_path));
  }
  void remains_off(std::chrono::milliseconds duration, const char* message) {
    const auto until = headunit::Clock::now() + duration;
    do {
      step();
      check(!on(), message);
      check(!process->exited(), "controller exited during fail-closed observation");
      std::this_thread::sleep_for(10ms);
    } while (headunit::Clock::now() < until);
  }
  std::uint64_t assert_timed_on(std::optional<std::uint64_t> maximum = {}) {
    const auto value = deadline(read_file(request_path));
    const auto observed = now_ns();
    check(value && *value > observed &&
              *value - observed <= std::uint64_t(configuration.request_lease_ms) * 1'000'000ULL &&
              on(),
          "managed request lacked a positive bounded lease");
    if (maximum) check(*value <= *maximum, "managed request exceeded original acquisition expiry");
    return *value;
  }
};
struct Datagram {
  int fd = -1;
  explicit Datagram() {
    fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    check(fd >= 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0, "HVO sender creation failed");
  }
  ~Datagram() {
    if (fd >= 0) ::close(fd);
  }
  void send(const fs::path& target, const headunit::VehicleObservationPacket& value) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = target.string();
    check(path.size() < sizeof(address.sun_path), "HVO fixture path exceeds socket bound");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto bytes = headunit::encode_vehicle_observation_datagram(value);
    check(::sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) == static_cast<ssize_t>(bytes.size()),
          "real HVO datagram send failed");
  }
};
headunit::VehicleObservationPacket reverse_packet(std::uint64_t acquired, std::uint64_t started,
                                                  std::uint64_t sequence,
                                                  headunit::VehicleObservationOrigin origin,
                                                  bool reverse = true) {
  headunit::VehicleObservationPacket packet;
  packet.version = headunit::VehicleObservationWireVersion::hvo2;
  packet.sequence = sequence;
  auto& sample = packet.sample;
  sample.sampled_at = headunit::Clock::now();
  sample.observation_timing.emplace();
  auto& timing = *sample.observation_timing;
  timing.generation = 1;
  timing.started_at = date(started);
  timing.transmission = {date(acquired), 1000ms, origin, headunit::VehicleAvailability::available};
  sample.transmission = headunit::TransmissionState{
      headunit::GearboxType::manual,
      {},
      headunit::EngagedGear{
          reverse ? headunit::EngagedGearKind::reverse : headunit::EngagedGearKind::neutral, 0}};
  sample.simulated = origin == headunit::VehicleObservationOrigin::simulation;
  return packet;
}

void validate_only(const std::string& binary) {
  Fixture f;
  f.launch(binary, true);
  f.wait([&] { return f.process->exited(); }, 1500ms, "validate-only failed to exit", true);
  check(f.process->success(), "valid profile rejected");
  for (const auto& path : {f.manual_path, f.manual_owner, f.request_path, f.owner_path,
                           f.status_path, fs::path(f.status_path.string() + ".owner"),
                           f.socket_path, fs::path(f.socket_path.string() + ".lock")})
    check(!fs::exists(path), "validate-only created a control endpoint");
}
void manual_peer_expiry_and_latch(const std::string& binary) {
  Fixture f;
  f.manual_on(true);
  f.launch(binary);
  f.remains_off(60ms, "missing native daemon status activated camera");
  f.phase = c::ControlPhase::active;
  f.start_peer();
  f.remains_off(40ms, "old active native owner bypassed initial withdrawal");
  f.phase = c::ControlPhase::waiting;
  f.remains_off(40ms, "old waiting native owner bypassed initial withdrawal");
  f.phase = c::ControlPhase::idle;
  f.wait([&] { return f.on(); }, 600ms, "fresh idle peer did not permit manual ON");
  const auto first_on = now_ns();
  (void)f.assert_timed_on();
  f.phase = c::ControlPhase::active;
  f.wait([&] { return f.last_peer_observation >= first_on && f.on(); }, 200ms,
         "fresh active acknowledgement was not accepted");
  f.heartbeats = false;
  f.extra = [&] {
    f.manual_on(true);
  };
  f.wait([&] { return f.off(); }, 400ms, "expired original peer heartbeat did not publish OFF");
  f.heartbeats = true;
  f.phase = c::ControlPhase::idle;
  f.remains_off(180ms, "fresh peer heartbeat cleared failure latch while manual remained ON");
  f.extra = {};
  check(::chmod(f.manual_path.c_str(), 0644) == 0, "unavailable manual fixture failed");
  f.remains_off(40ms, "unavailable manual input retained activation");
  check(::chmod(f.manual_path.c_str(), 0600) == 0, "manual input restoration failed");
  f.manual_on(true);
  f.remains_off(60ms, "unavailable manual input cleared fault latch without a genuine OFF");
  f.extra = {};
  f.manual_on(false);
  f.remains_off(40ms, "genuine manual OFF did not withdraw output");
  f.manual_on(true);
  f.wait([&] { return f.on(); }, 600ms, "manual OFF-to-ON failed to reset fault latch");
  (void)f.assert_timed_on();
  check(::kill(f.process->pid, SIGTERM) == 0, "controller graceful-stop signal failed");
  f.wait([&] { return f.process->exited(); }, 800ms, "controller graceful stop did not exit", true);
  check(f.process->success() && f.off() &&
            read_file(f.log_path).find("camera_controller_stopped") != std::string::npos,
        "graceful controller stop did not publish OFF and stopped state");
}
void killed_controller_lease_expiry(const std::string& binary) {
  Fixture f;
  f.start_peer();
  f.manual_on(true);
  f.launch(binary);
  f.wait([&] { return f.on(); }, 600ms, "controller kill test never activated");
  f.phase = c::ControlPhase::active;
  f.step();
  // Freeze the actual writer before reading its last lease. A live controller
  // may legally renew ON between a file read and delivery of SIGKILL.
  f.process->stop_for_observation();
  const auto expiration = f.assert_timed_on();
  const auto before = read_file(f.request_path);
  f.process->killed();
  check(f.process->exit_status && WIFSIGNALED(*f.process->exit_status) &&
            WTERMSIG(*f.process->exit_status) == SIGKILL,
        "controller did not receive SIGKILL");
  check(read_file(f.request_path) == before, "SIGKILL unexpectedly ran graceful OFF cleanup");
  f.wait([&] { return now_ns() > expiration + 10'000'000ULL && !f.on(); }, 300ms,
         "killed controller ON did not expire", true);
  check(read_file(f.request_path) == before, "expiry required another writer to change request");
}
void startup_timeout_anchor(const std::string& binary) {
  Fixture f;
  f.start_peer();
  f.manual_on(true);
  f.launch(binary);
  f.extra = [&] {
    f.manual_on(true);
  };
  f.wait([&] { return f.on(); }, 600ms, "idle peer did not start managed request");
  const auto first = now_ns();
  f.phase = c::ControlPhase::waiting;
  f.wait([&] { return f.off(); }, 450ms, "waiting heartbeats renewed startup timeout");
  check(now_ns() - first <= 450'000'000ULL, "startup deadline was not anchored to first ON");
  f.remains_off(150ms, "waiting heartbeat retried a latched failed startup");
  f.extra = {};
  f.manual_on(false);
  f.remains_off(40ms, "startup failure OFF reset failed");
  f.phase = c::ControlPhase::idle;
  f.manual_on(true);
  f.wait([&] { return f.on(); }, 600ms, "genuine OFF did not permit a new startup");
}
void reverse_acquisition_and_simulation(const std::string& binary) {
  Fixture f;
  f.configuration.manual_enabled = false;
  f.configuration.reverse_enabled = true;
  f.start_peer();
  f.launch(binary);
  f.wait([&] { return fs::exists(f.socket_path) && fs::is_socket(f.socket_path); }, 600ms,
         "dedicated HVO socket was not created");
  Datagram datagrams;
  const auto started = now_ns(), acquired = now_ns();
  std::uint64_t serial = 0;
  auto current = acquired;
  auto origin = headunit::VehicleObservationOrigin::measured;
  bool reverse = true;
  f.extra = [&] {
    datagrams.send(f.socket_path, reverse_packet(current, started, ++serial, origin, reverse));
  };
  f.wait([&] { return f.on(); }, 200ms, "measured reverse did not activate");
  const auto original_expiry = acquired + 250'000'000ULL;
  (void)f.assert_timed_on(original_expiry);
  f.phase = c::ControlPhase::active;
  f.wait([&] { return now_ns() >= original_expiry && f.off(); }, 500ms,
         "fresh publications renewed original reverse acquisition");
  f.remains_off(80ms, "repeated equal-acquisition HVO renewed stale reverse");
  current = now_ns();
  origin = headunit::VehicleObservationOrigin::simulation;
  f.remains_off(80ms, "simulation inferred a measured reverse signal");
  current = now_ns();
  origin = headunit::VehicleObservationOrigin::measured;
  reverse = false;
  f.remains_off(40ms, "neutral activated reverse camera");
  current = now_ns();
  reverse = true;
  f.phase = c::ControlPhase::idle;
  f.wait([&] { return f.on(); }, 200ms, "fresh measured reverse did not reactivate");
  (void)f.assert_timed_on(current + 250'000'000ULL);
}
void socket_conflict(const std::string& binary) {
  Fixture f;
  f.configuration.manual_enabled = false;
  f.configuration.reverse_enabled = true;
  headunit::SocketVehicle existing(f.socket_path.string());
  struct stat before{};
  check(::lstat(f.socket_path.c_str(), &before) == 0, "socket conflict fixture is absent");
  f.start_peer();
  f.launch(binary);
  f.wait([&] { return f.process->exited(); }, 1000ms, "dedicated socket conflict was not rejected",
         true);
  check(!f.process->success() && !f.on() &&
            read_file(f.log_path).find("vehicle input already has an owner") != std::string::npos,
        "controller did not reject the actual vehicle socket ownership conflict");
  struct stat after{};
  check(::lstat(f.socket_path.c_str(), &after) == 0 && before.st_dev == after.st_dev &&
            before.st_ino == after.st_ino,
        "controller conflict removed the existing vehicle endpoint");
}
void failed_peer_generations(const std::string& binary) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Fixture f;
    f.start_peer();
    f.manual_on(true);
    f.launch(binary);
    f.extra = [&] {
      f.manual_on(true);
    };
    f.wait([&] { return f.on(); }, 600ms, "peer-failure test never activated");
    f.phase = c::ControlPhase::active;
    f.step();
    (void)f.assert_timed_on();
    if (mode == 0) f.phase = c::ControlPhase::fault;
    if (mode == 1) f.phase = c::ControlPhase::stopped;
    if (mode == 2) {
      f.peer.reset();
      f.peer = std::make_unique<c::StatusPublisher>(f.status_path.string());
      f.instance = now_ns();
      f.sequence = 0;
      f.phase = c::ControlPhase::idle;
    }
    f.wait([&] { return f.off(); }, 300ms, "fault/stopped/new daemon instance did not publish OFF");
    f.phase = c::ControlPhase::idle;
    f.remains_off(150ms, "native recovery cleared latch without manual OFF");
  }
}

void reverse_owner_loss_requires_observed_nonreverse(const std::string& binary) {
  Fixture f;
  if (const auto* reports = std::getenv("HEADUNIT_CAMERA_JOURNEY_REPORT_DIR")) {
    struct stat info{};
    check(::lstat(reports, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::getuid() &&
              !(info.st_mode & 0077),
          "journey reports need an existing owned private directory");
    f.journey_reports = fs::canonical(reports);
    private_text(f.journey_reports / "original-runtime.txt", f.folder.string() + '\n');
  }
  const auto record = [&](std::string_view event) {
    std::ofstream log(f.folder / "journey.log", std::ios::app);
    log << now_ns() << ' ' << event << '\n';
    log.close();
    check(bool(log) && ::chmod((f.folder / "journey.log").c_str(), 0600) == 0,
          "journey trace could not be retained");
  };
  const auto observed = [&](std::string_view state, std::size_t offset = 0) {
    const auto log = read_file(f.log_path);
    check(offset <= log.size(), "journey controller log regressed");
    return log.find("\"state\":\"" + std::string(state) + "\"", offset) != log.npos;
  };
  record("journey-start-provider-and-status-are-fixtures");
  f.configuration.manual_enabled = false;
  f.configuration.reverse_enabled = true;
  f.start_peer();
  f.launch(binary);
  f.wait([&] { return fs::is_socket(f.socket_path); }, 600ms,
         "reverse journey did not create its dedicated HVO endpoint");
  Datagram datagrams;
  std::uint64_t generation = 1;
  auto started = now_ns();
  std::uint64_t serial = 0;
  std::uint64_t last_acquisition = 0;
  std::uint64_t last_publication = 0;
  unsigned packets = 0;
  bool reverse = true;
  bool unavailable = false;
  const auto send = [&] {
    const auto acquired = now_ns();
    check(acquired > last_acquisition, "journey acquisition did not advance");
    auto packet = reverse_packet(acquired, started, ++serial,
                                 headunit::VehicleObservationOrigin::measured, reverse);
    auto& timing = *packet.sample.observation_timing;
    timing.generation = generation;
    if (unavailable) {
      packet.sample.transmission.reset();
      timing.transmission.acquired_at.reset();
      timing.transmission.availability = headunit::VehicleAvailability::unavailable;
    }
    check(++packets <= 256, "journey packet trace exceeded its bound");
    const auto bytes = headunit::encode_vehicle_observation_datagram(packet);
    check(headunit::valid_vehicle_observation_packet(packet, headunit::Clock::now()),
          "journey generated an inadmissible HVO2 snapshot");
    private_text(f.folder / ("packet-" + std::to_string(packets) + ".hvo"), bytes);
    datagrams.send(f.socket_path, packet);
    last_acquisition = acquired;
    last_publication = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         packet.sample.sampled_at.time_since_epoch())
                                         .count());
  };
  // Measured-labelled HVO and HCS1 Active are explicit provider/status fixtures,
  // not authenticated car observations, GPU receipts or camera visibility.
  f.extra = send;
  f.wait([&] { return f.on() && observed("starting"); }, 200ms,
         "fresh reverse did not begin a bounded activation");
  (void)f.assert_timed_on(last_acquisition + 250'000'000ULL);
  private_text(f.folder / "first-on.request", read_file(f.request_path));
  f.phase = c::ControlPhase::active;
  f.wait([&] { return f.on() && observed("active"); }, 200ms,
         "reverse journey did not consume the newer same-owner Active acknowledgement");
  record("reverse-active-fixture-ack");
  const auto original_status = f.last_peer_observation;
  private_text(f.folder / "owner-before-loss.hcs", read_file(f.status_path));
  record("owner-last-observed-ns=" + std::to_string(original_status));
  f.heartbeats = false;
  f.wait(
      [&] {
        return f.off() &&
               read_file(f.log_path).find("\"state\":\"fault\",\"fault\":1") != std::string::npos;
      },
      400ms, "reverse owner loss did not latch owner_unavailable");
  check(now_ns() >= original_status + 120'000'000ULL,
        "reverse owner loss preceded the original heartbeat expiry");
  record("owner-unavailable-original-heartbeat-expired");
  const auto fault_offset = read_file(f.log_path).size();
  const auto original_reverse_expiry = last_acquisition + 250'000'000ULL;
  record("reverse-original-expiry-ns=" + std::to_string(original_reverse_expiry));
  f.extra = {};
  f.heartbeats = true;
  f.phase = c::ControlPhase::idle;
  f.wait([&] { return now_ns() >= original_reverse_expiry && f.off(); }, 500ms,
         "reverse journey did not reach original acquisition expiry");
  f.remains_off(40ms, "stale reverse cleared the failed activation latch");
  check(!observed("idle", fault_offset) && !observed("starting", fault_offset),
        "stale reverse was admitted as a genuine Off reset");
  record("original-reverse-expired-fault-still-latched");
  generation = 2;
  started = now_ns();
  check(started >= last_publication, "journey source restart preceded its publication");
  serial = 0;
  unavailable = true;
  f.extra = send;
  f.remains_off(40ms, "unavailable restarted source cleared the activation latch");
  check(!observed("idle", fault_offset) && !observed("starting", fault_offset),
        "unavailable source restart supplied reverse Off authority");
  unavailable = false;
  f.remains_off(150ms, "fresh reverse after source restart bypassed the failed request latch");
  check(!observed("idle", fault_offset) && !observed("starting", fault_offset),
        "fresh reverse recovery reset the failed request");
  record("new-source-missing-then-fresh-reverse-still-latched");
  reverse = false;
  f.wait([&] { return f.off() && observed("idle", fault_offset); }, 200ms,
         "genuinely acquired neutral did not reset the reverse latch");
  record("fresh-observed-neutral-reset");
  const auto reset_offset = read_file(f.log_path).size();
  reverse = true;
  f.wait([&] { return f.on() && observed("starting", reset_offset); }, 200ms,
         "fresh reverse after observed neutral did not start a new attempt");
  (void)f.assert_timed_on(last_acquisition + 250'000'000ULL);
  private_text(f.folder / "recovered-on.request", read_file(f.request_path));
  f.phase = c::ControlPhase::active;
  f.wait([&] { return f.on() && observed("active", reset_offset); }, 200ms,
         "new reverse attempt lacked a later same-owner acknowledgement");
  private_text(f.folder / "recovered-ack.hcs", read_file(f.status_path));
  record("fresh-reverse-new-attempt-fixture-ack");
  f.extra = {};
  check(::kill(f.process->pid, SIGTERM) == 0, "reverse journey graceful-stop signal failed");
  f.wait([&] { return f.process->exited(); }, 800ms, "reverse journey controller did not exit",
         true);
  check(f.process->success() && f.off() && !fs::exists(f.socket_path) &&
            read_file(f.log_path).find("camera_controller_stopped") != std::string::npos,
        "reverse journey shutdown did not publish Off, reap and remove its socket");
  c::RequestPublisher successor(f.request_path.string(), f.owner_path.string());
  check(f.off(), "retired reverse controller retained its output writer lock");
  record("normal-exit-zero-reaped-socket-absent-output-lock-reacquired");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 2 && fs::path(argv[1]).is_absolute() && ::access(argv[1], X_OK) == 0,
          "usage: camera-controller-integration-test ABS_CONTROLLER_BINARY");
    const std::string binary(argv[1]);
    unsigned passed = 0;
    const std::array<std::pair<const char*, void (*)(const std::string&)>, 8> tests{
        {{"validate-only leaves endpoints absent", validate_only},
         {"fresh peer gating, original heartbeat expiry and manual fault latch",
          manual_peer_expiry_and_latch},
         {"SIGKILL leaves a timed request that expires without another writer",
          killed_controller_lease_expiry},
         {"fresh waiting heartbeats cannot renew first-ON startup deadline",
          startup_timeout_anchor},
         {"real HVO2 preserves reverse acquisition expiry and origin",
          reverse_acquisition_and_simulation},
         {"dedicated SocketVehicle ownership conflict fails closed", socket_conflict},
         {"fresh fault, stopped and changed native instance fail closed", failed_peer_generations},
         {"reverse owner-loss latch survives stale/missing/restarted source until observed neutral",
          reverse_owner_loss_requires_observed_nonreverse}}};
    for (const auto& [name, test] : tests) {
      test(binary);
      ++passed;
      std::cout << "PASS " << name << '\n';
    }
    std::cout << passed
              << " actual controller CLI/IO/socket groups passed; companion status is a fixture, "
                 "no camera/GPU/hardware qualification.\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
