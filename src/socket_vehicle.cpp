// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/socket_vehicle.hpp>
#include <headunit/vehicle_wire.hpp>
#include <headunit/vehicle_observation_wire.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace headunit {
namespace {
int local_socket(bool nonblocking) {
#ifdef __linux__
  return socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0), 0);
#else
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return fd;
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
      (nonblocking && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)) {
    const auto error = errno;
    close(fd);
    errno = error;
    return -1;
  }
  return fd;
#endif
}
}  // namespace

SocketVehicle::SocketVehicle(std::string path) : path_(std::move(path)) {
  sockaddr_un address{};
  if (path_.empty() || path_.size() >= sizeof(address.sun_path)) {
    throw std::invalid_argument("vehicle socket path is too long or empty");
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
  try {
    const std::filesystem::path name(path_);
    const auto parent = name.parent_path();
    struct stat parent_info{};
    if (!name.is_absolute() || name.lexically_normal() != name ||
        lstat(parent.c_str(), &parent_info) != 0 || !S_ISDIR(parent_info.st_mode) ||
        parent_info.st_uid != getuid() || parent_info.st_mode & 0077 ||
        std::filesystem::canonical(parent) != parent)
      throw std::invalid_argument(
          "vehicle input needs an exact path in an owned private directory");
    lock_ = open((path_ + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lock_info{};
    if (lock_ < 0 || fstat(lock_, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) ||
        lock_info.st_uid != getuid() || lock_info.st_mode & 0077 ||
        flock(lock_, LOCK_EX | LOCK_NB) != 0)
      throw std::runtime_error("vehicle input already has an owner or an unsafe lock");
    struct stat old{};
    if (lstat(path_.c_str(), &old) == 0) {
      if (!S_ISSOCK(old.st_mode) || old.st_uid != getuid() || old.st_mode & 0077)
        throw std::runtime_error("vehicle input path has an unexpected owner or type");
      const int probe = local_socket(false);
      if (probe < 0) throw std::runtime_error("cannot inspect vehicle input owner");
      const auto connected = connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
      const auto error = errno;
      close(probe);
      if (connected == 0 || (error != ECONNREFUSED && error != ENOENT))
        throw std::runtime_error("vehicle input already has a live owner");
      struct stat current{};
      if (lstat(path_.c_str(), &current) == 0) {
        if (current.st_dev != old.st_dev || current.st_ino != old.st_ino ||
            unlink(path_.c_str()) != 0)
          throw std::runtime_error("vehicle input endpoint changed during recovery");
      } else if (errno != ENOENT)
        throw std::runtime_error("cannot inspect stale vehicle input");
    } else if (errno != ENOENT)
      throw std::runtime_error("cannot inspect vehicle input path");
    socket_ = local_socket(true);
    if (socket_ < 0) throw std::runtime_error("cannot create vehicle input socket");
    if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
      throw std::runtime_error("cannot bind vehicle input socket");
    struct stat own{};
    if (lstat(path_.c_str(), &own) != 0)
      throw std::runtime_error("cannot inspect bound vehicle input");
    bound_ = true;
    bound_device_ = own.st_dev;
    bound_inode_ = own.st_ino;
    if (chmod(path_.c_str(), S_IRUSR | S_IWUSR) != 0)
      throw std::runtime_error("cannot restrict vehicle input socket");
  } catch (...) {
    cleanup();
    throw;
  }
}

void SocketVehicle::cleanup() noexcept {
  if (socket_ >= 0) {
    close(socket_);
    socket_ = -1;
  }
  if (bound_) {
    struct stat current{};
    if (lstat(path_.c_str(), &current) == 0 &&
        static_cast<std::uint64_t>(current.st_dev) == bound_device_ &&
        static_cast<std::uint64_t>(current.st_ino) == bound_inode_)
      unlink(path_.c_str());
    bound_ = false;
  }
  if (lock_ >= 0) {
    close(lock_);
    lock_ = -1;
  }
}
SocketVehicle::~SocketVehicle() {
  cleanup();
}

VehicleSample SocketVehicle::sample(Clock::time_point now) {
  const auto owner = std::this_thread::get_id();
  if (sample_owner_ && owner != *sample_owner_)
    throw std::invalid_argument("vehicle socket sampled by another owner thread");
  if (now <= Clock::time_point{} || (last_now_ && now < *last_now_))
    throw std::invalid_argument("vehicle consumer clock is nonpositive or reversed");
  // Standard preview creates the source on its control thread and hands it to
  // its painter before first use. Bind the stateful receiver to that first user.
  if (!observations_) observations_.emplace();
  sample_owner_ = owner;
  last_now_ = now;
  char packet[maximum_vehicle_observation_datagram + 1];
  // Bound input work so a noisy adapter cannot hold up a cluster frame.
  for (int packet_count = 0; packet_count < 64; ++packet_count) {
    const auto size = recv(socket_, packet, sizeof(packet), MSG_DONTWAIT);
    if (size < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      throw std::runtime_error("vehicle input socket read failed");
    }
    if (static_cast<std::size_t>(size) > maximum_vehicle_observation_datagram) continue;
    const auto text = std::string_view(packet, static_cast<std::size_t>(size));
    if (text.starts_with("HVO1 ") || text.starts_with("HVO2 ") || text.starts_with("HVO3 ") ||
        text.starts_with("HVO4 ") || text.starts_with("HVO6 ")) {
      if (observed_format_ && !*observed_format_) continue;
      if (const auto parsed = parse_vehicle_observation_datagram(text, now)) {
        try {
          observations_->accept(*parsed, now);
          observed_format_ = true;
        } catch (const std::invalid_argument&) { /* Reject the complete bad/replayed snapshot. */
        }
      }
      continue;
    }
    if (observed_format_ && *observed_format_) continue;
    if (static_cast<std::size_t>(size) > maximum_vehicle_transport_datagram) continue;
    if (const auto parsed = parse_vehicle_datagram(text, now)) {
      // A consumer paused behind its kernel queue must not renew a broker
      // sample, nor let an older queued/replayed sample replace a newer one.
      if (parsed->sampled_at >= latest_.sampled_at) {
        latest_ = *parsed;
        observed_format_ = false;
      }
    }
  }
  return observed_format_ && *observed_format_ ? observations_->sample(now) : latest_;
}

}  // namespace headunit
