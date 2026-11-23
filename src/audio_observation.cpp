// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_observation.hpp"
#include <algorithm>
#include <charconv>
#include <csignal>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

extern char** environ;
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit::audio_observation {
Capture::Capture(std::uint64_t budget) : remaining_(budget) {
  if (!budget || budget > maximum_budget) throw std::invalid_argument("audio trace budget invalid");
}
std::uint64_t Capture::identity() noexcept {
  return identities_.fetch_add(1, std::memory_order_relaxed) + 1;
}
void Capture::record(Event event, bool lifecycle) noexcept {
  if (stopped_.load(std::memory_order_relaxed)) return;
  std::unique_lock lock(mutex_, std::defer_lock);
  if (!lock.try_lock()) {
    contention_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (stopped_.load(std::memory_order_relaxed)) return;
  if (lifecycle) {
    // Never overwrite an earlier retirement or reset receipt.
    if (lifecycle_head_ + lifecycle_count_ == lifecycle_capacity) {
      ++counts_.lifecycle_overflow;
      return;
    }
    lifecycle_[lifecycle_head_ + lifecycle_count_++] = event;
  } else {
    if (!remaining_) {
      ++counts_.budget;
      return;
    }
    --remaining_;
    if (count_ == capacity) {
      ++counts_.overflow;
      return;
    }
    events_[(head_ + count_++) % capacity] = event;
  }
  ++counts_.recorded;
}
std::size_t Capture::take(std::span<Event> destination) {
  std::lock_guard lock(mutex_);
  std::size_t copied = 0;
  while (copied < destination.size() && lifecycle_count_) {
    destination[copied++] = lifecycle_[lifecycle_head_++];
    --lifecycle_count_;
  }
  while (copied < destination.size() && count_) {
    destination[copied++] = events_[head_];
    head_ = (head_ + 1) % capacity;
    --count_;
  }
  return copied;
}
Counters Capture::counters() const {
  std::lock_guard lock(mutex_);
  auto result = counts_;
  result.contention = contention_.load(std::memory_order_relaxed);
  return result;
}
void Capture::stop() noexcept {
  std::lock_guard lock(mutex_);
  stopped_.store(true, std::memory_order_relaxed);
}
void Point::observe(Kind kind, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                    bool lifecycle) const noexcept {
  if (!capture) return;
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
  capture->record({std::uint64_t(now.tv_sec) * 1000000000 + std::uint64_t(now.tv_nsec), kind,
                   source, route, epoch, a, b, c, d},
                  lifecycle);
}
namespace {
constexpr std::size_t batch_events = 64;
constexpr int socket_bytes = 32768;
constexpr std::uint64_t ipc_version = 1;
struct Packet {
  std::uint64_t version = ipc_version, final = 0, count = 0;
  Counters capture;
  std::uint64_t dropped_events = 0, dropped_batches = 0;
  std::array<Event, batch_events> events{};
};
struct Reply {
  std::uint64_t version = ipc_version;
  File::State state = File::State::starting;
  File::Export exported;
};
static_assert(sizeof(Packet) < socket_bytes / 4);
std::uint64_t now_ns() noexcept {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
  return std::uint64_t(now.tv_sec) * 1000000000 + std::uint64_t(now.tv_nsec);
}
std::string encode(std::span<const Event> events) {
  std::string batch;
  batch.reserve(events.size() * 200);
  for (const auto& e : events) {
    batch += "[" + std::to_string(e.date_ns) + "," + std::to_string(std::uint64_t(e.kind)) + "," +
             std::to_string(e.source) + "," + std::to_string(e.route) + "," +
             std::to_string(e.epoch) + "," + std::to_string(e.a) + "," + std::to_string(e.b) + "," +
             std::to_string(e.c) + "," + std::to_string(e.d) + "]\n";
  }
  return batch;
}
int private_file(const std::string& path) {
  const std::filesystem::path target(path);
  const auto parent = target.parent_path();
  const auto name = target.filename().string();
  const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0) return -1;
  struct stat state{};
  if (fstat(directory, &state) || state.st_uid != getuid() || (state.st_mode & 0777) != 0700) {
    close(directory);
    errno = EACCES;
    return -1;
  }
  const int fd =
      openat(directory, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  const int error = errno;
  close(directory);
  errno = error;
  return fd;
}
void reply(int socket, const Reply& state) noexcept {
  (void)send(socket, &state, sizeof(state), MSG_DONTWAIT | MSG_NOSIGNAL);
}
bool write_file(int fd, std::string_view bytes, File::Export& measured) noexcept {
  const auto start = now_ns();
  const auto result = ::write(fd, bytes.data(), bytes.size());
  const int error = errno;
  measured.write_ns += now_ns() - start;
  ++measured.write_calls;
  if (result > 0) measured.written_bytes += std::uint64_t(result);
  if (result != static_cast<ssize_t>(bytes.size())) {
    measured.partial_bytes = result > 0 ? std::uint64_t(result) : 0;
    measured.error = result < 0 ? std::uint64_t(error) : EIO;
    return false;
  }
  return true;
}
int collect(int socket, const std::string& path, pid_t parent) noexcept {
#ifdef __linux__
  // A parent crash must never leave a surviving userspace logger.
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) return 1;
#else
  if (getppid() != parent) return 1;
#endif
  (void)signal(SIGXFSZ, SIG_IGN);
  Reply state;
  int fd = -1;
  try {
    // The existing receiver group and service cgroup can both receive soft
    // shutdown signals. Only the parent owns EOF, kill and reap for this
    // storage child. Spawn blocked these signals before exec; install ignore
    // dispositions before unblocking them, so startup has no signal race.
    sigset_t soft_stop;
    if (sigemptyset(&soft_stop)) throw std::runtime_error("collector signal set unavailable");
    for (const auto signum : {SIGINT, SIGTERM, SIGHUP}) {
      if (sigaddset(&soft_stop, signum) || std::signal(signum, SIG_IGN) == SIG_ERR)
        throw std::runtime_error("collector signal admission failed");
    }
    if (sigprocmask(SIG_UNBLOCK, &soft_stop, nullptr))
      throw std::runtime_error("collector signal unblock failed");
    fd = private_file(path);
    if (fd < 0) {
      state.exported.error = errno;
      throw std::runtime_error("private open failed");
    }
    int sender_bytes = 0, receiver_bytes = 0;
    socklen_t length = sizeof(sender_bytes);
    if (getsockopt(socket, SOL_SOCKET, SO_SNDBUF, &sender_bytes, &length) ||
        getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &receiver_bytes, &length)) {
      state.exported.error = errno;
      throw std::runtime_error("collector buffer observation failed");
    }
    if (sender_bytes > 2 * socket_bytes || receiver_bytes > 2 * socket_bytes ||
        sender_bytes < int(sizeof(Packet)) || receiver_bytes < int(sizeof(Packet))) {
      state.exported.error = EOVERFLOW;
      throw std::runtime_error("collector socket buffers outside bound");
    }
    const auto header =
        "{\"schema\":2,\"clock\":\"CLOCK_MONOTONIC\",\"unavailable\":18446744073709551615,"
        "\"collectorPid\":" +
        std::to_string(getpid()) + ",\"parentPid\":" + std::to_string(parent) +
        ",\"socketSendBufferBytes\":" + std::to_string(sender_bytes) +
        ",\"socketReceiveBufferBytes\":" + std::to_string(receiver_bytes) +
        ",\"columns\":[\"dateNs\",\"kind\",\"source\",\"route\",\"epoch\",\"a\",\"b\",\"c\",\"d\"]}"
        "\n";
    if (!write_file(fd, header, state.exported)) throw std::runtime_error("header write failed");
    state.state = File::State::running;
    reply(socket, state);
    for (;;) {
      pollfd ready{socket, POLLIN, 0};
      const int result = ::poll(&ready, 1, 100);
      if (getppid() != parent) throw std::runtime_error("parent retired");
      if (result < 0 && errno == EINTR) continue;
      if (result < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        state.exported.error = result < 0 ? errno : EPIPE;
        throw std::runtime_error("collector transport failed");
      }
      if (!(ready.revents & POLLIN)) continue;
      Packet packet;
      const auto size = recv(socket, &packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
      if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      if (size != static_cast<ssize_t>(sizeof(packet)) || packet.version != ipc_version ||
          packet.count > batch_events || packet.final > 1) {
        state.exported.error = EPROTO;
        throw std::runtime_error("collector packet invalid");
      }
      if (!packet.final) {
        if (state.exported.written_events + packet.count >
            Capture::maximum_budget + Capture::lifecycle_capacity) {
          state.exported.error = EOVERFLOW;
          throw std::runtime_error("collector finite budget exceeded");
        }
        const auto start = now_ns();
        const auto batch = encode(std::span(packet.events).first(packet.count));
        state.exported.encoding_ns += now_ns() - start;
        if (!write_file(fd, batch, state.exported)) throw std::runtime_error("batch write failed");
        state.exported.written_events += packet.count;
        reply(socket, state);
        continue;
      }
      const auto& c = packet.capture;
      const auto footer = "{\"recorded\":" + std::to_string(c.recorded) +
                          ",\"contention\":" + std::to_string(c.contention) +
                          ",\"overflow\":" + std::to_string(c.overflow) +
                          ",\"budget\":" + std::to_string(c.budget) +
                          ",\"lifecycleOverflow\":" + std::to_string(c.lifecycle_overflow) +
                          ",\"transportDroppedEvents\":" + std::to_string(packet.dropped_events) +
                          ",\"transportDroppedBatches\":" + std::to_string(packet.dropped_batches) +
                          ",\"writtenEvents\":" + std::to_string(state.exported.written_events) +
                          ",\"encodingNs\":" + std::to_string(state.exported.encoding_ns) +
                          ",\"writeNs\":" + std::to_string(state.exported.write_ns) +
                          ",\"writeCalls\":" + std::to_string(state.exported.write_calls) +
                          ",\"writtenBytes\":" + std::to_string(state.exported.written_bytes) +
                          ",\"finished\":true}\n";
      if (!write_file(fd, footer, state.exported)) throw std::runtime_error("footer write failed");
      state.state = File::State::finished;
      reply(socket, state);
      close(fd);
      close(socket);
      return 0;
    }
  } catch (...) {
    if (!state.exported.error) state.exported.error = EIO;
    state.state = File::State::failed;
    reply(socket, state);
    // Only this storage process writes diagnostics, including its error report.
    const auto error =
        "Private audio collector failed error=" + std::to_string(state.exported.error) +
        " writtenEvents=" + std::to_string(state.exported.written_events) +
        " partialBytes=" + std::to_string(state.exported.partial_bytes) + "\n";
    (void)::write(STDERR_FILENO, error.data(), error.size());
    if (fd >= 0) close(fd);
    close(socket);
    return 1;
  }
}
}  // namespace
struct File::Impl {
  std::shared_ptr<Capture> capture = std::make_shared<Capture>();
  int socket = -1;
  Status status;
  bool final_sent = false, closed = false, cancelled = false;
  int exit_status = -1;
  Impl(const std::string& path, const std::string& executable) {
    const std::filesystem::path target(path);
    const auto name = target.filename().string();
    if (path.size() > 4096 || !target.is_absolute() || name.empty() || name == "." || name == "..")
      throw std::invalid_argument("audio trace requires a new absolute private file");
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair))
      throw std::runtime_error("audio collector socket unavailable");
    const auto close_pair = [&] {
      close(pair[0]);
      close(pair[1]);
    };
    for (const auto fd : pair) {
      if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK) ||
          setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_bytes, sizeof(socket_bytes)) ||
          setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_bytes, sizeof(socket_bytes))) {
        close_pair();
        throw std::runtime_error("audio collector socket configuration failed");
      }
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
      close_pair();
      throw std::runtime_error("audio collector spawn actions unavailable");
    }
    int action_error = posix_spawn_file_actions_addclose(&actions, pair[0]);
    if (!action_error) action_error = posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
    if (!action_error && pair[1] != 3)
      action_error = posix_spawn_file_actions_addclose(&actions, pair[1]);
    if (action_error) {
      posix_spawn_file_actions_destroy(&actions);
      close_pair();
      throw std::runtime_error("audio collector descriptor transfer failed");
    }
    const auto parent = std::to_string(getpid());
    std::array<char*, 6> arguments{const_cast<char*>(executable.c_str()),
                                   const_cast<char*>("--audio-observation-collector"),
                                   const_cast<char*>("3"),
                                   const_cast<char*>(path.c_str()),
                                   const_cast<char*>(parent.c_str()),
                                   nullptr};
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes)) {
      posix_spawn_file_actions_destroy(&actions);
      close_pair();
      throw std::runtime_error("collector spawn attributes unavailable");
    }
    sigset_t child_mask;
    int attribute_error = sigprocmask(SIG_BLOCK, nullptr, &child_mask) ? errno : 0;
    for (const auto signum : {SIGINT, SIGTERM, SIGHUP}) {
      if (!attribute_error && sigaddset(&child_mask, signum)) attribute_error = errno;
    }
    if (!attribute_error) attribute_error = posix_spawnattr_setsigmask(&attributes, &child_mask);
    if (!attribute_error)
      attribute_error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
    if (attribute_error) {
      posix_spawnattr_destroy(&attributes);
      posix_spawn_file_actions_destroy(&actions);
      close_pair();
      throw std::runtime_error("collector startup signal mask unavailable");
    }
    pid_t child;
    const int spawned =
        posix_spawn(&child, executable.c_str(), &actions, &attributes, arguments.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned) {
      close_pair();
      throw std::runtime_error("audio collector spawn failed");
    }
    close(pair[1]);
    socket = pair[0];
    status.collector_pid = child;
  }
  void receive() noexcept {
    for (unsigned i = 0; i < 16; ++i) {
      Reply reply;
      const auto size = recv(socket, &reply, sizeof(reply), MSG_DONTWAIT | MSG_TRUNC);
      if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (size < 0 && errno == EINTR) continue;
      if (size != static_cast<ssize_t>(sizeof(reply)) || reply.version != ipc_version) break;
      status.exporter = reply.exported;
      // Progress queued before a parent transport failure cannot revive it or
      // authorize a footer while accepted records remain in the local ring.
      if (!cancelled && status.state != State::failed) status.state = reply.state;
    }
  }
  void reap() noexcept {
    if (status.reaped) return;
    int result;
    const auto waited = waitpid(status.collector_pid, &result, WNOHANG);
    if (waited == status.collector_pid) {
      status.reaped = true;
      exit_status = WIFEXITED(result) ? WEXITSTATUS(result) : 128 + WTERMSIG(result);
      if (!cancelled)
        status.state = exit_status == 0 && final_sent ? State::finished : State::failed;
    }
  }
  bool send(Packet& packet) noexcept {
    const auto result = ::send(socket, &packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL);
    const int error = errno;
    if (result == static_cast<ssize_t>(sizeof(packet))) return true;
    status.transport_dropped_events += packet.count;
    ++status.transport_dropped_batches;
    // Darwin reports ENOBUFS for a full Unix datagram socket. The removed
    // batch is rejected diagnostics, just as with EAGAIN; keep draining the
    // sealed ring within the existing finite poll bound.
    if (!(result < 0 &&
          (error == EAGAIN || error == EWOULDBLOCK || error == EINTR || error == ENOBUFS))) {
      status.transport_error = result < 0 ? error : EIO;
      status.state = State::failed;
      capture->stop();
    }
    return false;
  }
  bool poll() noexcept {
    if (closed) return status.state == State::finished;
    receive();
    reap();
    if (status.state == State::failed || status.state == State::finished || status.reaped) {
      capture->stop();
      return false;
    }
    for (unsigned i = 0; i < 8; ++i) {
      Packet packet;
      packet.count = capture->take(packet.events);
      if (!packet.count) break;
      (void)send(packet);
      if (status.state == State::failed) return false;
    }
    return true;
  }
  bool finish() noexcept {
    if (closed) return status.state == State::finished;
    capture->stop();
    for (unsigned i = 0; i < (Capture::capacity + Capture::lifecycle_capacity + 511) / 512; ++i)
      if (!poll()) break;
    if (!status.reaped && status.state != State::failed) {
      Packet final;
      final.final = 1;
      final.capture = capture->counters();
      final.dropped_events = status.transport_dropped_events;
      final.dropped_batches = status.transport_dropped_batches;
      final_sent = send(final);
    }
    const auto wait_until = [&](std::uint64_t deadline) {
      do {
        receive();
        reap();
        if (status.reaped) break;
        if (!cancelled && !final_sent && status.state != State::failed) {
          Packet final;
          final.final = 1;
          final.capture = capture->counters();
          final.dropped_events = status.transport_dropped_events;
          final.dropped_batches = status.transport_dropped_batches;
          final_sent = send(final);
        }
        pollfd ready{socket, POLLIN, 0};
        (void)::poll(&ready, 1, 1);
      } while (now_ns() < deadline);
    };
    wait_until(now_ns() + 100000000);
    if (!status.reaped) {
      cancelled = true;
      status.state = State::cancelled;
      (void)kill(status.collector_pid, SIGKILL);
      wait_until(now_ns() + 100000000);
    }
    if (status.reaped) {
      // Reaping fixes the remaining reply set. Drain its bounded socket buffer
      // so a final receipt cannot remain behind older progress snapshots.
      for (std::size_t i = 0; i <= 2 * socket_bytes / sizeof(Reply); i += 16)
        receive();
    }
    close(socket);
    socket = -1;
    closed = true;
    return status.reaped && exit_status == 0 && final_sent;
  }
};
File::File(const std::string& path, const std::string& executable)
    : impl_(std::make_unique<Impl>(path, executable)) {}
File::~File() {
  (void)finish();
}
std::shared_ptr<Capture> File::capture() const {
  return impl_->capture;
}
bool File::poll() noexcept {
  return impl_->poll();
}
bool File::finish() noexcept {
  return impl_->finish();
}
File::Status File::status() const noexcept {
  return impl_->status;
}
std::optional<int> File::collector_main(int argc, char** argv) noexcept {
  if (argc < 2 || std::string_view(argv[1]) != "--audio-observation-collector") return std::nullopt;
  if (argc != 5 || std::string_view(argv[2]) != "3") return 2;
  pid_t parent = 0;
  const std::string_view value(argv[4]);
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parent);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parent <= 1)
    return 2;
  return collect(3, argv[3], parent);
}
}  // namespace headunit::audio_observation
