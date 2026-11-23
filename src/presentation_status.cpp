// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/presentation_status.hpp>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace headunit {
namespace {
constexpr std::uint64_t heartbeat_ns = 250000000, snapshot_age_ns = 1000000000;
constexpr std::size_t bound = 512;
std::atomic<std::uint64_t> temporary_sequence{0};
void require(bool ok, const char* reason) {
  if (!ok) throw std::runtime_error(reason);
}
struct File {
  int fd = -1;
  explicit File(int value = -1) : fd(value) {}
  ~File() {
    if (fd >= 0) (void)::close(fd);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
};
bool same(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool directory(const struct stat& s) {
  return S_ISDIR(s.st_mode) && s.st_uid == getuid() && (s.st_mode & 07777) == 0700;
}
bool regular(const struct stat& s, std::size_t maximum) {
  return S_ISREG(s.st_mode) && s.st_uid == getuid() && (s.st_mode & 07777) == 0600 &&
         s.st_nlink == 1 && s.st_size >= 0 && std::uint64_t(s.st_size) <= maximum;
}
std::string_view state_name(PresentationState state) {
  switch (state) {
    case PresentationState::disconnected:
      return "disconnected";
    case PresentationState::starting:
      return "starting";
    case PresentationState::ready:
      return "ready";
    case PresentationState::home:
      return "home";
    case PresentationState::fault:
      return "fault";
    case PresentationState::stopped:
      return "stopped";
  }
  throw std::invalid_argument("unknown presentation state");
}
std::string_view mode_name(CarplayMode mode) {
  switch (mode) {
    case CarplayMode::standard:
      return "standard";
    case CarplayMode::ultra:
      return "ultra";
  }
  throw std::invalid_argument("unknown CarPlay mode");
}
}  // namespace
std::uint64_t presentation_monotonic_ns() {
  timespec value{};
  require(::clock_gettime(CLOCK_MONOTONIC, &value) == 0 && value.tv_sec >= 0 &&
              value.tv_nsec >= 0 && value.tv_nsec < 1000000000,
          "presentation monotonic clock unavailable");
  const auto seconds = std::uint64_t(value.tv_sec);
  require(seconds <= std::numeric_limits<std::uint64_t>::max() / 1000000000,
          "presentation monotonic clock overflow");
  const auto base = seconds * 1000000000;
  require(std::uint64_t(value.tv_nsec) <= std::numeric_limits<std::uint64_t>::max() - base,
          "presentation monotonic clock overflow");
  return base + std::uint64_t(value.tv_nsec);
}
std::uint64_t MainPresentationSnapshot::begin() {
  std::lock_guard lock(mutex_);
  require(generation_ != std::numeric_limits<std::uint64_t>::max(),
          "presentation generation exhausted");
  ++generation_;
  active_ = true;
  observed_ns_ = 0;
  presented_ = false;
  value_ = {};
  return generation_;
}
void MainPresentationSnapshot::observe(std::uint64_t generation, bool visible, bool home,
                                       bool healthy, std::uint64_t surface_restorations) {
  const auto now = presentation_monotonic_ns();
  std::lock_guard lock(mutex_);
  if (!generation || generation != generation_ || !active_) return;
  if (observed_ns_ && now < observed_ns_) {
    value_ = {};
    presented_ = false;
    return;
  }
  observed_ns_ = now;
  if (visible && healthy && !home) presented_ = true;
  // A clock/output fault destroys prior eligibility; recovery needs another
  // genuine receipt, rather than a refreshed observation of an old scene.
  if (!healthy) presented_ = false;
  value_ = {healthy && (visible || (home && presented_)), healthy, home, surface_restorations};
}
void MainPresentationSnapshot::retire(std::uint64_t generation) noexcept {
  std::lock_guard lock(mutex_);
  if (generation == generation_) {
    active_ = false;
    observed_ns_ = 0;
    presented_ = false;
    value_ = {};
  }
}
MainPresentationStatus MainPresentationSnapshot::sample() const {
  // Observe the genuine clock after the bounded snapshot read/lock. An owner
  // publishing between a pre-read clock query and the lock is not a fault.
  std::lock_guard lock(mutex_);
  const auto now = presentation_monotonic_ns();
  if (!observed_ns_ || now < observed_ns_ || now - observed_ns_ > snapshot_age_ns) return {};
  return value_;
}

struct PresentationStatusPublisher::Impl {
  std::string parent, name, owner_name, nonce;
  File parent_fd, owner_fd;
  struct stat parent_identity{}, owner_identity{};
  std::uint64_t sequence = 0, observed = 0, generation = 0;
  PresentationState state = PresentationState::disconnected;
  bool ready = false;
  std::optional<CarplayMode> mode;
  std::uint64_t home_requests = 0;

  void inspect(const std::string& leaf, std::size_t maximum, bool optional) const {
    struct stat info{};
    if (::fstatat(parent_fd.fd, leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
      require(optional && errno == ENOENT, "presentation endpoint cannot be inspected");
      return;
    }
    require(regular(info, maximum),
            "presentation endpoint must be bounded owned single-link 0600 regular data");
  }
  void verify() const {
    struct stat named{}, opened{}, owner{}, held{};
    require(::lstat(parent.c_str(), &named) == 0 && directory(named) &&
                same(named, parent_identity) && ::fstat(parent_fd.fd, &opened) == 0 &&
                directory(opened) && same(opened, parent_identity),
            "presentation parent generation changed");
    require(::fstatat(parent_fd.fd, owner_name.c_str(), &owner, AT_SYMLINK_NOFOLLOW) == 0 &&
                regular(owner, 0) && same(owner, owner_identity) &&
                ::fstat(owner_fd.fd, &held) == 0 && regular(held, 0) && same(held, owner_identity),
            "presentation publisher owner generation changed");
  }
  Impl(std::string path, std::string token) : nonce(std::move(token)) {
    require(nonce.size() == 32 && nonce.find_first_not_of("0123456789abcdef") == nonce.npos,
            "presentation nonce must be 32 lowercase hexadecimal characters");
    require(
        path.size() <= 4000 && path.starts_with('/') && !path.ends_with('/') &&
            path.find_first_not_of(
                "/abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == path.npos,
        "presentation endpoint requires a bounded canonical absolute path");
    for (const auto& part : std::filesystem::path(path))
      require(part != "." && part != "..", "presentation endpoint cannot traverse dot components");
    require(path.find("//") == path.npos, "presentation endpoint must be canonical");
    const std::filesystem::path selected(path);
    parent = selected.parent_path().string();
    name = selected.filename().string();
    owner_name = name + ".owner";
    require(std::filesystem::canonical(parent) == std::filesystem::path(parent),
            "presentation parent must have real canonical ancestors");
    require(!name.empty() && owner_name.size() <= 255, "presentation endpoint name exceeds bounds");
    struct stat before{};
    require(::lstat(parent.c_str(), &before) == 0 && directory(before),
            "presentation parent must be a real owned 0700 directory");
    parent_fd.fd =
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    require(parent_fd.fd >= 0 && ::fstat(parent_fd.fd, &parent_identity) == 0 &&
                directory(parent_identity) && same(before, parent_identity),
            "presentation parent changed during open");
    for (unsigned count = 0; count < 16 && owner_fd.fd < 0; ++count) {
      inspect(owner_name, 0, true);
      owner_fd.fd = ::openat(parent_fd.fd, owner_name.c_str(),
                             O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
      if (owner_fd.fd < 0 && errno == EEXIST) {
        owner_fd.fd = ::openat(parent_fd.fd, owner_name.c_str(),
                               O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
      } else if (owner_fd.fd < 0 && errno == EINTR)
        continue;
      require(owner_fd.fd >= 0, "presentation owner could not be opened");
      require(::fstat(owner_fd.fd, &owner_identity) == 0 && regular(owner_identity, 0),
              "presentation owner is unsafe");
    }
    require(owner_fd.fd >= 0 && ::flock(owner_fd.fd, LOCK_EX | LOCK_NB) == 0,
            "presentation publisher already owned");
    verify();
    inspect(name, bound, true);
    publish(PresentationState::disconnected, 0, false, {}, true);
  }
  void publish(PresentationState next, std::uint64_t connection, bool eligible,
               std::optional<CarplayMode> next_mode = {}, bool forced = false,
               std::uint64_t next_home_requests = 0) {
    (void)state_name(next);
    if (next == PresentationState::disconnected || next == PresentationState::stopped)
      next_mode.reset();
    if (next_mode) {
      (void)mode_name(*next_mode);
      require(connection != 0, "negotiated mode needs a connection");
    }
    require(!eligible || ((next == PresentationState::ready || next == PresentationState::home) &&
                          connection),
            "presentation readiness needs a current presented connection");
    require((next != PresentationState::ready && next != PresentationState::home) ||
                (eligible && connection),
            "presented states require qualified readiness");
    require(connection >= generation, "presentation connection generation reversed");
    const auto now = presentation_monotonic_ns();
    require(now > 0 && (!observed || now >= observed), "presentation clock reversed");
    if (!forced && next == state && connection == generation && eligible == ready &&
        next_mode == mode && next_home_requests == home_requests && observed &&
        now - observed < heartbeat_ns)
      return;
    require(sequence != std::numeric_limits<std::uint64_t>::max(),
            "presentation publication sequence exhausted");
    const auto bytes = "{\"version\":3,\"nonce\":\"" + nonce + "\",\"sequence\":\"" +
                       std::to_string(sequence + 1) + "\",\"connectionGeneration\":\"" +
                       std::to_string(connection) + "\",\"updatedMonotonicNs\":\"" +
                       std::to_string(now) + "\",\"state\":\"" + std::string(state_name(next)) +
                       "\",\"returnReady\":" + (eligible ? "true" : "false") +
                       ",\"negotiatedMode\":" +
                       (next_mode ? "\"" + std::string(mode_name(*next_mode)) + "\"" : "null") +
                       ",\"phoneHomeRequests\":\"" + std::to_string(next_home_requests) + "\"}\n";
    require(bytes.size() <= bound, "presentation publication exceeds bound");
    verify();
    inspect(name, bound, true);
    const auto temporary =
        "." + name + "." + std::to_string(getpid()) + "." + std::to_string(++temporary_sequence);
    File pending(::openat(parent_fd.fd, temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    require(pending.fd >= 0, "presentation temporary file could not be created");
    try {
      struct stat created{};
      require(::fstat(pending.fd, &created) == 0 && regular(created, 0),
              "presentation temporary file is unsafe");
      std::size_t offset = 0;
      for (unsigned turns = 0; turns < 32 && offset < bytes.size(); ++turns) {
        const auto written = ::write(pending.fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0)
          throw std::system_error(written < 0 ? errno : EIO, std::generic_category(),
                                  "presentation publication write failed");
        offset += std::size_t(written);
      }
      require(offset == bytes.size(), "presentation publication write exceeded retry bound");
      // This is an ephemeral heartbeat. Atomic rename supplies visibility;
      // durable fsync on every heartbeat would add eMMC writes and UI latency.
      verify();
      inspect(name, bound, true);
      require(::renameat(parent_fd.fd, temporary.c_str(), parent_fd.fd, name.c_str()) == 0,
              "presentation publication rename failed");
      verify();
      inspect(name, bound, false);
    } catch (...) {
      (void)::unlinkat(parent_fd.fd, temporary.c_str(), 0);
      throw;
    }
    ++sequence;
    observed = now;
    state = next;
    generation = connection;
    ready = eligible;
    mode = next_mode;
    home_requests = next_home_requests;
  }
};
PresentationStatusPublisher::PresentationStatusPublisher(std::string path, std::string nonce)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(nonce))) {}
PresentationStatusPublisher::~PresentationStatusPublisher() {
  if (impl_) try {
      impl_->publish(PresentationState::stopped, impl_->generation, false, {}, true);
    } catch (...) {
    }
}
void PresentationStatusPublisher::update(PresentationState state, std::uint64_t generation,
                                         bool ready, std::optional<CarplayMode> mode,
                                         std::uint64_t phone_home_requests) {
  impl_->publish(state, generation, ready, mode, false, phone_home_requests);
}
}  // namespace headunit
