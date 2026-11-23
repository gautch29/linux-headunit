// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/iap2_link.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace headunit {
namespace {

constexpr std::size_t header_bytes = 9;
constexpr std::uint8_t marker_first = 0xff;
constexpr std::uint8_t marker_second = 0x5a;

std::uint8_t checksum(std::span<const std::uint8_t> data) {
  unsigned sum = 0;
  for (const auto byte : data)
    sum += byte;
  return static_cast<std::uint8_t>(0u - sum);
}

bool checksum_valid(std::span<const std::uint8_t> data) {
  unsigned sum = 0;
  for (const auto byte : data)
    sum += byte;
  return (sum & 0xffu) == 0;
}

}  // namespace

LinkFrameReader::LinkFrameReader(std::size_t maximum_frame_bytes)
    : maximum_frame_bytes_(maximum_frame_bytes) {
  if (maximum_frame_bytes < header_bytes || maximum_frame_bytes > 65535) {
    throw std::invalid_argument("iAP2 maximum frame size must be 9..65535");
  }
}

std::vector<LinkFrame> LinkFrameReader::feed(std::span<const std::uint8_t> bytes) {
  std::vector<LinkFrame> frames;
  // Fill the bounded header/body in chunks. A validated header does not need
  // its checksum recalculated for every payload byte in the same TCP read.
  const auto append = [&](std::size_t wanted) {
    const auto count = std::min(wanted, bytes.size());
    if (!count) return;
    pending_.insert(pending_.end(), bytes.begin(), bytes.begin() + count);
    bytes = bytes.subspan(count);
  };
  const auto resynchronize = [&] {
    const auto next = std::find(pending_.begin() + 1, pending_.end(), marker_first);
    pending_.erase(pending_.begin(), next);
  };
  for (;;) {
    if (pending_.empty()) {
      if (bytes.empty()) break;
      const auto first = std::find(bytes.begin(), bytes.end(), marker_first);
      bytes = bytes.subspan(static_cast<std::size_t>(first - bytes.begin()));
      if (bytes.empty()) break;
    }
    if (pending_.size() < 2) append(2 - pending_.size());
    if (pending_.size() < 2) break;
    if (pending_[1] != marker_second) {
      resynchronize();
      continue;
    }
    if (pending_.size() < header_bytes) append(header_bytes - pending_.size());
    if (pending_.size() < header_bytes) break;
    const std::size_t total = (std::size_t(pending_[2]) << 8) | pending_[3];
    if (total < header_bytes || total > maximum_frame_bytes_ ||
        !checksum_valid(std::span(pending_.data(), header_bytes))) {
      resynchronize();
      continue;
    }
    if (pending_.size() < total) append(total - pending_.size());
    if (pending_.size() < total) break;
    if (total > header_bytes &&
        !checksum_valid(std::span(pending_.data() + header_bytes, total - header_bytes))) {
      resynchronize();
      continue;
    }
    LinkFrame frame{pending_[4], pending_[5], pending_[6], pending_[7], {}};
    if (total > header_bytes) {
      frame.payload.assign(pending_.begin() + header_bytes, pending_.begin() + total - 1);
    }
    frames.push_back(std::move(frame));
    pending_.erase(pending_.begin(), pending_.begin() + total);
  }
  return frames;
}

std::vector<std::uint8_t> encode_link_frame(const LinkFrame& frame) {
  const std::size_t total = header_bytes + (frame.payload.empty() ? 0 : frame.payload.size() + 1);
  if (total > 65535) throw std::invalid_argument("iAP2 frame exceeds 16-bit length");
  std::vector<std::uint8_t> bytes{marker_first,
                                  marker_second,
                                  static_cast<std::uint8_t>(total >> 8),
                                  static_cast<std::uint8_t>(total),
                                  frame.control,
                                  frame.sequence,
                                  frame.acknowledgement,
                                  frame.session,
                                  0};
  bytes[8] = checksum(std::span(bytes.data(), 8));
  if (!frame.payload.empty()) {
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
    bytes.push_back(checksum(frame.payload));
  }
  return bytes;
}

}  // namespace headunit
