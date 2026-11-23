// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/control_response.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct Sockets {
  std::array<int, 2> fd{-1, -1};
  Sockets() {
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, fd.data()) == 0, "socketpair failed");
    const int small = 1024;
    check(setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0,
          "send buffer failed");
    for (const auto socket : fd) {
      const auto flags = fcntl(socket, F_GETFL);
      check(flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0, "nonblocking failed");
    }
  }
  int release_peer() {
    const int peer = fd[1];
    fd[1] = -1;
    return peer;
  }
  ~Sockets() {
    for (const auto socket : fd)
      if (socket >= 0) close(socket);
  }
};
std::chrono::milliseconds timed_out(int socket, const std::vector<std::uint8_t>& bytes,
                                    std::chrono::milliseconds timeout = 2000ms) {
  const auto begin = Clock::now();
  bool rejected = false;
  try {
    headunit::send_control_response(socket, bytes, timeout);
  } catch (const std::runtime_error& error) {
    check(std::string(error.what()) == "control response send timed out",
          "failure was not the absolute deadline");
    rejected = true;
  }
  check(rejected, "blocked response completed unexpectedly");
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin);
  check(elapsed >= timeout, "deadline was fabricated or expired early");
  check(elapsed < timeout + 700ms, "deadline renewed or owner remained blocked");
  return elapsed;
}
volatile std::sig_atomic_t interrupts = 0;
void interrupt(int) {
  interrupts = interrupts + 1;
}
}  // namespace

int main() try {
  std::vector<std::uint8_t> payload(256 * 1024);
  for (std::size_t index = 0; index < payload.size(); ++index)
    payload[index] = static_cast<std::uint8_t>(index * 37);

  // Normal completion preserves bytes across actual partial socket writes.
  {
    Sockets sockets;
    std::vector<std::uint8_t> received;
    unsigned reads = 0;
    std::atomic<bool> stop{false};
    const int peer = sockets.release_peer();
    std::thread reader([&] {
      const auto deadline = Clock::now() + 3s;
      std::array<std::uint8_t, 511> buffer{};
      while (received.size() < payload.size() && !stop && Clock::now() < deadline) {
        pollfd descriptor{peer, POLLIN, 0};
        if (poll(&descriptor, 1, 20) <= 0) continue;
        const auto count = recv(peer, buffer.data(), buffer.size(), 0);
        if (count <= 0) break;
        ++reads;
        received.insert(received.end(), buffer.begin(), buffer.begin() + count);
      }
      close(peer);
    });
    try {
      headunit::send_control_response(sockets.fd[0], payload);
    } catch (...) {
      stop = true;
      reader.join();
      throw;
    }
    reader.join();
    check(received == payload && reads > 1, "partial-write bytes/order changed");
  }
  // Actual no-reader socket backpressure reaches the production two-second
  // whole-call limit; no fake clock or readiness event is substituted.
  {
    Sockets sockets;
    timed_out(sockets.fd[0], payload);
  }
  // The peer makes repeated real progress, but progress cannot renew the
  // envelope. Its own one-second cap bounds the fixture even for old senders.
  {
    Sockets sockets;
    const int peer = sockets.release_peer();
    std::atomic<bool> stop{false};
    std::size_t received = 0;
    std::thread reader([&] {
      const auto deadline = Clock::now() + 1s;
      std::array<std::uint8_t, 512> buffer{};
      while (!stop && Clock::now() < deadline) {
        const auto count = recv(peer, buffer.data(), buffer.size(), 0);
        if (count > 0) received += static_cast<std::size_t>(count);
        // 512 B per 2 ms cannot drain 256 KiB within the 200 ms envelope, and
        // still makes several reads on a heavily loaded CI host.
        std::this_thread::sleep_for(2ms);
      }
      close(peer);
    });
    try {
      timed_out(sockets.fd[0], payload, 200ms);
    } catch (...) {
      stop = true;
      reader.join();
      throw;
    }
    stop = true;
    reader.join();
    if (!(received > 4096 && received < payload.size()))
      throw std::runtime_error("trickle fixture did not make partial progress; received bytes=" +
                               std::to_string(received));
  }
  // Real signals interrupt the actual poll; repeated EINTR cannot reset its
  // date. The signal handler performs only an async-signal-safe scalar write.
  {
    Sockets sockets;
    struct sigaction action{}, previous{};
    action.sa_handler = interrupt;
    sigemptyset(&action.sa_mask);
    check(sigaction(SIGUSR1, &action, &previous) == 0, "signal fixture failed");
    const auto target = pthread_self();
    std::atomic<bool> stop{false};
    std::thread signaler([&] {
      const auto deadline = Clock::now() + 1s;
      while (!stop && Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
        if (!stop) pthread_kill(target, SIGUSR1);
      }
    });
    try {
      timed_out(sockets.fd[0], payload, 100ms);
    } catch (...) {
      stop = true;
      signaler.join();
      sigaction(SIGUSR1, &previous, nullptr);
      throw;
    }
    stop = true;
    signaler.join();
    sigaction(SIGUSR1, &previous, nullptr);
    check(interrupts > 1, "EINTR boundary was not exercised");
  }
  // Closed peers fail promptly without SIGPIPE terminating the owner.
  {
    Sockets sockets;
    close(sockets.release_peer());
    bool rejected = false;
    const auto begin = Clock::now();
    try {
      headunit::send_control_response(sockets.fd[0], payload);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected && Clock::now() - begin < 700ms, "peer close did not fail promptly");
  }
  // A closure during POLLOUT wait must retire the incomplete response, not
  // promote a hangup event into a successful write or spin on readiness.
  {
    Sockets sockets;
    const int peer = sockets.release_peer();
    std::thread closer([&] {
      std::this_thread::sleep_for(30ms);
      close(peer);
    });
    bool rejected = false;
    const auto begin = Clock::now();
    try {
      headunit::send_control_response(sockets.fd[0], payload);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    closer.join();
    check(rejected && Clock::now() - begin < 700ms, "wait hangup did not fail promptly");
  }
  for (const auto timeout : {0ms, -1ms, 2001ms}) {
    bool rejected = false;
    try {
      headunit::send_control_response(-1, payload, timeout);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "local deadline policy was bypassed");
  }
  std::cout << "actual socket response deadlines, progress, EINTR and peer retirement passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
