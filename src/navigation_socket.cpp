// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/navigation_socket.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace headunit {
namespace {
std::uint64_t date(Clock::time_point value) {
  const auto n =
      std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
  if (n <= 0) throw std::invalid_argument("invalid navigation date");
  return std::uint64_t(n);
}
Clock::time_point time(std::uint64_t n) {
  if (n == 0 || n > std::uint64_t(INT64_MAX))
    throw std::invalid_argument("invalid navigation date");
  return Clock::time_point{
      std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(n))};
}
bool valid_text(std::string_view value) {
  if (value.size() > 256) return false;
  for (std::size_t i = 0; i < value.size();) {
    const auto b = static_cast<unsigned char>(value[i++]);
    if (b < 32 || b == 127) return false;
    if (b < 128) continue;
    const unsigned length = b >= 0xc2 && b <= 0xdf   ? 1
                            : b >= 0xe0 && b <= 0xef ? 2
                            : b >= 0xf0 && b <= 0xf4 ? 3
                                                     : 0;
    if (!length || i + length > value.size()) return false;
    std::uint32_t code = b & ((1u << (6 - length)) - 1);
    for (unsigned j = 0; j < length; ++j) {
      const auto next = static_cast<unsigned char>(value[i++]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 63);
    }
    if ((length == 2 && code < 0x800) || (length == 3 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return true;
}
struct Writer {
  std::vector<std::uint8_t> bytes{'H', 'N', 'G', '1'};
  void number(std::uint64_t v, unsigned n) {
    for (unsigned i = n; i > 0; --i)
      bytes.push_back(std::uint8_t(v >> ((i - 1) * 8)));
  }
  template <class T>
  void optional(const std::optional<T>& v, unsigned n) {
    number(bool(v), 1);
    if (v) number(std::uint64_t(*v), n);
  }
  void text(const std::string& v) {
    if (!valid_text(v)) throw std::invalid_argument("invalid navigation text");
    number(v.size(), 2);
    bytes.insert(bytes.end(), v.begin(), v.end());
  }
};
struct Reader {
  std::span<const std::uint8_t> bytes;
  std::size_t offset = 4;
  std::uint64_t number(unsigned n) {
    if (n > bytes.size() - offset) throw std::invalid_argument("truncated navigation packet");
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
      v = (v << 8) | bytes[offset++];
    return v;
  }
  bool flag() {
    const auto v = number(1);
    if (v > 1) throw std::invalid_argument("invalid navigation flag");
    return v != 0;
  }
  template <class T>
  std::optional<T> optional(unsigned n) {
    if (!flag()) return {};
    return T(number(n));
  }
  std::string text() {
    const auto n = number(2);
    if (n > 256 || n > bytes.size() - offset)
      throw std::invalid_argument("invalid navigation text size");
    std::string v(reinterpret_cast<const char*>(bytes.data() + offset), n);
    offset += n;
    if (!valid_text(v)) throw std::invalid_argument("invalid navigation text");
    return v;
  }
};
void valid(const NavigationPublication& p) {
  const auto& n = p.navigation;
  if (!p.owner_epoch || p.owner_epoch > date(p.sent_at))
    throw std::invalid_argument("invalid navigation publisher epoch");
  if (n.available && (!p.session_generation || !n.route_state || *n.route_state < 1 ||
                      *n.route_state > 6 || !n.updated_at || *n.updated_at > p.sent_at))
    throw std::invalid_argument("incomplete navigation source");
  if (n.updated_at) (void)date(*n.updated_at);
  if (!n.available && (n.route_state || !n.source.empty() || !n.current_road.empty() ||
                       !n.destination.empty() || n.distance_m || n.remaining_m ||
                       n.remaining_seconds || n.arrival_unix_seconds || n.maneuver || n.updated_at))
    throw std::invalid_argument("unavailable navigation carries retained content");
}
int local_socket() {
#ifdef __linux__
  return socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return fd;
  const auto flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    const auto error = errno;
    close(fd);
    errno = error;
    return -1;
  }
  return fd;
#endif
}
sockaddr_un address(const std::string& path) {
  sockaddr_un a{};
  const std::filesystem::path name(path);
  const auto parent = name.parent_path();
  struct stat st{};
  if (path.empty() || path.size() >= sizeof(a.sun_path) || !name.is_absolute() ||
      name.lexically_normal() != name || lstat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != getuid() || (st.st_mode & 0077) || std::filesystem::canonical(parent) != parent)
    throw std::invalid_argument(
        "navigation endpoint needs an exact path in an owned private directory");
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  return a;
}
int owner_lock(const std::string& path) {
  const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  struct stat st{};
  if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
      st.st_uid != getuid() || (st.st_mode & 0077) || flock(fd, LOCK_EX | LOCK_NB) != 0) {
    if (fd >= 0) close(fd);
    throw std::runtime_error("navigation endpoint already owned or unsafe lock");
  }
  return fd;
}
struct Endpoint {
  std::string path;
  int fd = -1, lock = -1;
  std::uint64_t device = 0, inode = 0;
  bool bound = false;
  ~Endpoint() {
    if (fd >= 0) close(fd);
    if (bound) {
      struct stat st{};
      if (lstat(path.c_str(), &st) == 0 && std::uint64_t(st.st_dev) == device &&
          std::uint64_t(st.st_ino) == inode)
        unlink(path.c_str());
    }
    if (lock >= 0) close(lock);
  }
  void bind(const std::string& name) {
    path = name;
    const auto a = address(path);
    lock = owner_lock(path + ".lock");
    struct stat old{};
    if (lstat(path.c_str(), &old) == 0) {
      if (!S_ISSOCK(old.st_mode) || old.st_nlink != 1 || old.st_uid != getuid() ||
          (old.st_mode & 0077))
        throw std::runtime_error("unsafe navigation endpoint");
      const int probe = local_socket();
      if (probe < 0) throw std::runtime_error("cannot inspect navigation endpoint");
      const int result = connect(probe, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
      const auto error = errno;
      close(probe);
      if (result == 0 || (error != ECONNREFUSED && error != ENOENT))
        throw std::runtime_error("navigation endpoint has a live owner");
      struct stat current{};
      if (lstat(path.c_str(), &current) == 0) {
        if (current.st_dev != old.st_dev || current.st_ino != old.st_ino ||
            unlink(path.c_str()) != 0)
          throw std::runtime_error("navigation endpoint changed during recovery");
      } else if (errno != ENOENT)
        throw std::runtime_error("cannot recover navigation endpoint");
    } else if (errno != ENOENT)
      throw std::runtime_error("cannot inspect navigation endpoint");
    fd = local_socket();
    if (fd < 0 || ::bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0)
      throw std::runtime_error("cannot bind navigation endpoint");
    struct stat own{};
    if (lstat(path.c_str(), &own) != 0)
      throw std::runtime_error("cannot inspect navigation endpoint");
    bound = true;
    device = own.st_dev;
    inode = own.st_ino;
    if (chmod(path.c_str(), 0600) != 0)
      throw std::runtime_error("cannot restrict navigation endpoint");
#ifdef __linux__
    const int credentials = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &credentials, sizeof(credentials)) != 0)
      throw std::runtime_error("cannot authenticate navigation publisher");
#endif
  }
};
}  // namespace
std::vector<std::uint8_t> encode_navigation_datagram(const NavigationPublication& p) {
  valid(p);
  Writer w;
  const auto& n = p.navigation;
  w.number(p.owner_epoch, 8);
  w.number(p.session_generation, 8);
  w.number(date(p.sent_at), 8);
  w.number(n.available, 1);
  if (n.available) {
    w.number(date(*n.updated_at), 8);
    w.number(*n.route_state, 1);
    w.text(n.source);
    w.text(n.current_road);
    w.text(n.destination);
    w.optional(n.distance_m, 4);
    w.optional(n.remaining_m, 4);
    w.optional(n.remaining_seconds, 8);
    w.optional(n.arrival_unix_seconds, 8);
    w.number(bool(n.maneuver), 1);
    if (n.maneuver) {
      const auto& m = *n.maneuver;
      w.number(m.index, 2);
      w.optional(m.type, 1);
      w.text(m.instruction);
      w.text(m.road);
      w.optional(m.exit_angle, 2);
    }
  }
  if (w.bytes.size() > maximum_navigation_datagram)
    throw std::invalid_argument("navigation packet too large");
  return std::move(w.bytes);
}
std::optional<NavigationPublication> parse_navigation_datagram(std::span<const std::uint8_t> bytes,
                                                               Clock::time_point now) {
  try {
    if (bytes.size() < 29 || bytes.size() > maximum_navigation_datagram ||
        std::memcmp(bytes.data(), "HNG1", 4) != 0)
      return {};
    Reader r{bytes};
    NavigationPublication p;
    p.owner_epoch = r.number(8);
    p.session_generation = r.number(8);
    p.sent_at = time(r.number(8));
    auto& n = p.navigation;
    n.available = r.flag();
    if (n.available) {
      n.updated_at = time(r.number(8));
      n.route_state = std::uint8_t(r.number(1));
      n.source = r.text();
      n.current_road = r.text();
      n.destination = r.text();
      n.distance_m = r.optional<std::uint32_t>(4);
      n.remaining_m = r.optional<std::uint32_t>(4);
      n.remaining_seconds = r.optional<std::uint64_t>(8);
      n.arrival_unix_seconds = r.optional<std::uint64_t>(8);
      if (r.flag()) {
        NavigationManeuver m;
        m.index = std::uint16_t(r.number(2));
        m.type = r.optional<std::uint8_t>(1);
        m.instruction = r.text();
        m.road = r.text();
        m.exit_angle = r.optional<std::int16_t>(2);
        n.maneuver = std::move(m);
      }
    }
    valid(p);
    if (r.offset != bytes.size() || p.sent_at > now || now - p.sent_at >= navigation_owner_lease)
      return {};
    return p;
  } catch (const std::invalid_argument&) {
    return {};
  }
}
struct NavigationPublisher::Impl {
  std::string path;
  int fd = -1, lock = -1;
  sockaddr_un target{};
  std::uint64_t epoch = 0;
  std::optional<Clock::time_point> last;
  ~Impl() {
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
  }
};
NavigationPublisher::NavigationPublisher(std::string path) : impl_(std::make_unique<Impl>()) {
  auto& p = *impl_;
  p.path = std::move(path);
  p.target = address(p.path);
  p.lock = owner_lock(p.path + ".publisher.lock");
  p.fd = local_socket();
  if (p.fd < 0) throw std::runtime_error("cannot create navigation publisher");
  p.epoch = date(Clock::now());
}
NavigationPublisher::~NavigationPublisher() = default;
bool NavigationPublisher::publish(const NavigationSnapshot& navigation, std::uint64_t generation,
                                  Clock::time_point now) {
  auto& p = *impl_;
  if (p.last && now < *p.last) throw std::invalid_argument("navigation publisher clock reversed");
  if (p.last && now - *p.last < std::chrono::milliseconds(250)) return false;
  const auto bytes = encode_navigation_datagram(
      {p.epoch, generation, now, navigation.available ? navigation : NavigationSnapshot{}});
  p.last = now;
  struct stat endpoint{};
  if (lstat(p.path.c_str(), &endpoint) != 0) {
    if (errno == ENOENT) return false;
    throw std::runtime_error("cannot inspect navigation publication endpoint");
  }
  if (!S_ISSOCK(endpoint.st_mode) || endpoint.st_nlink != 1 || endpoint.st_uid != getuid() ||
      (endpoint.st_mode & 0777) != 0600)
    throw std::runtime_error("unsafe navigation publication endpoint");
  const auto sent = sendto(p.fd, bytes.data(), bytes.size(), MSG_DONTWAIT,
                           reinterpret_cast<const sockaddr*>(&p.target), sizeof(p.target));
  if (sent < 0) {
    if (errno == ENOENT || errno == ECONNREFUSED || errno == EAGAIN || errno == EWOULDBLOCK ||
        errno == ENOBUFS || errno == EINTR)
      return false;
    throw std::runtime_error("navigation publication failed");
  }
  return std::size_t(sent) == bytes.size();
}
struct SocketNavigation::Impl {
  Endpoint endpoint;
  std::optional<NavigationPublication> latest;
  std::optional<Clock::time_point> last_now;
  std::optional<std::thread::id> owner;
  std::optional<Clock::time_point> withdrawn_change;
  std::vector<std::uint8_t> latest_wire;
};
SocketNavigation::SocketNavigation(std::string path) : impl_(std::make_unique<Impl>()) {
  impl_->endpoint.bind(path);
}
SocketNavigation::~SocketNavigation() = default;
NavigationSnapshot SocketNavigation::sample(Clock::time_point now) {
  auto& s = *impl_;
  const auto thread = std::this_thread::get_id();
  if ((s.owner && *s.owner != thread) || now <= Clock::time_point{} ||
      (s.last_now && now < *s.last_now))
    throw std::invalid_argument("navigation sampling owner or clock changed");
  s.owner = thread;
  s.last_now = now;
  for (unsigned count = 0; count < 64; ++count) {
    std::array<std::uint8_t, maximum_navigation_datagram + 1> bytes{};
    iovec io{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
#ifdef __linux__
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(ucred))> control{};
    message.msg_control = control.data();
    message.msg_controllen = control.size();
#endif
    const auto size = recvmsg(s.endpoint.fd, &message, MSG_DONTWAIT);
    if (size < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      throw std::runtime_error("navigation endpoint read failed");
    }
    if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC) ||
        std::size_t(size) > maximum_navigation_datagram)
      continue;
#ifdef __linux__
    bool owned = false;
    for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS &&
          c->cmsg_len == CMSG_LEN(sizeof(ucred))) {
        ucred credential{};
        std::memcpy(&credential, CMSG_DATA(c), sizeof(credential));
        owned = credential.uid == getuid();
      }
    if (!owned) continue;
#endif
    auto p = parse_navigation_datagram(std::span(bytes.data(), std::size_t(size)), now);
    if (!p) continue;
    if (s.latest) {
      const auto& old = *s.latest;
      if (p->owner_epoch < old.owner_epoch || p->sent_at <= old.sent_at ||
          (p->owner_epoch == old.owner_epoch && p->session_generation < old.session_generation))
        continue;
      if (p->owner_epoch == old.owner_epoch && p->session_generation == old.session_generation &&
          p->navigation.available && old.navigation.updated_at &&
          p->navigation.updated_at < old.navigation.updated_at)
        continue;
      if (p->owner_epoch == old.owner_epoch && p->session_generation == old.session_generation) {
        if (p->navigation.available && s.withdrawn_change &&
            p->navigation.updated_at <= s.withdrawn_change)
          continue;
        if (p->navigation.available && old.navigation.available &&
            p->navigation.updated_at == old.navigation.updated_at &&
            (std::size_t(size) != s.latest_wire.size() ||
             !std::equal(bytes.begin() + 28, bytes.begin() + size, s.latest_wire.begin() + 28)))
          continue;
        if (!p->navigation.available && old.navigation.updated_at)
          s.withdrawn_change = old.navigation.updated_at;
      } else
        s.withdrawn_change.reset();
    }
    s.latest_wire.assign(bytes.begin(), bytes.begin() + size);
    s.latest = std::move(p);
  }
  if (!s.latest || now - s.latest->sent_at >= navigation_owner_lease) return {};
  return s.latest->navigation;
}
}  // namespace headunit
