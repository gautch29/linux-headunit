// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/control_response.hpp"

#include <cerrno>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>

namespace headunit {
void send_control_response(int socket, std::span<const std::uint8_t> response,
                           std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(2))
    throw std::invalid_argument("control response deadline must be within two seconds");
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  if (response.empty()) return;
#ifndef MSG_NOSIGNAL
  const int suppress_sigpipe = 1;
  if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &suppress_sigpipe, sizeof(suppress_sigpipe)) < 0)
    throw std::runtime_error("control response socket configuration failed");
#endif
  while (!response.empty()) {
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("control response send timed out");
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
    constexpr int flags = MSG_DONTWAIT;
#endif
    const auto count = send(socket, response.data(), response.size(), flags);
    if (count > 0) {
      response = response.subspan(static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) throw std::runtime_error("control response send timed out");
      // Round up only the individual poll wait; the next iteration checks the
      // same absolute date, so signals/progress cannot renew its envelope.
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
      pollfd descriptor{socket, POLLOUT, 0};
      const int ready = poll(&descriptor, 1, static_cast<int>(remaining.count()));
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0 || (ready > 0 && ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) ||
                                      !(descriptor.revents & POLLOUT))))
        throw std::runtime_error("control response socket failed while waiting");
      continue;
    }
    throw std::runtime_error("control response send failed");
  }
}
}  // namespace headunit
