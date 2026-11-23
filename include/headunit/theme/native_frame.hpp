// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace headunit::theme::video {
struct DecodedMetadata {
  std::uint64_t sender_ntp, timestamp_ns;
  std::uint32_t width, height;
};
enum class YuvMatrix { bt601, bt709, bt2020 };
struct DmaPlane {
  int fd = -1;
  std::uint32_t offset = 0, stride = 0;
};
struct NativeFrame {
  DecodedMetadata metadata{};
  std::uint32_t storage_width = 0, storage_height = 0, fourcc = 0;
  std::uint64_t modifier = 0;
  std::array<DmaPlane, 4> planes{};
  unsigned plane_count = 0;
  std::uint32_t crop_x = 0, crop_y = 0;
  YuvMatrix matrix = YuvMatrix::bt709;
  bool full_range = false, chroma_x_cosited = false, chroma_y_cosited = false;
  // Own the producer sample, not just duplicated file descriptors. Retain this
  // lease through every GPU read; never close the borrowed plane descriptors.
  std::shared_ptr<const void> lifetime;
};
}  // namespace headunit::theme::video
