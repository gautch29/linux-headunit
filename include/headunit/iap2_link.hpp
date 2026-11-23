// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

// A decoded iAP2 link packet. The payload excludes the trailing checksum.
struct LinkFrame {
  std::uint8_t control = 0;
  std::uint8_t sequence = 0;
  std::uint8_t acknowledgement = 0;
  std::uint8_t session = 0;
  std::vector<std::uint8_t> payload;

  bool operator==(const LinkFrame&) const = default;
};

// RFCOMM is a byte stream: frames can be split or joined at any read boundary.
// This class validates frame lengths and checksums before delivering payloads.
class LinkFrameReader {
 public:
  explicit LinkFrameReader(std::size_t maximum_frame_bytes = 65535);
  std::vector<LinkFrame> feed(std::span<const std::uint8_t> bytes);
  std::size_t buffered_bytes() const { return pending_.size(); }

 private:
  std::size_t maximum_frame_bytes_;
  std::vector<std::uint8_t> pending_;
};

std::vector<std::uint8_t> encode_link_frame(const LinkFrame& frame);

}  // namespace headunit
