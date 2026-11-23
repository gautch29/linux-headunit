// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_manual_client.hpp>
#include "theme/vehicle_json.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace headunit::camera {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view request = "{\"action\":\"camera\",\"enabled\":false}\n";
constexpr std::size_t maximum_response = 4096, maximum_message = 1200;
void check(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}
struct File {
  int fd = -1;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
  File() = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
};
bool same(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool private_parent(const struct stat& value) {
  return S_ISDIR(value.st_mode) && value.st_uid == getuid() && (value.st_mode & 07777) == 0700;
}
bool private_socket(const struct stat& value) {
  return S_ISSOCK(value.st_mode) && value.st_uid == getuid() && (value.st_mode & 07777) == 0600 &&
         value.st_nlink == 1;
}
bool canonical(std::string_view value) {
  if (value.size() < 2 || value.size() > 103 || value.front() != '/' || value.back() == '/')
    return false;
  // Passed directly to AF_UNIX and filesystem APIs, never a shell or URI.
  if (!std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c <= 126; }))
    return false;
  for (std::size_t begin = 1; begin < value.size();) {
    const auto slash = value.find('/', begin), end = slash == value.npos ? value.size() : slash;
    const auto part = value.substr(begin, end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    begin = end + 1;
  }
  return true;
}
void nonblocking(int fd) {
  const auto flags = ::fcntl(fd, F_GETFL);
  check(flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
            ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0,
        "Camera command descriptor could not be secured");
}
void peer(int fd) {
#if defined(__linux__)
  struct ucred identity{};
  socklen_t size = sizeof(identity);
  check(::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &identity, &size) == 0 &&
            size == sizeof(identity) && (identity.uid == getuid() || identity.uid == 0),
        "Camera command peer has an unexpected owner");
#elif defined(__APPLE__)
  uid_t uid = 0;
  gid_t gid = 0;
  check(::getpeereid(fd, &uid, &gid) == 0 && (uid == getuid() || uid == 0),
        "Camera command peer has an unexpected owner");
#else
  (void)fd;
  throw std::runtime_error("Camera command peer verification is unavailable on this platform");
#endif
}
ManualCloseResult response(std::string_view bytes, std::uint64_t sequence) {
  check(!bytes.empty() && bytes.size() <= maximum_response && bytes.back() == '\n',
        "Camera command reply is incomplete or oversized");
  const auto parsed = theme::vehicle::detail::Json(bytes).parse();
  const auto* fields = std::get_if<theme::opack::Dictionary>(&parsed.data);
  check(fields && fields->size() == 2, "Camera command reply has unknown or missing fields");
  const bool* ok = nullptr;
  const std::string* message = nullptr;
  bool is_error = false;
  for (const auto& [key, value] : *fields) {
    const auto* name = std::get_if<std::string>(&key.data);
    check(name, "Camera command reply key is invalid");
    if (*name == "ok")
      ok = std::get_if<bool>(&value.data);
    else if (*name == "message" || *name == "error") {
      message = std::get_if<std::string>(&value.data);
      is_error = *name == "error";
    } else
      throw std::runtime_error("Camera command reply has an unknown acknowledgement field");
  }
  check(
      ok && message && !message->empty() && message->size() <= maximum_message && (*ok != is_error),
      "Camera command reply has an invalid acknowledgement state");
  return {*ok ? ManualCloseState::accepted : ManualCloseState::failed, sequence, *message};
}
}  // namespace
struct ManualCloseClient::Impl {
  std::optional<std::string> socket_path;
  const unsigned timeout_ms;
  mutable std::mutex mutex;
  std::condition_variable ready;
  ManualCloseResult result;
  bool queued = false;
  std::atomic<bool> stopping{false};
  File wake_read, wake_write, parent;
  std::string parent_path, name;
  struct stat parent_identity{}, socket_identity{};
  bool pinned_parent = false, pinned_socket = false;
  std::thread worker;
  Impl(std::optional<std::string> selected, unsigned timeout)
      : socket_path(std::move(selected)), timeout_ms(timeout) {
    if (socket_path && !canonical(*socket_path))
      throw std::invalid_argument(
          "Camera command socket must be a canonical bounded absolute ASCII path");
    if (timeout_ms < 10 || timeout_ms > 500)
      throw std::invalid_argument("Camera command timeout must be 10..500 ms");
    if (!socket_path) return;
    int descriptors[2];
    check(::pipe(descriptors) == 0, "Camera command cancellation pipe could not be created");
    wake_read.fd = descriptors[0];
    wake_write.fd = descriptors[1];
    nonblocking(wake_read.fd);
    nonblocking(wake_write.fd);
    worker = std::thread([this] { run(); });
  }
  ~Impl() {
    stop();
    if (worker.joinable()) worker.join();
  }
  void stop() noexcept {
    if (!stopping.exchange(true) && wake_write.fd >= 0) {
      const char byte = 1;
      (void)::write(wake_write.fd, &byte, 1);
    }
    ready.notify_one();
  }
  void due(Clock::time_point deadline) const {
    check(!stopping.load(), "Camera close request cancelled");
    check(Clock::now() < deadline, "Camera close request timed out");
  }
  void wait(int socket, short events, Clock::time_point deadline) const {
    for (unsigned turns = 0; turns < 64; ++turns) {
      due(deadline);
      const auto remaining =
          std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
      std::array<pollfd, 2> descriptors = {pollfd{socket, events, 0},
                                           pollfd{wake_read.fd, POLLIN, 0}};
      const auto received =
          ::poll(descriptors.data(), descriptors.size(), int(std::max<std::int64_t>(1, remaining)));
      if (received < 0 && errno == EINTR) continue;
      due(deadline);
      check(received >= 0, "Camera command socket poll failed");
      if (descriptors[1].revents) throw std::runtime_error("Camera close request cancelled");
      if (descriptors[0].revents & (events | POLLHUP | POLLERR | POLLNVAL)) return;
    }
    throw std::runtime_error("Camera command wait exceeded its retry bound");
  }
  void verify_path() {
    if (!pinned_parent) {
      const std::filesystem::path selected(*socket_path);
      parent_path = selected.parent_path().string();
      name = selected.filename().string();
      check(::lstat(parent_path.c_str(), &parent_identity) == 0 && private_parent(parent_identity),
            "Camera command parent must be a real owned 0700 directory");
      std::error_code error;
      check(std::filesystem::canonical(parent_path, error) == parent_path && !error,
            "Camera command parent must not use redirected ancestors");
      File opened_parent;
      opened_parent.fd =
          ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
      struct stat opened{};
      check(opened_parent.fd >= 0 && ::fstat(opened_parent.fd, &opened) == 0 &&
                private_parent(opened) && same(parent_identity, opened),
            "Camera command parent changed while opening");
      parent.fd = std::exchange(opened_parent.fd, -1);
      pinned_parent = true;
    }
    struct stat named_parent{}, opened_parent{}, endpoint{};
    check(::lstat(parent_path.c_str(), &named_parent) == 0 && private_parent(named_parent) &&
              same(parent_identity, named_parent) && ::fstat(parent.fd, &opened_parent) == 0 &&
              private_parent(opened_parent) && same(parent_identity, opened_parent),
          "Camera command parent generation changed");
    std::error_code error;
    check(std::filesystem::canonical(parent_path, error) == parent_path && !error,
          "Camera command parent must not use redirected ancestors");
    check(::fstatat(parent.fd, name.c_str(), &endpoint, AT_SYMLINK_NOFOLLOW) == 0 &&
              private_socket(endpoint),
          "Camera command endpoint must be an owned single-link 0600 socket");
    if (pinned_socket)
      check(same(socket_identity, endpoint), "Camera command socket generation changed");
    else {
      socket_identity = endpoint;
      pinned_socket = true;
    }
  }
  ManualCloseResult exchange(std::uint64_t sequence) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    due(deadline);
    verify_path();
    due(deadline);
    File connection;
    connection.fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    check(connection.fd >= 0, "Camera command socket could not be created");
    nonblocking(connection.fd);
#ifdef __APPLE__
    const int no_sigpipe = 1;
    check(
        ::setsockopt(connection.fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) == 0,
        "Camera command socket could not suppress SIGPIPE");
#endif
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(socket_path->begin(), socket_path->end(), address.sun_path);
    const auto connected =
        ::connect(connection.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected != 0) {
      check(errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK,
            "Camera command connection failed");
      wait(connection.fd, POLLOUT, deadline);
      int error = 0;
      socklen_t size = sizeof(error);
      check(::getsockopt(connection.fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
                size == sizeof(error) && error == 0,
            "Camera command connection was not accepted");
    }
    due(deadline);
    peer(connection.fd);
    verify_path();
    std::size_t sent = 0;
    while (sent < request.size()) {
      due(deadline);
#ifdef __linux__
      const auto count =
          ::send(connection.fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
#else
      const auto count = ::send(connection.fd, request.data() + sent, request.size() - sent, 0);
#endif
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        wait(connection.fd, POLLOUT, deadline);
        continue;
      }
      check(count > 0, "Camera close command could not be sent");
      sent += std::size_t(count);
    }
    std::string reply;
    reply.reserve(512);
    for (;;) {
      due(deadline);
      std::array<char, 512> bytes{};
      const auto count = ::recv(connection.fd, bytes.data(), bytes.size(), 0);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        wait(connection.fd, POLLIN, deadline);
        continue;
      }
      check(count >= 0, "Camera command reply could not be read");
      if (!count) break;
      check(reply.size() + std::size_t(count) <= maximum_response,
            "Camera command reply exceeds 4 KiB");
      reply.append(bytes.data(), std::size_t(count));
    }
    due(deadline);
    verify_path();
    auto completed = response(reply, sequence);
    due(deadline);
    return completed;
  }
  void run() noexcept {
    for (;;) {
      std::uint64_t sequence = 0;
      {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return stopping.load() || queued; });
        if (stopping.load()) {
          if (result.state == ManualCloseState::pending) {
            result.state = ManualCloseState::failed;
            result.message = "Camera close request cancelled";
          }
          return;
        }
        queued = false;
        sequence = result.request_sequence;
      }
      ManualCloseResult completed;
      try {
        completed = exchange(sequence);
      } catch (const std::exception& error) {
        completed = {ManualCloseState::failed, sequence,
                     std::string(error.what()).substr(0, maximum_message)};
      } catch (...) {
        completed = {ManualCloseState::failed, sequence, "Camera close request failed"};
      }
      {
        std::lock_guard lock(mutex);
        if (stopping.load()) {
          completed.state = ManualCloseState::failed;
          completed.message = "Camera close request cancelled";
        }
        result = std::move(completed);
      }
    }
  }
};
ManualCloseClient::ManualCloseClient(std::optional<std::string> socket, unsigned timeout)
    : impl_(std::make_unique<Impl>(std::move(socket), timeout)) {}
ManualCloseClient::~ManualCloseClient() = default;
bool ManualCloseClient::configured() const noexcept {
  return impl_->socket_path.has_value();
}
bool ManualCloseClient::request_close() noexcept {
  try {
    auto& owner = *impl_;
    std::lock_guard lock(owner.mutex);
    if (owner.stopping.load() || !owner.socket_path) {
      owner.result.state = ManualCloseState::failed;
      owner.result.message = owner.stopping.load() ? "Camera command client stopped"
                                                   : "Camera command socket is not configured";
      return false;
    }
    if (owner.result.state == ManualCloseState::pending) return true;
    if (owner.result.request_sequence == std::numeric_limits<std::uint64_t>::max()) return false;
    ++owner.result.request_sequence;
    owner.result.state = ManualCloseState::pending;
    owner.result.message.clear();
    owner.queued = true;
    owner.ready.notify_one();
    return true;
  } catch (...) {
    return false;
  }
}
ManualCloseResult ManualCloseClient::status() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->result;
}
void ManualCloseClient::request_stop() noexcept {
  impl_->stop();
}
}  // namespace headunit::camera
