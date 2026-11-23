// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Cross-language fixture: actual clock, request reader, status publisher and
// native Close client. HCS1 phases here are fixture observations, not GPU or
// camera presentation receipts. No camera/device/display is opened.
#include <headunit/camera_control.hpp>
#include <headunit/camera_controller_io.hpp>
#include <headunit/camera_manual_client.hpp>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {
namespace c = headunit::camera;
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
std::uint64_t now_ns() {
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  require(count > 0, "nonpositive real steady clock");
  return static_cast<std::uint64_t>(count);
}
std::string quoted(std::string_view value) {
  std::string out = "\"";
  for (const unsigned char ch : value) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += static_cast<char>(ch);
    } else if (ch >= 32 && ch <= 126)
      out += static_cast<char>(ch);
    else
      out += '?';
  }
  return out + '"';
}
std::string_view phase_name(c::ControlPhase phase) {
  switch (phase) {
    case c::ControlPhase::idle:
      return "idle";
    case c::ControlPhase::waiting:
      return "waiting";
    case c::ControlPhase::active:
      return "active";
    case c::ControlPhase::fault:
      return "fault";
    case c::ControlPhase::stopped:
      return "stopped";
  }
  throw std::runtime_error("unknown fixture phase");
}
c::ControlPhase phase_value(std::string_view value) {
  for (const auto phase : {c::ControlPhase::idle, c::ControlPhase::waiting, c::ControlPhase::active,
                           c::ControlPhase::fault, c::ControlPhase::stopped})
    if (phase_name(phase) == value) return phase;
  throw std::runtime_error("invalid fixture status command");
}
unsigned duration_ms(std::string_view text) {
  unsigned duration = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), duration);
  require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
              duration >= 100 && duration <= 5000,
          "fixture status duration must be 100..5000 ms");
  return duration;
}
void status_line(const c::ControlStatus& value) {
  std::cout << "{\"instance\":" << value.instance_started_ns << ",\"sequence\":" << value.sequence
            << ",\"observed\":" << value.observed_ns
            << ",\"phase\":" << quoted(phase_name(value.phase)) << "}\n"
            << std::flush;
}
void publish_status(const std::string& path, unsigned duration) {
  c::StatusPublisher publisher(path);
  const auto started = now_ns();
  c::ControlStatus value{started, 1, started, c::ControlPhase::idle};
  publisher.publish(value);
  status_line(value);
  // This process owns the publisher for the whole fixture. Every renewal uses
  // its actual current steady date and strictly increasing same-instance seq.
  const int flags = ::fcntl(STDIN_FILENO, F_GETFL);
  require(flags >= 0 && ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0,
          "fixture stdin could not become nonblocking");
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration);
  std::string pending;
  while (std::chrono::steady_clock::now() < until) {
    pollfd poller{STDIN_FILENO, POLLIN, 0};
    const int ready = ::poll(&poller, 1, 10);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("fixture status poll failed");
    }
    require(!(poller.revents & (POLLERR | POLLNVAL)), "fixture status input failed");
    bool command = false;
    if (poller.revents & (POLLIN | POLLHUP)) {
      std::array<char, 64> bytes{};
      const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
      if (!count) return;
      if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
          throw std::runtime_error("fixture status read failed");
      } else {
        pending.append(bytes.data(), static_cast<std::size_t>(count));
        require(pending.size() <= 64, "fixture status input exceeded bound");
        for (auto newline = pending.find('\n'); newline != pending.npos;
             newline = pending.find('\n')) {
          value.phase = phase_value(std::string_view(pending).substr(0, newline));
          pending.erase(0, newline + 1);
          command = true;
        }
        require(pending.size() <= 7, "fixture status command exceeded bound");
      }
    }
    const auto observed = now_ns();
    require(observed >= value.observed_ns && value.sequence < UINT64_MAX,
            "fixture steady clock regressed or sequence exhausted");
    value.observed_ns = observed;
    ++value.sequence;
    publisher.publish(value);
    if (command) status_line(value);
  }
}
std::string_view close_name(c::ManualCloseState state) {
  switch (state) {
    case c::ManualCloseState::idle:
      return "idle";
    case c::ManualCloseState::pending:
      return "pending";
    case c::ManualCloseState::accepted:
      return "accepted";
    case c::ManualCloseState::failed:
      return "failed";
  }
  throw std::runtime_error("unknown native Close result");
}
int close_manual(const std::string& path) {
  c::ManualCloseClient client(path, 200);
  require(client.configured() && client.request_close(), "native Close was not queued");
  const auto until = std::chrono::steady_clock::now() + 1000ms;
  c::ManualCloseResult result;
  do {
    result = client.status();
    if (result.state == c::ManualCloseState::accepted ||
        result.state == c::ManualCloseState::failed)
      break;
    std::this_thread::sleep_for(2ms);
  } while (std::chrono::steady_clock::now() < until);
  require(
      result.state == c::ManualCloseState::accepted || result.state == c::ManualCloseState::failed,
      "native Close exceeded fixture deadline");
  std::cout << "{\"state\":" << quoted(close_name(result.state))
            << ",\"sequence\":" << result.request_sequence
            << ",\"message\":" << quoted(result.message) << "}\n";
  client.request_stop();
  return result.state == c::ManualCloseState::accepted ? 0 : 2;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc >= 2,
            "usage: camera-manual-interop clock | request PATH | status-publish PATH [MS] | "
            "manual-client PATH");
    const std::string_view mode = argv[1];
    if (mode == "clock" && argc == 2) {
      std::cout << now_ns() << '\n';
      return 0;
    }
    if (mode == "request" && argc == 3) {
      c::ActivationControl reader(argv[2]);
      switch (reader.read()) {
        case c::ActivationRequest::on:
          std::cout << "on\n";
          break;
        case c::ActivationRequest::off:
          std::cout << "off\n";
          break;
        case c::ActivationRequest::unavailable:
          std::cout << "unknown\n";
          break;
      }
      return 0;
    }
    if (mode == "status-publish" && (argc == 3 || argc == 4)) {
      publish_status(argv[2], argc == 4 ? duration_ms(argv[3]) : 5000);
      return 0;
    }
    if (mode == "manual-client" && argc == 3) return close_manual(argv[2]);
    throw std::runtime_error("invalid camera interop fixture arguments");
  } catch (const std::exception& error) {
    std::cerr << "camera manual interop: " << error.what() << '\n';
    return 1;
  }
}
