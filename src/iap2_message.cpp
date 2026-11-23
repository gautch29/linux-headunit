// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/iap2_message.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace headunit {
namespace {
constexpr std::size_t header_bytes = 6;
}

ControlMessageReader::ControlMessageReader(std::size_t maximum_message_bytes)
    : maximum_message_bytes_(maximum_message_bytes) {
  if (maximum_message_bytes < header_bytes || maximum_message_bytes > 65535) {
    throw std::invalid_argument("iAP2 message maximum must be 6..65535");
  }
}

std::vector<ControlMessage> ControlMessageReader::feed(std::span<const std::uint8_t> bytes) {
  std::vector<ControlMessage> messages;
  for (const auto byte : bytes) {
    if (pending_.empty() && byte != 0x40) continue;
    pending_.push_back(byte);
    for (;;) {
      if (pending_.size() < 2) break;
      if (pending_[1] != 0x40) {
        const auto next = std::find(pending_.begin() + 1, pending_.end(), std::uint8_t{0x40});
        pending_.erase(pending_.begin(), next);
        continue;
      }
      if (pending_.size() < header_bytes) break;
      const std::size_t total = (std::size_t(pending_[2]) << 8) | pending_[3];
      if (total < header_bytes || total > maximum_message_bytes_) {
        const auto next = std::find(pending_.begin() + 1, pending_.end(), std::uint8_t{0x40});
        pending_.erase(pending_.begin(), next);
        continue;
      }
      if (pending_.size() < total) break;
      const auto identifier =
          static_cast<std::uint16_t>((std::uint16_t(pending_[4]) << 8) | pending_[5]);
      ControlMessage message{identifier,
                             {pending_.begin() + header_bytes, pending_.begin() + total}};
      messages.push_back(std::move(message));
      pending_.erase(pending_.begin(), pending_.begin() + total);
    }
  }
  return messages;
}

std::vector<std::uint8_t> encode_control_message(const ControlMessage& message) {
  const std::size_t total = header_bytes + message.parameters.size();
  if (total > 65535) throw std::invalid_argument("iAP2 message exceeds 16-bit length");
  std::vector<std::uint8_t> bytes{0x40,
                                  0x40,
                                  static_cast<std::uint8_t>(total >> 8),
                                  static_cast<std::uint8_t>(total),
                                  static_cast<std::uint8_t>(message.identifier >> 8),
                                  static_cast<std::uint8_t>(message.identifier)};
  bytes.insert(bytes.end(), message.parameters.begin(), message.parameters.end());
  return bytes;
}

}  // namespace headunit
