// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Actual private AF_UNIX command server fixtures. No camera/display/phone or
// installation state is touched. A positive reply proves only acceptance.
#include <headunit/camera_manual_client.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
namespace c = headunit::camera;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::string_view expected = "{\"action\":\"camera\",\"enabled\":false}\n";
constexpr std::string_view accepted = "{\"ok\":true,\"message\":\"Close request accepted\"}\n";
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
template <class F>
void rejects(F action) {
  bool bad = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    bad = true;
  }
  check(bad, "unsafe client configuration was accepted");
}
struct File {
  int fd = -1;
  ~File() {
    if (fd >= 0) ::close(fd);
  }
};
void flags(int fd) {
  const auto current = ::fcntl(fd, F_GETFL);
  check(current >= 0 && ::fcntl(fd, F_SETFL, current | O_NONBLOCK) == 0,
        "fixture socket nonblocking setup failed");
#ifdef __APPLE__
  const int enabled = 1;
  check(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0,
        "fixture SIGPIPE suppression failed");
#endif
}
struct Directory {
  fs::path path;
  Directory() {
    char name[] = "/tmp/headunit-manual-client-XXXXXX";
    check(::mkdtemp(name) != nullptr, "private runtime creation failed");
    path = fs::canonical(name);
    check(::chmod(path.c_str(), 0700) == 0, "private runtime permissions failed");
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
struct Server {
  fs::path path;
  File listener;
  struct stat identity{};
  std::thread worker;
  std::atomic<bool> stopping{false};
  mutable std::mutex mutex;
  std::condition_variable changed;
  unsigned requests = 0;
  bool released = false, hold = false;
  std::string prefix, suffix, error;
  Server(fs::path selected, std::string reply = std::string(accepted), bool pause = false,
         std::string first = {})
      : path(std::move(selected)), hold(pause), prefix(std::move(first)), suffix(std::move(reply)) {
    listener.fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    check(listener.fd >= 0, "fixture socket creation failed");
    flags(listener.fd);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto value = path.string();
    check(value.size() < sizeof(address.sun_path), "fixture Unix socket path is too long");
    std::copy(value.begin(), value.end(), address.sun_path);
    check(::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
              ::chmod(path.c_str(), 0600) == 0 && ::lstat(path.c_str(), &identity) == 0 &&
              ::listen(listener.fd, 4) == 0,
          "fixture socket setup failed");
    worker = std::thread([this] { serve(); });
  }
  ~Server() {
    stopping = true;
    changed.notify_all();
    if (worker.joinable()) worker.join();
    struct stat current{};
    if (::lstat(path.c_str(), &current) == 0 && current.st_dev == identity.st_dev &&
        current.st_ino == identity.st_ino)
      (void)::unlink(path.c_str());
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
  unsigned count() const {
    std::lock_guard lock(mutex);
    check(error.empty(), error.c_str());
    return requests;
  }
  void wait_request(unsigned wanted = 1) {
    std::unique_lock lock(mutex);
    check(changed.wait_for(lock, 1000ms, [&] { return requests >= wanted || !error.empty(); }),
          "fixture request did not arrive");
    check(error.empty(), error.c_str());
  }
  bool wait(int fd, short events, Clock::time_point until) {
    while (!stopping && Clock::now() < until) {
      pollfd descriptor{fd, events, 0};
      const auto ready = ::poll(&descriptor, 1, 5);
      if (ready < 0 && errno == EINTR) continue;
      check(ready >= 0, "fixture socket poll failed");
      if (ready && descriptor.revents & (events | POLLHUP | POLLERR)) return true;
    }
    return false;
  }
  bool send(int fd, std::string_view bytes) {
    const auto until = Clock::now() + 1000ms;
    while (!bytes.empty()) {
      if (!wait(fd, POLLOUT, until)) return false;
      // Small writes deliberately exercise the actual client's framing.
      const auto length = std::min<std::size_t>(7, bytes.size());
#ifdef __linux__
      const auto wrote = ::send(fd, bytes.data(), length, MSG_NOSIGNAL);
#else
      const auto wrote = ::send(fd, bytes.data(), length, 0);
#endif
      if (wrote < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      // Rejected/oversized replies and deadline tests deliberately close the
      // peer while this fixture still writes; Darwin may report ENOTCONN.
      if (wrote < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)) return false;
      if (wrote <= 0)
        throw std::runtime_error("fixture reply write failed: errno " + std::to_string(errno));
      bytes.remove_prefix(std::size_t(wrote));
    }
    return true;
  }
  void serve() noexcept try {
    while (!stopping) {
      if (!wait(listener.fd, POLLIN, Clock::now() + 100ms)) continue;
      File client;
      client.fd = ::accept(listener.fd, nullptr, nullptr);
      if (client.fd < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      check(client.fd >= 0, "fixture accept failed");
      flags(client.fd);
      std::string request;
      const auto until = Clock::now() + 1000ms;
      while (!request.ends_with('\n')) {
        if (!wait(client.fd, POLLIN, until)) {
          if (stopping) return;
          throw std::runtime_error("fixture request timeout");
        }
        std::array<char, 5> bytes{};
        const auto received = ::recv(client.fd, bytes.data(), bytes.size(), 0);
        if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        check(received > 0 && request.size() + std::size_t(received) <= 64,
              "fixture received an invalid command");
        request.append(bytes.data(), std::size_t(received));
      }
      check(request == expected, "client sent anything other than the fixed Close command");
      {
        std::lock_guard lock(mutex);
        ++requests;
        changed.notify_all();
      }
      if (!send(client.fd, prefix)) continue;
      if (hold) {
        std::unique_lock lock(mutex);
        check(changed.wait_for(lock, 1000ms, [&] { return released || stopping.load(); }),
              "fixture response gate was not released");
        if (stopping) return;
      }
      (void)send(client.fd, suffix);
    }
  } catch (const std::exception& failure) {
    std::lock_guard lock(mutex);
    error = failure.what();
    changed.notify_all();
  }
};
c::ManualCloseResult complete(c::ManualCloseClient& client,
                              std::chrono::milliseconds maximum = 700ms) {
  const auto until = Clock::now() + maximum;
  while (Clock::now() < until) {
    auto state = client.status();
    if (state.state != c::ManualCloseState::pending) return state;
    (void)::poll(nullptr, 0, 1);
  }
  throw std::runtime_error("client completion exceeded its bounded deadline");
}
void configured_and_idle() {
  const auto open_files = []() {
    unsigned count = 0;
    for (int fd = 0; fd < 4096; ++fd)
      if (::fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
  };
  const auto before = open_files();
  std::vector<std::unique_ptr<c::ManualCloseClient>> unused;
  for (unsigned i = 0; i < 20; ++i)
    unused.push_back(std::make_unique<c::ManualCloseClient>());
  check(open_files() == before, "unconfigured clients allocated unused descriptors");
  unused.clear();
  check(open_files() == before, "unconfigured client destruction leaked resources");
  c::ManualCloseClient disabled;
  check(!disabled.configured() && disabled.status().state == c::ManualCloseState::idle,
        "default client was enabled");
  check(!disabled.request_close() && disabled.status().state == c::ManualCloseState::failed,
        "disabled close claimed acceptance");
  for (const auto& path :
       std::vector<std::string>{"", "relative", "/", "/private/./command", "/private/../command",
                                "/private//command", "/private/command/", "/private/command\n",
                                "/private/command\t", std::string("/private/command\0ignored", 24),
                                "/private/command" + std::string(1, '\x7f'),
                                "/private/command\xc3\xa9", "/" + std::string(103, 'x')})
    rejects([&] { c::ManualCloseClient invalid(path); });
  rejects([] { c::ManualCloseClient invalid(std::nullopt, 0); });
  rejects([] { c::ManualCloseClient invalid(std::nullopt, 501); });
  Directory dir;
  Server server(dir.path / "commands.sock");
  c::ManualCloseClient client(server.path.string());
  pollfd descriptor{server.listener.fd, POLLIN, 0};
  check(::poll(&descriptor, 1, 20) == 0 && server.count() == 0,
        "constructing/querying the client contacted a server");
  check(client.configured() && client.status().state == c::ManualCloseState::idle,
        "configured idle state was wrong");
  Server spaced(dir.path / "Close command ($).sock");
  c::ManualCloseClient printable(spaced.path.string());
  check(printable.request_close() && complete(printable).state == c::ManualCloseState::accepted,
        "a safe printable ASCII endpoint could not be used directly");
}
void partial_io_and_coalescing() {
  Directory dir;
  Server server(dir.path / "commands.sock", "\"message\":\"Close request accepted\"}\n", true,
                "{\"ok\":true,");
  c::ManualCloseClient client(server.path.string());
  check(client.request_close(), "close was not queued");
  server.wait_request();
  check(client.status().state == c::ManualCloseState::pending, "partial reply claimed a success");
  const auto began = Clock::now();
  for (unsigned i = 0; i < 128; ++i) {
    check(client.request_close(), "pending close was not coalesced");
    const auto state = client.status();
    check(state.state == c::ManualCloseState::pending && state.request_sequence == 1,
          "pending close queued unbounded work");
  }
  check(Clock::now() - began < 50ms, "render-facing request/query waited for socket I/O");
  check(server.count() == 1, "coalesced closes made extra connections");
  server.release();
  const auto result = complete(client);
  check(result.state == c::ManualCloseState::accepted && result.request_sequence == 1 &&
            result.message == "Close request accepted",
        "complete actual positive reply was not parsed");
  check(client.request_close(), "explicit second user action was rejected");
  server.wait_request(2);
  check(complete(client).state == c::ManualCloseState::accepted &&
            client.status().request_sequence == 2,
        "explicit retry reused a prior acknowledgement");
  check(server.count() == 2, "worker reconnected without an explicit user action");
}
void invalid_and_negative_replies() {
  for (const auto& bytes : std::vector<std::string>{
           "{\"ok\":false,\"error\":\"Reverse source is still requesting the camera\"}\n",
           "{\"ok\":true}\n", "{\"ok\":1,\"message\":\"Close\"}\n",
           "{\"ok\":true,\"error\":\"Close\"}\n", "{\"ok\":false,\"message\":\"Close\"}\n",
           "{\"ok\":true,\"message\":null}\n", "{\"ok\":true,\"message\":\"\"}\n",
           "{\"ok\":true,\"message\":\"Close\",\"withdrawn\":true}\n",
           "{\"ok\":true,\"ok\":false,\"message\":\"Close\"}\n", "[]\n", "true\n",
           "{\"ok\":true,\"message\":\"Close\"}",
           "{\"ok\":true,\"message\":\"Close\\u0000hidden\"}\n",
           std::string(accepted) + std::string(accepted),
           "{\"ok\":true,\"message\":\"" + std::string(1300, 'x') + "\"}\n",
           std::string(5000, 'x') + "\n", ""}) {
    Directory dir;
    Server server(dir.path / "commands.sock", bytes);
    c::ManualCloseClient client(server.path.string());
    check(client.request_close(), "malformed reply fixture was not queued");
    const auto result = complete(client);
    check(result.state == c::ManualCloseState::failed && result.message.size() <= 1200,
          "invalid/negative reply was reported as accepted");
    check(server.count() == 1, "malformed reply started a reconnect loop");
    if (bytes.starts_with("{\"ok\":false,\"error\":\"Reverse"))
      check(result.message == "Reverse source is still requesting the camera",
            "actual negative server reason was lost");
  }
}
void timeout_and_cancel() {
  Directory dir;
  Server server(dir.path / "commands.sock", std::string(accepted), true, "{\"ok\":true}");
  c::ManualCloseClient client(server.path.string(), 80);
  const auto began = Clock::now();
  check(client.request_close(), "timeout action was not queued");
  server.wait_request();
  check(client.status().state == c::ManualCloseState::pending, "unframed reply was accepted");
  const auto failed = complete(client, 300ms);
  check(failed.state == c::ManualCloseState::failed &&
            failed.message.find("timed out") != std::string::npos && Clock::now() - began < 250ms,
        "partial I/O renewed the total deadline");
  server.release();
  Directory framed_dir;
  Server framed(framed_dir.path / "commands.sock", "", true, std::string(accepted));
  c::ManualCloseClient awaiting_end(framed.path.string(), 80);
  check(awaiting_end.request_close(), "framed reply test was not queued");
  framed.wait_request();
  check(complete(awaiting_end, 300ms).state == c::ManualCloseState::failed,
        "a positive line before the complete connection reply claimed acceptance");
  framed.release();
  Directory cancellation_dir;
  Server silence(cancellation_dir.path / "commands.sock", std::string(accepted), true);
  auto cancelled = std::make_unique<c::ManualCloseClient>(silence.path.string());
  check(cancelled->request_close(), "cancel action was not queued");
  silence.wait_request();
  const auto start = Clock::now();
  cancelled->request_stop();
  check(complete(*cancelled, 100ms).state == c::ManualCloseState::failed,
        "cancelled request remained pending or claimed success");
  cancelled.reset();
  check(Clock::now() - start < 150ms, "shutdown blocked for a full socket deadline");
  silence.release();
}
void unsafe_endpoints() {
  for (const auto mode : {0, 1, 2, 3}) {
    Directory dir;
    Server server(dir.path / "commands.sock");
    fs::path selected = server.path;
    if (mode == 0)
      check(::chmod(server.path.c_str(), 0644) == 0, "unsafe socket mode fixture failed");
    if (mode == 1) check(::chmod(dir.path.c_str(), 0755) == 0, "unsafe parent fixture failed");
    if (mode == 2) {
      selected = dir.path / "socket-link";
      fs::create_symlink(server.path, selected);
    }
    if (mode == 3) {
      const auto link = dir.path / "parent-link";
      fs::create_directory_symlink(dir.path, link);
      selected = link / server.path.filename();
    }
    c::ManualCloseClient client(selected.string());
    check(client.request_close(), "unsafe action was not queued for rejection");
    check(complete(client).state == c::ManualCloseState::failed && server.count() == 0,
          "unsafe endpoint was contacted");
  }
  Directory dir;
  const auto path = dir.path / "regular.sock";
  {
    std::ofstream file(path);
    file << "not a socket";
  }
  check(::chmod(path.c_str(), 0600) == 0, "regular endpoint fixture mode failed");
  c::ManualCloseClient regular(path.string());
  check(regular.request_close() && complete(regular).state == c::ManualCloseState::failed,
        "regular file acted as command server");
  c::ManualCloseClient missing((dir.path / "missing.sock").string());
  check(missing.request_close() && complete(missing).state == c::ManualCloseState::failed,
        "missing endpoint was accepted");
}
void replaced_socket_generation() {
  Directory dir;
  Server old(dir.path / "commands.sock", std::string(accepted), true);
  c::ManualCloseClient client(old.path.string());
  check(client.request_close(), "replacement action was not queued");
  old.wait_request();
  check(::unlink(old.path.c_str()) == 0, "replacement fixture unlink failed");
  Server replacement(old.path);
  old.release();
  check(complete(client).state == c::ManualCloseState::failed,
        "replaced socket produced an accepted acknowledgement");
  check(client.request_close() && complete(client).state == c::ManualCloseState::failed &&
            replacement.count() == 0,
        "client adopted a new socket generation on retry");
  c::ManualCloseClient new_owner(replacement.path.string());
  check(new_owner.request_close() && complete(new_owner).state == c::ManualCloseState::accepted,
        "fresh client could not use the new qualified owner");
  Directory ancestor_dir;
  const auto branch = ancestor_dir.path / "branch", renamed = ancestor_dir.path / "original";
  fs::create_directory(branch);
  check(::chmod(branch.c_str(), 0700) == 0, "ancestor runtime permissions failed");
  const auto private_parent = branch / "private";
  fs::create_directory(private_parent);
  check(::chmod(private_parent.c_str(), 0700) == 0, "inner runtime permissions failed");
  Server unchanged(private_parent / "commands.sock");
  c::ManualCloseClient redirected(unchanged.path.string());
  check(redirected.request_close() && complete(redirected).state == c::ManualCloseState::accepted,
        "qualified nested endpoint failed");
  fs::rename(branch, renamed);
  fs::create_directory_symlink(renamed, branch);
  check(redirected.request_close() && complete(redirected).state == c::ManualCloseState::failed &&
            unchanged.count() == 1,
        "client accepted a redirected ancestor after its initial qualification");
}
}  // namespace
int main() try {
  configured_and_idle();
  partial_io_and_coalescing();
  invalid_and_negative_replies();
  timeout_and_cancel();
  unsafe_endpoints();
  replaced_socket_generation();
  std::cout << "Manual camera Close client: six actual Unix-server groups passed (no "
               "camera-withdrawal or hardware claim)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
