// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Portable control-owner model: compile the actual native tool loop, with an
// explicitly FAKE display and capture provider. Queue, activation deadlines,
// private HCS1 status publication and actual cross-process surface locks remain
// production implementations. No GPU, compositor or hardware evidence here.
#include <headunit/camera_display.hpp>
#include <headunit/camera_controller_io.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <stdexcept>
#include <sys/wait.h>

#define main(...) headunit_native_camera_owner_entry(__VA_ARGS__)
#include "../tools/camera_wayland.cpp"
#undef main

namespace model {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void require(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
std::uint64_t clock_ns() {
  return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           headunit::Clock::now().time_since_epoch())
                           .count());
}
enum class Mode { cold_expiry, heartbeat_failure, provider_failure, presentation_gate };
struct Command {
  std::uint64_t deadline = 0;
  char operation = 0;
};
struct ChildState {
  Mode mode;
  int events = -1, commands = -1;
  bool provider_stopping = false;
  unsigned turns = 0;
};
ChildState* fixture = nullptr;
void write_all(int fd, const void* source, std::size_t length) {
  const auto* bytes = static_cast<const char*>(source);
  for (unsigned turn = 0; length && turn < 64; ++turn) {
    const auto wrote = ::write(fd, bytes, length);
    if (wrote < 0 && errno == EINTR) continue;
    require(wrote > 0, "model pipe write failed");
    bytes += wrote;
    length -= std::size_t(wrote);
  }
  require(!length, "model pipe write exceeded bounds");
}
void emit(char value) {
  write_all(fixture->events, &value, 1);
}
void emit_noexcept(char value) noexcept {
  try {
    emit(value);
  } catch (...) {
    ::_exit(92);
  }
}
void read_exact(int fd, void* destination, std::size_t length,
                std::chrono::milliseconds maximum = 2000ms) {
  auto* bytes = static_cast<char*>(destination);
  const auto until = headunit::Clock::now() + maximum;
  while (length) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(until - headunit::Clock::now())
            .count();
    require(left >= 0, "model pipe deadline expired");
    pollfd descriptor{fd, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, int(std::max<std::int64_t>(1, left)));
    if (ready < 0 && errno == EINTR) continue;
    require(ready > 0 && descriptor.revents & POLLIN,
            "model pipe did not provide a complete handshake");
    const auto got = ::read(fd, bytes, length);
    if (got < 0 && errno == EINTR) continue;
    require(got > 0, "model pipe closed during handshake");
    bytes += got;
    length -= std::size_t(got);
  }
}
Command command(char expected) {
  Command result;
  read_exact(fixture->commands, &result, sizeof(result));
  require(result.operation == expected, "unexpected model handshake command");
  return result;
}
struct FakeProvider final : headunit::camera::Provider {
  headunit::camera::Configuration configuration;
  bool failed = false;
  explicit FakeProvider(headunit::camera::Configuration selected)
      : configuration(std::move(selected)) {
    emit('P');
  }
  ~FakeProvider() override {
    emit_noexcept('B');
    // The production owner must have returned its mapped surface and EX lease
    // before this intentional provider-destructor delay is allowed to finish.
    if (fixture->mode == Mode::heartbeat_failure || fixture->mode == Mode::provider_failure) {
      try {
        (void)command('R');
      } catch (...) {
        ::_exit(93);
      }
    }
    emit_noexcept('E');
  }
  bool poll(std::uint64_t now, headunit::camera::Queue& queue) override {
    headunit::theme::graphics::Image image;
    image.width = configuration.width;
    image.height = configuration.height;
    image.pixels.assign(std::size_t(image.width) * image.height, 0xff004488);
    const bool offered = queue.offer_image(image, now, now, true);
    if (fixture->mode == Mode::provider_failure && !failed) {
      failed = true;
      emit('F');
    }
    return offered;
  }
  std::optional<std::string> failure() const override {
    return failed ? std::optional<std::string>("explicit fake terminal capture failure")
                  : std::nullopt;
  }
  void request_stop() noexcept override {
    if (!fixture->provider_stopping) {
      fixture->provider_stopping = true;
      emit_noexcept('S');
    }
  }
  bool stopped() const noexcept override { return fixture->provider_stopping; }
};
}  // namespace model

namespace headunit::camera {
struct NativeDisplay::Impl {
  Queue& queue;
  DisplayStatistics statistics;
  explicit Impl(Queue& selected) : queue(selected) {}
};
NativeDisplay::NativeDisplay(Configuration configuration, Queue& queue, DisplayOptions options)
    : impl_(std::make_unique<Impl>(queue)) {
  validate_configuration(configuration);
  model::require(options.window.display_name == "owner-model-main" &&
                     options.window.output_name == "owner-model-output",
                 "native tool lost its explicit main output");
  model::emit('C');
  if (model::fixture->mode == model::Mode::cold_expiry) {
    const auto command = model::command('X');
    const auto now = model::clock_ns();
    model::require(command.deadline > 0 && command.deadline <= now + 100'000'000,
                   "cold-start fixture requires one short genuine future deadline");
    const auto until = Clock::time_point{
        std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(command.deadline))};
    std::this_thread::sleep_until(until);
    model::require(model::clock_ns() >= command.deadline,
                   "cold fixture did not consume the original lease");
    model::emit('I');
  }
}
NativeDisplay::~NativeDisplay() {
  model::emit_noexcept('D');
}
void NativeDisplay::turn() {
  ++model::fixture->turns;
  model::emit('T');
  bool present = true;
  if (model::fixture->mode == model::Mode::heartbeat_failure) (void)model::command('H');
  if (model::fixture->mode == model::Mode::presentation_gate) {
    if (model::fixture->turns == 1)
      present = false;
    else if (model::fixture->turns == 2) {
      model::emit('W');
      (void)model::command('P');
    } else {
      model::emit('A');
      (void)model::command('Q');
    }
  }
  const auto frame = impl_->queue.stage(model::clock_ns());
  model::require(bool(frame), "fake display had no original queued frame");
  ++impl_->statistics.submissions;
  model::require(impl_->queue.complete(frame->sequence, present),
                 "fake queue receipt was not accepted");
  // Deliberate fixture receipts. They exercise the tool's status predicate,
  // and must never be described as real display/GPU presentation evidence.
  if (present) {
    ++impl_->statistics.presentations;
    ++impl_->statistics.fresh_camera_presentations;
  }
}
DisplayStatistics NativeDisplay::statistics() const {
  return impl_->statistics;
}
std::vector<headunit::TouchInput> NativeDisplay::take_touches() {
  return {};
}
std::string NativeDisplay::renderer() const {
  return "explicit portable fake";
}
std::unique_ptr<Provider> make_provider(const Configuration& configuration, Queue&) {
  return std::make_unique<model::FakeProvider>(configuration);
}
}  // namespace headunit::camera

namespace model {
namespace c = headunit::camera;
struct Pipe {
  int ends[2]{-1, -1};
  Pipe() { require(::pipe(ends) == 0, "model pipe creation failed"); }
  ~Pipe() {
    for (const int fd : ends)
      if (fd >= 0) ::close(fd);
  }
  void close_end(unsigned i) {
    if (ends[i] >= 0) {
      ::close(ends[i]);
      ends[i] = -1;
    }
  }
};
void private_text(const fs::path& path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), std::streamsize(bytes.size()));
  output.close();
  require(bool(output) && ::chmod(path.c_str(), 0600) == 0, "model private file write failed");
}
std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::string value(65537, '\0');
  input.read(value.data(), std::streamsize(value.size()));
  require(input.gcount() <= 65536, "model process output exceeded bounds");
  value.resize(std::size_t(input.gcount()));
  return value;
}
struct OwnedRuntime {
  fs::path path;
  OwnedRuntime() {
    char selected[] = "/tmp/headunit-native-model-XXXXXX";
    require(::mkdtemp(selected) != nullptr, "model runtime creation failed");
    path = selected;
    if (::chmod(path.c_str(), 0700) != 0) {
      std::error_code ignored;
      fs::remove_all(path, ignored);
      throw std::runtime_error("model runtime permissions failed");
    }
  }
  ~OwnedRuntime() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
struct Process {
  OwnedRuntime runtime;
  fs::path directory, profile, display, request, lease, status, log;
  Pipe events, commands;
  pid_t pid = -1;
  std::optional<int> exit_status;
  std::string trace;
  explicit Process(Mode mode) : directory(runtime.path) {
    profile = directory / "camera.json";
    display = directory / "displays.conf";
    request = directory / "request";
    lease = directory / "lease";
    status = directory / "status";
    log = directory / "process.log";
    private_text(
        profile,
        R"({"schemaVersion":1,"enabled":true,"manualActivation":true,"streamIdentifier":"owner-model-camera",
      "source":{"kind":"synthetic-test-pattern"},"capture":{"width":320,"height":180,"frameRate":30,"maximumAgeMs":150},
      "destination":{"panel":"main","canvas":{"width":640,"height":360},
      "frame":{"x":0,"y":0,"width":640,"height":360},"rotationDegrees":0,"mirrorHorizontal":false}})");
    private_text(
        display,
        "main.width=640\nmain.height=360\nmain.fps=30\nmain.panel=Center_Display\n"
        "main.wayland=owner-model-main\nmain.output=owner-model-output\n"
        "cluster.width=640\ncluster.height=360\ncluster.fps=30\ncluster.panel=Cluster_Display\n");
    private_text(request, "on\n");  // Explicit legacy bench input, never a managed publication.
    std::cout.flush();
    std::cerr.flush();
    pid = ::fork();
    require(pid >= 0, "model child fork failed");
    if (!pid) {
      events.close_end(0);
      commands.close_end(1);
      const int output = ::open(log.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
      if (output < 0 || ::dup2(output, STDOUT_FILENO) < 0 || ::dup2(output, STDERR_FILENO) < 0)
        ::_exit(94);
      ::close(output);
      ChildState child{mode, events.ends[1], commands.ends[0]};
      fixture = &child;
      std::vector<std::string> values = {
          "native-owner-model", "--camera-profile", profile.string(), "--display-config",
          display.string(),     "--request-file",   request.string(), "--surface-lease-file",
          lease.string(),       "--status-file",    status.string()};
      std::vector<char*> argv;
      for (auto& value : values)
        argv.push_back(value.data());
      const auto result = headunit_native_camera_owner_entry(int(argv.size()), argv.data());
      std::cout.flush();
      std::cerr.flush();
      ::_exit(result);
    }
    events.close_end(1);
    commands.close_end(0);
  }
  ~Process() {
    if (pid > 0 && !exit_status) {
      (void)::kill(pid, SIGKILL);
      reap_noexcept();
    }
  }
  bool exited() {
    if (exit_status) return true;
    int value = 0;
    const auto result = ::waitpid(pid, &value, WNOHANG);
    if (result == pid) {
      exit_status = value;
      return true;
    }
    require(result == 0 || (result < 0 && errno == EINTR), "model child wait failed");
    return false;
  }
  void reap_noexcept() noexcept {
    const auto until = headunit::Clock::now() + 2000ms;
    while (headunit::Clock::now() < until) {
      int value = 0;
      const auto result = ::waitpid(pid, &value, WNOHANG);
      if (result == pid) {
        exit_status = value;
        return;
      }
      if (result < 0 && errno != EINTR) return;
      (void)::poll(nullptr, 0, 1);
    }
  }
  void send(char operation, std::uint64_t deadline = 0) {
    const Command value{deadline, operation};
    write_all(commands.ends[1], &value, sizeof(value));
  }
  void wait_event(char expected) {
    const auto until = headunit::Clock::now() + 2000ms;
    do {
      pollfd descriptor{events.ends[0], POLLIN, 0};
      const auto ready = ::poll(&descriptor, 1, 10);
      if (ready < 0 && errno == EINTR) continue;
      require(ready >= 0, "model event poll failed");
      if (ready && descriptor.revents & POLLIN) {
        char value = 0;
        require(::read(events.ends[0], &value, 1) == 1, "model event read failed");
        trace += value;
        if (value == expected) return;
      } else if (exited())
        throw std::runtime_error("model child exited before handshake: " + read_file(log));
    } while (headunit::Clock::now() < until);
    throw std::runtime_error("model event timeout: " + trace + ": " + read_file(log));
  }
  void finish(int expected) {
    const auto until = headunit::Clock::now() + 2000ms;
    while (!exited() && headunit::Clock::now() < until)
      (void)::poll(nullptr, 0, 1);
    require(exit_status && WIFEXITED(*exit_status) && WEXITSTATUS(*exit_status) == expected,
            "model child exit code or deadline mismatch");
    // Drain complete event bytes after exit; no pipe writer remains.
    for (unsigned turns = 0; turns < 128; ++turns) {
      pollfd descriptor{events.ends[0], POLLIN, 0};
      if (::poll(&descriptor, 1, 0) <= 0 || !(descriptor.revents & POLLIN)) break;
      char value = 0;
      if (::read(events.ends[0], &value, 1) != 1) break;
      trace += value;
    }
    require(trace.size() < 128, "model event trace exceeded bounds");
    (void)read_file(log);
  }
  void stop() { require(!exited() && ::kill(pid, SIGTERM) == 0, "model SIGTERM failed"); }
};
void phase(c::StatusReader& reader, c::ControlPhase expected) {
  const auto current = reader.poll(1000);
  require(current && current->phase == expected, "actual HCS1 status phase mismatch");
}
void cold_request_expiry() {
  Process process(Mode::cold_expiry);
  process.wait_event('C');
  c::CameraSurfaceLease main(process.lease.string());
  require(!main.acquire_shared(), "cold owner never acquired actual EX");
  c::RequestPublisher request(process.request.string(),
                              (process.directory / "request-writer.owner").string());
  const auto original = clock_ns() + 40'000'000;
  request.publish(true, original);
  process.send('X', original);
  process.wait_event('I');
  process.wait_event('E');
  require(main.acquire_shared(), "expired startup kept its EX surface ownership");
  main.release();
  process.stop();
  process.finish(0);
  require(process.trace.find('T') == process.trace.npos,
          "expired cold request submitted a first fake frame");
  require(process.trace.find('S') != process.trace.npos &&
              process.trace.find('E') != process.trace.npos,
          "expired cold source was not stopped/destroyed");
}
void outer_heartbeat_failure() {
  Process process(Mode::heartbeat_failure);
  process.wait_event('T');
  c::CameraSurfaceLease main(process.lease.string());
  require(!main.acquire_shared(), "active model failed to hold actual EX");
  // This is a real production StatusPublisher failure outside its render try.
  require(::chmod(process.status.c_str(), 0644) == 0, "model status failure injection failed");
  process.send('H');
  process.wait_event('B');
  require(!process.exited(), "provider destructor did not pause at its handshake");
  require(process.trace.find('D') < process.trace.find('B') &&
              process.trace.find('S') < process.trace.find('B'),
          "outer status exception joined provider before withdrawing display/source");
  require(main.acquire_shared(),
          "outer status exception held EX during delayed provider destruction");
  main.release();
  process.send('R');
  process.finish(1);
  require(process.trace.find('E') != process.trace.npos &&
              read_file(process.log).find("single-link") != std::string::npos,
          "outer status failure was hidden or provider never finished");
}
void terminal_provider_failure() {
  Process process(Mode::provider_failure);
  process.wait_event('B');
  c::StatusReader status(process.status.string());
  phase(status, c::ControlPhase::fault);
  c::CameraSurfaceLease main(process.lease.string());
  require(main.acquire_shared(), "failed capture kept EX during retirement");
  main.release();
  require(process.trace.find('F') != process.trace.npos &&
              process.trace.find('T') == process.trace.npos,
          "terminal fake capture failure submitted instead of withdrawing");
  process.send('R');
  process.wait_event('E');
  // New actual fault heartbeats must not relaunch under the same still-ON input.
  // The owner publishes concurrently. A bounded read may temporarily withdraw
  // during replacement; collect genuine status within the original heartbeat
  // window, rather than require one instantaneous read to be a receipt.
  const auto until = headunit::Clock::now() + 500ms;
  std::optional<c::ControlStatus> first;
  bool advanced = false;
  while (headunit::Clock::now() < until) {
    const auto next = status.poll(1000);
    if (next) {
      require(next->phase == c::ControlPhase::fault, "same ON cleared fault latch");
      if (!first)
        first = next;
      else if (next->sequence >= first->sequence + 2) {
        advanced = true;
        break;
      }
    }
    require(!process.exited(), "faulted native owner exited prematurely");
    (void)::poll(nullptr, 0, 1);
  }
  require(bool(first), "fault status disappeared");
  require(advanced, "faulted owner stopped its actual status heartbeat");
  process.stop();
  process.finish(0);
  require(std::count(process.trace.begin(), process.trace.end(), 'C') == 1,
          "terminal fault retried without a genuine OFF edge");
}
void presentation_count_gate() {
  Process process(Mode::presentation_gate);
  process.wait_event('W');
  c::StatusReader status(process.status.string());
  phase(status, c::ControlPhase::waiting);
  c::CameraSurfaceLease main(process.lease.string());
  require(!main.acquire_shared(), "waiting display lost actual EX");
  process.send('P');
  process.wait_event('A');
  phase(status, c::ControlPhase::active);
  process.stop();
  process.send('Q');
  process.finish(0);
  phase(status, c::ControlPhase::stopped);
  require(main.acquire_shared(), "normal native owner shutdown kept EX");
  main.release();
}
}  // namespace model
int main(int argc, char** argv) try {
  // The real CLI preflight creates no display/provider; allow the shared
  // Python validation suite to exercise it against this portable link.
  if (argc > 1) {
    if (std::find_if(argv + 1, argv + argc, [](const char* argument) {
          return std::string_view(argument) == "--validate-only";
        }) == argv + argc) {
      std::cerr << "Portable CLI fixture supports --validate-only exclusively\n";
      return 1;
    }
    return headunit_native_camera_owner_entry(argc, argv);
  }
  model::cold_request_expiry();
  model::outer_heartbeat_failure();
  model::terminal_provider_failure();
  model::presentation_count_gate();
  std::cout << "Native camera owner: four actual-loop expiry/cleanup/fault/status model groups "
               "passed (explicit fake display/provider; no GPU evidence)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
