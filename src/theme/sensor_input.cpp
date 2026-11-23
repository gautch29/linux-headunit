// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/sensor_input.hpp>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace headunit::theme::vehicle {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
const opack::Value* field(const opack::Dictionary& record, const char* name) {
  for (const auto& [key, value] : record)
    if (const auto* text = std::get_if<std::string>(&key.data); text && *text == name)
      return &value;
  return nullptr;
}
void apply(ReadOnlyService& source, std::span<const std::uint8_t> bytes, std::uint64_t now) {
  const auto decoded = opack::decode(bytes);
  const auto* record = std::get_if<opack::Dictionary>(&decoded.data);
  check(record && record->size() == 3, "invalid sensor record fields");
  const auto *id = field(*record, "iid"), *time = field(*record, "sampledMs");
  check(id && time && std::holds_alternative<std::int64_t>(id->data) &&
            std::holds_alternative<std::int64_t>(time->data),
        "invalid sensor identity or timestamp");
  const auto iid = std::get<std::int64_t>(id->data), sampled = std::get<std::int64_t>(time->data);
  check(iid >= 0 && sampled >= 0, "negative sensor identity or timestamp");
  const auto *value = field(*record, "value"), *invalid = field(*record, "invalid");
  check(bool(value) != bool(invalid), "sensor record must contain one reading or invalidation");
  if (invalid) {
    check(std::holds_alternative<bool>(invalid->data) && std::get<bool>(invalid->data),
          "invalid sensor invalidation");
    source.invalidate(iid, std::uint64_t(sampled), now);
  } else {
    check(!std::holds_alternative<std::monostate>(value->data), "null sensor reading");
    source.publish(iid, *value, std::uint64_t(sampled), now);
  }
}
}  // namespace
SensorInput::SensorInput(int fd, std::shared_ptr<ReadOnlyService> source)
    : source_(std::move(source)) {
  check(bool(source_) && fd >= 3, "invalid sensor input source");
  sockaddr_storage address{};
  socklen_t address_size = sizeof(address);
  int type = 0;
  socklen_t type_size = sizeof(type);
  check(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) == 0 &&
            address.ss_family == AF_UNIX &&
            ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) == 0 && type == SOCK_SEQPACKET,
        "sensor input requires a local packet socket");
  fd_ = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
  if (fd_ < 0) throw std::runtime_error("cannot retain sensor input socket");
  const auto flags = ::fcntl(fd_, F_GETFL);
  if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("cannot configure sensor input socket");
  }
}
SensorInput::~SensorInput() {
  if (fd_ >= 0) ::close(fd_);
}
void SensorInput::fail(std::string reason, std::uint64_t now) {
  if (fd_ < 0) return;
  failure_ = std::move(reason);
  ::close(fd_);
  fd_ = -1;
  source_->invalidate_dynamic(now);
}
std::size_t SensorInput::pump(std::uint64_t now) {
  if (fd_ < 0) return 0;
  std::size_t count = 0;
  for (; count < 32; ++count) {
    std::array<std::uint8_t, 2049> packet{};
    const auto size = ::recv(fd_, packet.data(), packet.size(), MSG_DONTWAIT);
    if (size < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return count;
      if (errno == EINTR) continue;
      fail("sensor socket read failed", now);
      return count;
    }
    if (size == 0) {
      fail("sensor provider disconnected", now);
      return count;
    }
    if (size > 2048) {
      fail("sensor packet exceeds limit", now);
      return count;
    }
    try {
      apply(*source_, std::span(packet).first(std::size_t(size)), now);
    } catch (const std::exception&) {
      fail("invalid sensor packet or sample", now);
      return count;
    }
  }
  return count;
}
}  // namespace headunit::theme::vehicle
