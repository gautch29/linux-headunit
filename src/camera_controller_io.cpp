// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_controller_io.hpp>
#include <headunit/camera.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace headunit::camera {
ControlPhase source_control_phase(bool requested, Availability availability) {
  if (!requested) return ControlPhase::idle;
  switch (availability) {
    case Availability::ready:
      return ControlPhase::active;
    case Availability::closed:
    case Availability::clock_reversed:
      return ControlPhase::fault;
    case Availability::missing:
    case Availability::stale:
      return ControlPhase::waiting;
  }
  return ControlPhase::fault;
}
namespace {
constexpr std::size_t request_bound = 24, status_bound = 128;
constexpr unsigned io_turns = 64;
constexpr unsigned snapshot_attempts = 4;
std::atomic<std::uint64_t> temporary_sequence{0};
void require(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
void io(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
struct File {
  int fd = -1;
  explicit File(int value = -1) : fd(value) {}
  ~File() {
    if (fd >= 0) (void)::close(fd);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
  File& operator=(File&& other) noexcept {
    if (this != &other) {
      if (fd >= 0) (void)::close(fd);
      fd = std::exchange(other.fd, -1);
    }
    return *this;
  }
};
bool same(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool private_directory(const struct stat& s) {
  return S_ISDIR(s.st_mode) && s.st_uid == getuid() && (s.st_mode & 07777) == 0700;
}
bool private_regular(const struct stat& s, std::size_t maximum) {
  return S_ISREG(s.st_mode) && s.st_uid == getuid() && (s.st_mode & 07777) == 0600 &&
         s.st_size >= 0 && std::uint64_t(s.st_size) <= maximum;
}
bool private_file(const struct stat& s, std::size_t maximum) {
  return private_regular(s, maximum) && s.st_nlink == 1;
}
bool same_content(const struct stat& before, const struct stat& after) {
#ifdef __APPLE__
  const auto& bm = before.st_mtimespec;
  const auto& am = after.st_mtimespec;
#else
  const auto& bm = before.st_mtim;
  const auto& am = after.st_mtim;
#endif
  return same(before, after) && before.st_size == after.st_size && bm.tv_sec == am.tv_sec &&
         bm.tv_nsec == am.tv_nsec;
}
bool stable(const struct stat& before, const struct stat& after) {
#ifdef __APPLE__
  const auto& bc = before.st_ctimespec;
  const auto& ac = after.st_ctimespec;
#else
  const auto& bc = before.st_ctim;
  const auto& ac = after.st_ctim;
#endif
  return same_content(before, after) && bc.tv_sec == ac.tv_sec && bc.tv_nsec == ac.tv_nsec;
}
struct PrivatePath {
  std::string parent, name;
  File directory;
  struct stat identity{};
  explicit PrivatePath(const std::string& value) {
    const std::filesystem::path path(value);
    require(!value.empty() && value.size() <= 4000 && value.find('\0') == value.npos &&
                path.is_absolute(),
            "camera controller endpoint requires a bounded absolute path");
    for (const auto& part : path)
      require(part != "." && part != "..",
              "camera controller endpoint cannot traverse dot components");
    parent = path.parent_path().string();
    name = path.filename().string();
    require(!name.empty() && name.size() <= 255,
            "camera controller endpoint must name one bounded file");
    struct stat before{};
    require(::lstat(parent.c_str(), &before) == 0 && private_directory(before),
            "camera controller parent must be a real owned 0700 directory");
    directory.fd =
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    io(directory.fd >= 0, "camera controller parent could not be opened");
    require(::fstat(directory.fd, &identity) == 0 && private_directory(identity) &&
                same(before, identity),
            "camera controller parent changed during open");
  }
  void verify() const {
    struct stat named{}, opened{};
    require(::lstat(parent.c_str(), &named) == 0 && private_directory(named) &&
                same(identity, named) && ::fstat(directory.fd, &opened) == 0 &&
                private_directory(opened) && same(identity, opened),
            "camera controller parent generation changed");
  }
  // A concurrent rename unlinks the replaced inode before the pathname resolves
  // to its successor; readers may observe that unlinked inode, never an alias.
  std::optional<struct stat> inspect(std::string_view leaf, std::size_t maximum,
                                     bool replacing = false) const {
    struct stat s{};
    const std::string name_string(leaf);
    if (::fstatat(directory.fd, name_string.c_str(), &s, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) return {};
      throw std::runtime_error("camera controller endpoint cannot be inspected");
    }
    require(private_file(s, maximum) || (replacing && private_regular(s, maximum) && !s.st_nlink),
            "camera controller endpoint must be bounded owned single-link 0600 regular data");
    return s;
  }
};
void distinct(const PrivatePath& target, const PrivatePath& owner) {
  require(!same(target.identity, owner.identity) || target.name != owner.name,
          "camera controller data and owner endpoints alias");
  struct stat a{}, b{};
  const bool have_a =
      ::fstatat(target.directory.fd, target.name.c_str(), &a, AT_SYMLINK_NOFOLLOW) == 0;
  if (!have_a) require(errno == ENOENT, "camera controller data endpoint cannot be inspected");
  const bool have_b =
      ::fstatat(owner.directory.fd, owner.name.c_str(), &b, AT_SYMLINK_NOFOLLOW) == 0;
  if (!have_b) require(errno == ENOENT, "camera controller owner endpoint cannot be inspected");
  require(!have_a || !have_b || !same(a, b), "camera controller data and owner inode alias");
}
struct Snapshot {
  std::string bytes;
  struct stat identity{};
};
void validate_snapshot(std::string_view bytes, std::size_t maximum);
std::optional<Snapshot> read_snapshot(const PrivatePath& path, std::size_t maximum) {
  for (unsigned attempt = 0; attempt < snapshot_attempts; ++attempt) {
    path.verify();
    const auto named = path.inspect(path.name, maximum, true);
    if (!named) return {};
    // Inspect before open: never open a known symlink, FIFO, device or directory.
    File file(::openat(path.directory.fd, path.name.c_str(),
                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat opened{};
    require(file.fd >= 0 && ::fstat(file.fd, &opened) == 0 && private_regular(opened, maximum) &&
                opened.st_nlink <= 1,
            "camera controller endpoint became unsafe during open");
    const bool replaced_during_open = !same(*named, opened);
    require(replaced_during_open ||
                (same_content(*named, opened) && (stable(*named, opened) || opened.st_nlink == 0)),
            "camera controller endpoint was modified during open");
    std::array<char, status_bound + 1> bytes{};
    std::size_t count = 0;
    bool eof = false;
    for (unsigned turns = 0; count < maximum + 1 && turns < io_turns; ++turns) {
      const auto got = ::read(file.fd, bytes.data() + count, maximum + 1 - count);
      if (got < 0 && errno == EINTR) continue;
      io(got >= 0, "camera controller endpoint read failed");
      if (!got) {
        eof = true;
        break;
      }
      count += std::size_t(got);
    }
    struct stat after{};
    require(eof && count == std::size_t(opened.st_size) && ::fstat(file.fd, &after) == 0 &&
                private_regular(after, maximum) && after.st_nlink <= 1 &&
                same_content(opened, after) && (stable(opened, after) || after.st_nlink == 0),
            "camera controller endpoint changed or exceeded read bounds");
    // Even a retired inode must contain canonical data. Never hide malformed
    // bytes or an in-place mutation behind a newer pathname generation.
    const std::string_view contents(bytes.data(), count);
    validate_snapshot(contents, maximum);
    path.verify();
    const auto current = path.inspect(path.name, maximum, true);
    require(current.has_value(), "camera controller endpoint disappeared during read");
    if (replaced_during_open || !same(after, *current) ||
        (current->st_nlink == 0 && !stable(after, *current))) {
      // rename unlinks the old opened inode and changes its ctime, before the
      // name resolves to its successor. Retry only when the current name is
      // another fully validated private regular file or that unlinked inode;
      // missing/unsafe endpoints and same-inode changes fail immediately. An
      // unmodified unlinked inode that the name still resolves to is current.
      require(!same(replaced_during_open ? *named : after, *current) || current->st_nlink == 0,
              "camera controller endpoint changed without a safe replacement");
      continue;
    }
    require(private_regular(after, maximum) && after.st_nlink <= 1 && stable(after, *current),
            "camera controller endpoint changed after read");
    return Snapshot{std::string(contents), after};
  }
  throw std::runtime_error("camera controller endpoint exceeded bounded replacement retries");
}
std::uint64_t decimal(std::string_view text) {
  require(!text.empty() && text.size() <= 20 && text.front() >= '1' && text.front() <= '9',
          "camera controller dates and sequences require canonical positive decimals");
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
          "camera controller decimal is malformed or overflows");
  return value;
}
void validate_request(std::string_view text) {
  if (text == "off\n" || text == "on\n") return;  // Preserve inspected legacy manual data.
  require(text.size() <= request_bound && text.starts_with("on ") && text.ends_with('\n'),
          "camera controller request is malformed");
  (void)decimal(text.substr(3, text.size() - 4));
}
std::string_view phase_name(ControlPhase phase) {
  switch (phase) {
    case ControlPhase::idle:
      return "idle";
    case ControlPhase::waiting:
      return "waiting";
    case ControlPhase::active:
      return "active";
    case ControlPhase::fault:
      return "fault";
    case ControlPhase::stopped:
      return "stopped";
  }
  throw std::invalid_argument("camera controller phase is unknown");
}
ControlPhase parse_phase(std::string_view text) {
  for (const auto phase : {ControlPhase::idle, ControlPhase::waiting, ControlPhase::active,
                           ControlPhase::fault, ControlPhase::stopped})
    if (text == phase_name(phase)) return phase;
  throw std::invalid_argument("camera controller status phase is malformed");
}
void validate_status(const ControlStatus& status) {
  require(status.instance_started_ns && status.sequence && status.observed_ns &&
              status.instance_started_ns <= status.observed_ns,
          "camera controller status requires positive instance, sequence and ordered local dates");
  (void)phase_name(status.phase);
}
ControlStatus decode_status(std::string_view text) {
  require(!text.empty() && text.size() <= status_bound && text.starts_with("HCS1 ") &&
              text.ends_with('\n'),
          "camera controller status header is malformed");
  text.remove_prefix(5);
  text.remove_suffix(1);
  std::array<std::string_view, 4> fields{};
  for (unsigned i = 0; i < 3; ++i) {
    const auto space = text.find(' ');
    require(space != text.npos, "camera controller status has missing fields");
    fields[i] = text.substr(0, space);
    text.remove_prefix(space + 1);
  }
  fields[3] = text;
  ControlStatus result{decimal(fields[0]), decimal(fields[1]), decimal(fields[2]),
                       parse_phase(fields[3])};
  validate_status(result);
  return result;
}
void validate_snapshot(std::string_view bytes, std::size_t maximum) {
  if (maximum == request_bound)
    validate_request(bytes);
  else {
    require(maximum == status_bound, "camera controller snapshot bound is unknown");
    (void)decode_status(bytes);
  }
}
enum class Payload { request, status };
void validate_payload(std::string_view text, Payload type) {
  if (type == Payload::request)
    validate_request(text);
  else
    (void)decode_status(text);
}
struct ExclusiveOwner {
  PrivatePath path;
  File file;
  struct stat identity{};
  explicit ExclusiveOwner(PrivatePath selected) : path(std::move(selected)) {
    path.verify();
    for (unsigned attempt = 0; attempt < io_turns; ++attempt) {
      const auto existing = path.inspect(path.name, 0);
      if (existing) {
        file.fd = ::openat(path.directory.fd, path.name.c_str(),
                           O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        require(file.fd >= 0 && ::fstat(file.fd, &identity) == 0 && private_file(identity, 0) &&
                    same(*existing, identity),
                "camera controller owner changed while opening");
        break;
      }
      file.fd = ::openat(path.directory.fd, path.name.c_str(),
                         O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
      if (file.fd < 0) {
        io(errno == EEXIST || errno == EINTR, "camera controller owner could not be created");
        continue;
      }
      io(::fchmod(file.fd, 0600) == 0 && ::fstat(file.fd, &identity) == 0 &&
             private_file(identity, 0),
         "camera controller owner could not be secured");
      break;
    }
    io(file.fd >= 0, "camera controller owner creation exceeded retry bound");
    io(::flock(file.fd, LOCK_EX | LOCK_NB) == 0,
       "camera controller already has a writer or cannot acquire ownership");
    verify();
  }
  void verify() const {
    path.verify();
    const auto current = path.inspect(path.name, 0);
    struct stat opened{};
    require(current && same(identity, *current) && ::fstat(file.fd, &opened) == 0 &&
                private_file(opened, 0) && same(identity, opened),
            "camera controller owner generation changed");
  }
};
struct Temporary {
  const PrivatePath& path;
  std::string name;
  struct stat identity{};
  bool installed = false;
  ~Temporary() {
    if (!installed) {
      struct stat current{};
      if (::fstatat(path.directory.fd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
          same(identity, current))
        (void)::unlinkat(path.directory.fd, name.c_str(), 0);
    }
  }
};
void atomic_publish(const PrivatePath& path, const ExclusiveOwner& owner, std::string_view bytes,
                    Payload type) {
  const auto maximum = type == Payload::request ? request_bound : status_bound;
  require(bytes.size() <= maximum, "camera controller output exceeds its bound");
  validate_payload(bytes, type);
  owner.verify();
  path.verify();
  distinct(path, owner.path);
  const auto existing = read_snapshot(path, maximum);
  if (existing) validate_payload(existing->bytes, type);
  File output;
  std::string name;
  for (unsigned attempt = 0; attempt < io_turns; ++attempt) {
    name = ".headunit-camera-" + std::to_string(::getpid()) + "-" +
           std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    if (name == path.name || (same(path.identity, owner.path.identity) && name == owner.path.name))
      continue;
    output.fd = ::openat(path.directory.fd, name.c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (output.fd >= 0) break;
    io(errno == EEXIST || errno == EINTR, "camera controller temporary could not be created");
  }
  io(output.fd >= 0, "camera controller temporary creation exceeded retry bound");
  struct stat created{};
  io(::fstat(output.fd, &created) == 0, "camera controller temporary cannot be identified");
  Temporary temporary{path, name, created};
  io(::fchmod(output.fd, 0600) == 0, "camera controller temporary permissions could not be set");
  struct stat secured{};
  require(
      ::fstat(output.fd, &secured) == 0 && private_file(secured, maximum) && same(created, secured),
      "camera controller temporary owner or identity changed");
  std::size_t count = 0;
  for (unsigned turn = 0; count < bytes.size() && turn < io_turns; ++turn) {
    const auto wrote = ::write(output.fd, bytes.data() + count, bytes.size() - count);
    if (wrote < 0 && errno == EINTR) continue;
    io(wrote > 0, "camera controller temporary write failed");
    count += std::size_t(wrote);
  }
  io(count == bytes.size(), "camera controller write exceeded bounded turns");
  struct stat written{};
  const auto named = path.inspect(name, maximum);
  require(::fstat(output.fd, &written) == 0 && private_file(written, maximum) &&
              written.st_size == off_t(bytes.size()) && same(created, written) && named &&
              stable(written, *named),
          "camera controller temporary changed after writing");
  // Data is transient control/heartbeat state. Atomic replacement provides a
  // complete snapshot, without disk flushes in the control or display turn.
  owner.verify();
  path.verify();
  distinct(path, owner.path);
  const auto current = read_snapshot(path, maximum);
  if (current) validate_payload(current->bytes, type);
  require(bool(current) == bool(existing) &&
              (!current || stable(existing->identity, current->identity)),
          "camera controller endpoint changed during publication");
  io(::renameat(path.directory.fd, name.c_str(), path.directory.fd, path.name.c_str()) == 0,
     "camera controller snapshot could not be installed atomically");
  temporary.installed = true;
  owner.verify();
  path.verify();
  const auto installed = path.inspect(path.name, maximum);
  require(installed && same(written, *installed),
          "camera controller installed snapshot is no longer current");
}
}  // namespace
struct RequestPublisher::Impl {
  PrivatePath request;
  std::unique_ptr<ExclusiveOwner> owner;
  Impl(const std::string& request_path, const std::string& owner_path) : request(request_path) {
    PrivatePath selected(owner_path);
    distinct(request, selected);
    const auto existing = read_snapshot(request, request_bound);
    if (existing) validate_request(existing->bytes);
    owner = std::make_unique<ExclusiveOwner>(std::move(selected));
    publish(false, 0);
  }
  void publish(bool on, std::uint64_t deadline) {
    require(!on || deadline, "camera controller ON requires a positive monotonic deadline");
    const auto text = on ? "on " + std::to_string(deadline) + "\n" : std::string("off\n");
    atomic_publish(request, *owner, text, Payload::request);
  }
};
RequestPublisher::RequestPublisher(std::string request, std::string owner)
    : impl_(std::make_unique<Impl>(request, owner)) {}
RequestPublisher::~RequestPublisher() {
  try {
    impl_->publish(false, 0);
  } catch (...) {
  }
}
void RequestPublisher::publish(bool on, std::uint64_t deadline) {
  impl_->publish(on, deadline);
}
struct StatusPublisher::Impl {
  PrivatePath status;
  std::unique_ptr<ExclusiveOwner> owner;
  std::optional<ControlStatus> last;
  explicit Impl(const std::string& path) : status(path) {
    PrivatePath selected(path + ".owner");
    distinct(status, selected);
    const auto existing = read_snapshot(status, status_bound);
    if (existing) (void)decode_status(existing->bytes);
    owner = std::make_unique<ExclusiveOwner>(std::move(selected));
  }
  void publish(const ControlStatus& value) {
    validate_status(value);
    require(!last || (value.instance_started_ns == last->instance_started_ns &&
                      value.sequence > last->sequence && value.observed_ns >= last->observed_ns),
            "camera controller publisher instance, sequence or clock regressed");
    const auto text = "HCS1 " + std::to_string(value.instance_started_ns) + " " +
                      std::to_string(value.sequence) + " " + std::to_string(value.observed_ns) +
                      " " + std::string(phase_name(value.phase)) + "\n";
    atomic_publish(status, *owner, text, Payload::status);
    last = value;
  }
};
StatusPublisher::StatusPublisher(std::string path) : impl_(std::make_unique<Impl>(path)) {}
StatusPublisher::~StatusPublisher() = default;
void StatusPublisher::publish(const ControlStatus& value) {
  impl_->publish(value);
}
struct StatusReader::Impl {
  PrivatePath status;
  std::optional<ControlStatus> last;
  std::optional<std::uint64_t> last_poll;
  bool live = false, clock_fault = false;
  explicit Impl(const std::string& path) : status(path) {}
  bool reference(std::uint64_t now) {
    if (!now || (last_poll && now < *last_poll)) {
      clock_fault = true;
      live = false;
      return false;
    }
    last_poll = now;
    return true;
  }
  static std::uint64_t clock_now() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
  }
  std::optional<ControlStatus> poll(std::optional<std::uint64_t> supplied_now,
                                    unsigned maximum_age) {
    if (clock_fault) return {};
    if (supplied_now && !reference(*supplied_now)) return {};
    if (!maximum_age) {
      if (!supplied_now) (void)reference(clock_now());
      live = false;
      return {};
    }
    const auto snapshot = read_snapshot(status, status_bound);
    // Production time is sampled after all read/retry work. It never replaces
    // the heartbeat's original observation date; fixture time remains exact.
    const auto now = supplied_now ? *supplied_now : clock_now();
    if (!supplied_now && !reference(now)) return {};
    if (!snapshot) {
      live = false;
      return {};
    }
    const auto value = decode_status(snapshot->bytes);
    if (value.observed_ns > now ||
        now - value.observed_ns >= std::uint64_t(maximum_age) * 1000000) {
      live = false;
      return {};
    }
    if (last) {
      if (value == *last) return live ? last : std::nullopt;
      if (value.instance_started_ns < last->instance_started_ns ||
          value.observed_ns < last->observed_ns ||
          (value.instance_started_ns == last->instance_started_ns &&
           value.sequence <= last->sequence)) {
        live = false;
        return {};
      }
    }
    last = value;
    live = true;
    return value;
  }
};
StatusReader::StatusReader(std::string path) : impl_(std::make_unique<Impl>(path)) {}
StatusReader::~StatusReader() = default;
std::optional<ControlStatus> StatusReader::poll(std::uint64_t now, unsigned age) noexcept {
  try {
    return impl_->poll(now, age);
  } catch (...) {
    impl_->live = false;
    return {};
  }
}
std::optional<ControlStatus> StatusReader::poll(unsigned age) noexcept {
  try {
    return impl_->poll(std::nullopt, age);
  } catch (...) {
    impl_->live = false;
    return {};
  }
}
}  // namespace headunit::camera
