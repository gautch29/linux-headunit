// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_publication.hpp>
#include <headunit/ambient_light_publication.hpp>
#include <headunit/prephone_indicator_publication.hpp>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <ctime>
#ifdef __linux__
#include <cerrno>
#include <filesystem>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace headunit {
namespace {
std::uint64_t ns(Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}
void integer(PrephonePacket& packet, std::size_t offset, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    packet[offset + i] = std::byte(value >> (i * 8));
}
}  // namespace
PrephonePacket prephone_packet(const PrephoneVehicleSummary& model, const PrephoneNonce& nonce,
                               std::uint64_t sequence, Clock::time_point copied_at) {
  static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
  PrephonePacket packet{};
  std::memcpy(packet.data(), "HUPN001", 7);
  std::memcpy(packet.data() + 8, nonce.data(), nonce.size());
  integer(packet, 40, sequence);
  integer(packet, 48, ns(copied_at));
  integer(packet, 56, static_cast<std::uint64_t>(model.state));
  integer(packet, 64, model.source_generation.value_or(0));
  integer(packet, 72, model.source_started_at ? ns(*model.source_started_at) : 0);
  integer(packet, 80, model.source_sampled_at ? ns(*model.source_sampled_at) : 0);
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    const auto& row = model.rows[i];
    const auto at = 88 + i * 32;
    packet[at] = std::byte(row.availability);
    packet[at + 1] = row.origin ? std::byte(*row.origin) : std::byte{255};
    integer(packet, at + 8, std::bit_cast<std::uint64_t>(row.value.value_or(0)));
    integer(packet, at + 16, row.acquired_at ? ns(*row.acquired_at) : 0);
    integer(packet, at + 24,
            row.maximum_age
                ? std::chrono::duration_cast<std::chrono::nanoseconds>(*row.maximum_age).count()
                : 0);
  }
  return packet;
}
PrephonePublisher::PrephonePublisher(std::string path, bool ambient_enabled,
                                     bool indicators_enabled)
    : path_(std::move(path)),
      ambient_enabled_(ambient_enabled),
      indicators_enabled_(indicators_enabled) {}
PrephonePublisher::~PrephonePublisher() {
  retire();
}
void PrephonePublisher::retire() noexcept {
  if (ambient_) {
    try {
      ambient_->retire(Clock::now());
    } catch (...) {
      // Optional software policy cannot interrupt native/source retirement.
    }
    ambient_.reset();
  }
#ifdef __linux__
  if (socket_ >= 0) close(socket_);
#endif
  socket_ = -1;
  admitted_ = false;
  sequence_ = 0;
  nonce_.fill(std::byte{0});
  peer_.fill(0);
}
void PrephonePublisher::publish(const VehicleSample& sample, Clock::time_point now) noexcept {
#ifdef __linux__
  if (now < next_copy_) return;
  next_copy_ = now + std::chrono::milliseconds(200);
  if (socket_ < 0) {
    if (now < retry_at_) return;
    retry_at_ = now + std::chrono::seconds(1);
    sockaddr_un address{};
    // No filesystem operations here: manager owns private endpoint validation.
    // Bounded canonical path validation is also in the sealed configuration.
    if (path_.empty() || path_.size() >= sizeof(address.sun_path) || path_[0] != '/') return;
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
    address.sun_family = AF_UNIX;
    socket_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket_ < 0) return;
    if (connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      retire();
      return;
    }
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (getsockopt(socket_, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 ||
        (peer.uid != 0 && peer.uid != getuid())) {
      retire();
      return;
    }
    peer_ = {peer.pid, static_cast<int>(peer.uid), static_cast<int>(peer.gid)};
    if (ambient_enabled_) {
      const int enabled = 1;
      if (setsockopt(socket_, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0) {
        retire();
        return;
      }
    }
  }
  // Exactly one bounded receive/send attempt. No busy retry and no pending queue.
  if (!admitted_) {
    std::array<std::byte, 33> challenge{};
    const auto size = recv(socket_, challenge.data(), challenge.size(), MSG_DONTWAIT);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (size != static_cast<ssize_t>(nonce_.size())) {
      retire();
      return;
    }
    std::memcpy(nonce_.data(), challenge.data(), nonce_.size());
    admitted_ = true;
    if (ambient_enabled_) {
      try {
        ambient_ = std::make_unique<ambient::SoftwareModel>();
      } catch (...) {
        retire();
        return;
      }
    }
  }
  if (ambient_) {
    // ONE fixed software action, on the same owner/connection. No JSON, queue,
    // physical I/O, blocking wait or new source. Credentials reject child writers.
    std::array<std::byte, ambient::software_action_size + 1> bytes{};
    alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(ucred))> control{};
    iovec vector{bytes.data(), bytes.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const auto received = recvmsg(socket_, &message, MSG_DONTWAIT);
    if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      retire();
      return;
    }
    if (received >= 0) {
      const auto* header = CMSG_FIRSTHDR(&message);
      ucred sender{};
      if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS &&
          header->cmsg_len == CMSG_LEN(sizeof(sender)))
        std::memcpy(&sender, CMSG_DATA(header), sizeof(sender));
      try {
        if (received != static_cast<ssize_t>(ambient::software_action_size) ||
            message.msg_flags & (MSG_TRUNC | MSG_CTRUNC) || !header ||
            CMSG_NXTHDR(&message, const_cast<cmsghdr*>(header)) ||
            peer_ != std::array<int, 3>{sender.pid, static_cast<int>(sender.uid),
                                        static_cast<int>(sender.gid)} ||
            !ambient_->change(std::span(bytes).first(received), nonce_, Clock::now())) {
          retire();
          return;
        }
      } catch (...) {
        retire();
        return;
      }
    }
  }
  if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    retire();
    return;
  }
  const auto generation = sample.observation_timing ? sample.observation_timing->generation : 0;
  // Absence of actual metadata is explicitly undated, not a generation minted
  // from native redraw time. The pure projection performs full validity checks.
  auto model = prephone_vehicle_summary(sample, {generation, false}, now);
  if (!sample.observation_timing &&
      (sample.speed_kmh || sample.engine_rpm || sample.sampled_at != Clock::time_point{}))
    model.state = VehicleSummaryState::undated_source;
  const auto packet = prephone_packet(model, nonce_, ++sequence_, now);
  const auto sent = send(socket_, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
  if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
  if (sent != static_cast<ssize_t>(packet.size())) {
    retire();
    return;
  }
  if (indicators_enabled_) {
    // The existing admitted Linux native role uses this host steady domain.
    // Reuse the accepted model's source gate on the SAME original sample/date.
    auto indicators = prephone_indicator_summary(
        sample, {{generation, false}, PrephoneIndicatorClockDomain::host_steady}, now);
    // Match the existing carrier's undated diagnostic, without inventing a
    // generation or changing the accepted model's source-admission gate.
    if (model.state == VehicleSummaryState::undated_source)
      indicators.state = VehicleSummaryState::undated_source;
    const auto bytes = prephone_indicator_packet(indicators, nonce_, sequence_, now);
    const auto copied = send(socket_, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (copied < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      retire();
      return;
    }
    if (copied >= 0 && copied != static_cast<ssize_t>(bytes.size())) {
      retire();
      return;
    }
    // A skipped optional lamp record does not queue work or prevent ambient
    // policy progress. Each projection independently retains original freshness.
  }
  if (ambient_) {
    try {
      std::optional<unsigned> minute;
      const auto wall = std::time(nullptr);
      std::tm local{};
      if (wall != std::time_t(-1) && localtime_r(&wall, &local))
        minute = static_cast<unsigned>(local.tm_hour * 60 + local.tm_min);
      const auto model = ambient_->publish(sample, nonce_, sequence_, now, minute);
      const auto copied = send(socket_, model.data(), model.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
      if (copied < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
      if (copied != static_cast<ssize_t>(model.size())) retire();
    } catch (...) {
      retire();
    }
  }
#else
  (void)sample;
  (void)now;
  (void)ambient_enabled_;  // Kernel role/clock admission is Linux-only.
  (void)indicators_enabled_;
#endif
}
PublishingVehicle::PublishingVehicle(std::unique_ptr<VehicleSource> source, std::string path,
                                     bool ambient_enabled, bool indicators_enabled)
    : source_(std::move(source)), publisher_(std::move(path), ambient_enabled, indicators_enabled) {
  if (!source_) throw std::invalid_argument("Publishing vehicle needs its existing source");
}
VehicleSample PublishingVehicle::sample(Clock::time_point now) {
  try {
    auto sample = source_->sample(now);
    publisher_.publish(sample, now);
    return sample;
  } catch (...) {
    publisher_.retire();
    throw;
  }
}
}  // namespace headunit
