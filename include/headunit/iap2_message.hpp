// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

struct ControlMessage {
  std::uint16_t identifier = 0;
  std::vector<std::uint8_t> parameters;

  bool operator==(const ControlMessage&) const = default;
};

// Parses the control-session envelope carried inside iAP2 link payloads.
// It does not interpret message IDs, TLV parameters, or authentication data.
class ControlMessageReader {
 public:
  explicit ControlMessageReader(std::size_t maximum_message_bytes = 65535);
  std::vector<ControlMessage> feed(std::span<const std::uint8_t> bytes);
  std::size_t buffered_bytes() const { return pending_.size(); }

 private:
  std::size_t maximum_message_bytes_;
  std::vector<std::uint8_t> pending_;
};

std::vector<std::uint8_t> encode_control_message(const ControlMessage& message);

}  // namespace headunit
