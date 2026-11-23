// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/pending_bytes.hpp"

#include <array>
#include <deque>
#include <iostream>
#include <new>
#include <random>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

void fail_allocation_after(int count);

namespace {
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
template <class F>
void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid queue operation succeeded");
}
}  // namespace

int main() try {
  headunit::PendingBytes queue(8);
  const std::array<std::uint8_t, 6> first{1, 2, 3, 4, 5, 6};
  queue.append(first);
  const auto* start = queue.pending().data();
  queue.consume(5);
  check(queue.pending().data() == start + 5, "partial send shifted remaining bytes");
  queue.append(std::array<std::uint8_t, 4>{7, 8, 9, 10});
  const std::array<std::uint8_t, 5> retained{6, 7, 8, 9, 10};
  check(std::ranges::equal(queue.pending(), retained), "compaction changed wire order");
  rejects([&] { queue.append(first); });
  rejects([&] { queue.consume(6); });
  check(std::ranges::equal(queue.pending(), retained), "rejection changed pending bytes");
  queue.consume(1);
  headunit::PendingBytes moved(std::move(queue));
  check(queue.empty() && queue.available() == 8, "moved queue retained pending state");
  queue = std::move(moved);
  check(moved.empty() && queue.size() == 4, "move assignment lost output or retained old state");
  queue.consume(4);
  queue.append(first);
  queue.clear();
  check(queue.empty() && queue.available() == 8, "clear retained output state");

  headunit::PendingBytes faults(32);
  faults.append(std::array<std::uint8_t, 26>{1, 2, 3});
  faults.consume(23);
  const auto before = faults.pending();
  const std::vector<std::uint8_t> expected(before.begin(), before.end());
  const std::array<std::uint8_t, 24> more{};
  fail_allocation_after(0);
  bool injected = false;
  try {
    faults.append(more);
  } catch (const std::bad_alloc&) {
    injected = true;
  }
  fail_allocation_after(-1);
  check(injected && std::ranges::equal(faults.pending(), expected),
        "allocation failure lost previously queued ciphertext");
  faults.append(more);
  check(faults.size() == 27, "allocation failure made append unretryable");

  // Match a simple reference FIFO across many partial sends and capacity refusals.
  headunit::PendingBytes stream(4096);
  std::deque<std::uint8_t> reference;
  std::mt19937 random(0x20261009);
  for (unsigned turn = 0; turn < 20000; ++turn) {
    if (random() & 1) {
      std::vector<std::uint8_t> bytes(random() % 1025);
      for (auto& byte : bytes)
        byte = static_cast<std::uint8_t>(random());
      if (bytes.size() > 4096 - reference.size()) {
        rejects([&] { stream.append(bytes); });
      } else {
        stream.append(bytes);
        reference.insert(reference.end(), bytes.begin(), bytes.end());
      }
    } else {
      const auto count = std::min<std::size_t>(random() % 1025, reference.size());
      stream.consume(count);
      reference.erase(reference.begin(), reference.begin() + count);
    }
    check(stream.available() == 4096 - reference.size() &&
              std::ranges::equal(stream.pending(), reference),
          "partial send/append diverged from reference FIFO");
  }

  // Actual nonblocking writes with a deliberately small socket send buffer.
  // Append after the first partial write, then compare the complete byte stream.
  struct Sockets {
    int fds[2]{-1, -1};
    ~Sockets() {
      for (const auto fd : fds)
        if (fd >= 0) close(fd);
    }
  } sockets;
  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.fds) == 0, "socketpair failed");
  for (const auto fd : sockets.fds)
    check(fcntl(fd, F_SETFL, O_NONBLOCK) == 0, "nonblocking socket setup failed");
  const int socket_capacity = 4096;
  check(setsockopt(sockets.fds[0], SOL_SOCKET, SO_SNDBUF, &socket_capacity,
                   sizeof(socket_capacity)) == 0,
        "socket send buffer setup failed");
  headunit::PendingBytes socket_output(65536);
  std::vector<std::uint8_t> wire(65536);
  for (std::size_t i = 0; i < wire.size(); ++i)
    wire[i] = static_cast<std::uint8_t>(i);
  socket_output.append(wire);
  const auto pending = socket_output.pending();
  const auto sent = write(sockets.fds[0], pending.data(), pending.size());
  check(sent > 0 && static_cast<std::size_t>(sent) < pending.size(),
        "small socket buffer did not produce a partial write");
  socket_output.consume(static_cast<std::size_t>(sent));
  check(socket_output.pending().data() == pending.data() + sent,
        "socket partial write shifted queued bytes");
  std::vector<std::uint8_t> tail(static_cast<std::size_t>(sent), 0xa5);
  socket_output.append(tail);
  wire.insert(wire.end(), tail.begin(), tail.end());
  std::vector<std::uint8_t> received;
  for (unsigned turn = 0; turn < 10000 && received.size() < wire.size(); ++turn) {
    if (!socket_output.empty()) {
      const auto bytes = socket_output.pending();
      const auto count = write(sockets.fds[0], bytes.data(), bytes.size());
      if (count > 0)
        socket_output.consume(static_cast<std::size_t>(count));
      else
        check(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR),
              "queued socket write failed");
    }
    std::array<std::uint8_t, 1003> buffer{};
    const auto count = read(sockets.fds[1], buffer.data(), buffer.size());
    if (count > 0)
      received.insert(received.end(), buffer.begin(), buffer.begin() + count);
    else
      check(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR),
            "queued socket read failed");
  }
  check(socket_output.empty() && received == wire,
        "partial socket writes lost, duplicated or reordered bytes");
  std::cout << "Bounded output queue tests passed\n";
} catch (const std::exception& error) {
  fail_allocation_after(-1);
  std::cerr << error.what() << '\n';
  return 1;
}
