// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/scene.hpp>
#include <algorithm>
#include <stdexcept>

namespace headunit::theme::graphics {
AlphaMask::AlphaMask(const Image& source, std::int32_t x, std::int32_t y) {
  if (!source.width || !source.height || source.width > 4096 || source.height > 2160 ||
      source.pixels.size() != std::size_t(source.width) * source.height || x < -10000000 ||
      x > 10000000 || y < -10000000 || y > 10000000)
    throw std::invalid_argument("invalid alpha-mask image or origin");
  auto x0 = source.width, y0 = source.height;
  std::uint32_t x1 = 0, y1 = 0;
  for (std::uint32_t sy = 0; sy < source.height; ++sy)
    for (std::uint32_t sx = 0; sx < source.width; ++sx)
      if (source.pixels[std::size_t(sy) * source.width + sx] >> 24) {
        x0 = std::min(x0, sx);
        y0 = std::min(y0, sy);
        x1 = std::max(x1, sx + 1);
        y1 = std::max(y1, sy + 1);
      }
  if (x1 <= x0 || y1 <= y0) return;
  x_ = x + std::int32_t(x0);
  y_ = y + std::int32_t(y0);
  width_ = x1 - x0;
  height_ = y1 - y0;
  pixels_.resize(std::size_t(width_) * height_);
  for (std::uint32_t sy = 0; sy < height_; ++sy)
    for (std::uint32_t sx = 0; sx < width_; ++sx)
      pixels_[std::size_t(sy) * width_ + sx] =
          source.pixels[std::size_t(sy + y0) * source.width + sx + x0] >> 24;
}
}  // namespace headunit::theme::graphics
