// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_pixels.hpp>
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace headunit::camera {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
unsigned bytes_per_pixel(CapturePixelFormat format) {
  switch (format) {
    case CapturePixelFormat::yuyv:
      return 2;
    case CapturePixelFormat::rgb24:
    case CapturePixelFormat::bgr24:
      return 3;
  }
  throw std::invalid_argument("unsupported packed camera pixel format");
}
struct YuvTransform {
  std::int32_t y, red_cr, green_cb, green_cr, blue_cb;
  int y_offset;
};
YuvTransform transform(const PackedCaptureFormat& format) {
  // Inverse R'G'B' matrix from the V4L2 BT.601/709 definitions. Coefficients
  // are rounded once to 16 fractional bits; limited luma/chroma scales are
  // 255/219 and 255/224. This preserves excursions until final RGB saturation.
  // https://docs.kernel.org/userspace-api/media/v4l/colorspaces-details.html
  if (format.matrix == theme::video::YuvMatrix::bt601)
    return format.full_range ? YuvTransform{65536, 91881, 22553, 46802, 116130, 0}
                             : YuvTransform{76309, 104597, 25675, 53279, 132201, 16};
  return format.full_range ? YuvTransform{65536, 103206, 12276, 30679, 121609, 0}
                           : YuvTransform{76309, 117489, 13975, 34925, 138438, 16};
}
std::uint32_t channel(std::int32_t value) {
  if (value <= 0) return 0;
  if (value >= 255 * 65536) return 255;
  return std::uint32_t((value + 32768) / 65536);
}
std::uint32_t yuv_pixel(std::uint8_t y, int cb, int cr, const YuvTransform& t) {
  const std::int32_t luma = t.y * (int(y) - t.y_offset);
  const auto red = channel(luma + t.red_cr * cr);
  const auto green = channel(luma - t.green_cb * cb - t.green_cr * cr);
  const auto blue = channel(luma + t.blue_cb * cb);
  return 0xff000000U | (red << 16) | (green << 8) | blue;
}
}  // namespace
void validate_packed_capture_format(const PackedCaptureFormat& format) {
  check(format.width >= 64 && format.width <= 1920 && format.height >= 64 && format.height <= 1080,
        "packed camera dimensions outside 64..1920 by 64..1080");
  const auto bpp = bytes_per_pixel(format.pixel_format);
  check(format.stride >= format.width * bpp, "packed camera row stride is too short");
  // Division before multiplication prevents overflow even on 32-bit hosts.
  check(format.stride <= maximum_packed_capture_storage / format.height,
        "packed camera storage exceeds 32 MiB");
  if (format.pixel_format == CapturePixelFormat::yuyv) {
    check(format.width % 2 == 0, "YUYV camera width must be even");
    check(format.matrix == theme::video::YuvMatrix::bt601 ||
              format.matrix == theme::video::YuvMatrix::bt709,
          "packed YUYV camera supports only BT.601 and BT.709");
  }
}
theme::graphics::Image copy_capture_image(const PackedCaptureFormat& format,
                                          std::span<const std::uint8_t> source) {
  validate_packed_capture_format(format);
  const auto bpp = bytes_per_pixel(format.pixel_format);
  const auto storage = std::size_t(format.stride) * format.height;
  const auto visible_end = std::size_t(format.stride) * (format.height - 1) + format.width * bpp;
  check(source.size() >= visible_end, "packed camera source is truncated");
  check(source.size() <= storage, "packed camera source exceeds negotiated storage");
  theme::graphics::Image result{format.width, format.height, {}};
  result.pixels.resize(std::size_t(format.width) * format.height);
  if (format.pixel_format == CapturePixelFormat::yuyv) {
    const auto t = transform(format);
    for (unsigned row = 0; row < format.height; ++row) {
      const auto* in = source.data() + std::size_t(row) * format.stride;
      auto* out = result.pixels.data() + std::size_t(row) * format.width;
      for (unsigned column = 0; column < format.width; column += 2, in += 4) {
        const int cb = int(in[1]) - 128, cr = int(in[3]) - 128;
        out[column] = yuv_pixel(in[0], cb, cr, t);
        out[column + 1] = yuv_pixel(in[2], cb, cr, t);
      }
    }
  } else {
    const unsigned red = format.pixel_format == CapturePixelFormat::rgb24 ? 0 : 2;
    const unsigned blue = 2 - red;
    for (unsigned row = 0; row < format.height; ++row) {
      const auto* in = source.data() + std::size_t(row) * format.stride;
      auto* out = result.pixels.data() + std::size_t(row) * format.width;
      for (unsigned column = 0; column < format.width; ++column, in += 3)
        out[column] =
            0xff000000U | (std::uint32_t(in[red]) << 16) | (std::uint32_t(in[1]) << 8) | in[blue];
    }
  }
  return result;
}
CaptureMapping capture_mapping(unsigned source_width, unsigned source_height, unsigned width,
                               unsigned height, CaptureFit fit) {
  check(source_width >= 64 && source_width <= 1920 && source_height >= 64 && source_height <= 1080,
        "invalid camera adaptation source dimensions");
  check(width >= 64 && width <= 1920 && height >= 64 && height <= 1080,
        "camera adaptation destination exceeds bounds");
  check(fit == CaptureFit::contain || fit == CaptureFit::cover, "unknown camera adaptation fit");
  const double sx = double(width) / source_width, sy = double(height) / source_height;
  const double scale = fit == CaptureFit::contain ? std::min(sx, sy) : std::max(sx, sy);
  const double left = (double(width) - source_width * scale) / 2;
  const double top = (double(height) - source_height * scale) / 2;
  const theme::graphics::Rect image{{left, top}, {source_width * scale, source_height * scale}};
  // Keep contain's complete field and cover's complete delivery explicit rather
  // than accumulating floating-point clipping error at the limiting edge.
  const theme::graphics::Rect source =
      fit == CaptureFit::contain
          ? theme::graphics::Rect{{0, 0}, {double(source_width), double(source_height)}}
          : theme::graphics::Rect{{-left / scale, -top / scale}, {width / scale, height / scale}};
  const theme::graphics::Rect destination =
      fit == CaptureFit::contain ? image
                                 : theme::graphics::Rect{{0, 0}, {double(width), double(height)}};
  return {scale, image, source, destination};
}
theme::graphics::Image adapt_capture_image(const theme::graphics::Image& source, unsigned width,
                                           unsigned height, CaptureFit fit,
                                           CaptureMapping* consumed_mapping) {
  const auto mapping = capture_mapping(source.width, source.height, width, height, fit);
  check(source.pixels.size() == std::size_t(source.width) * source.height,
        "invalid camera adaptation source");
  const auto scale = mapping.scale;
  const auto left = mapping.image.origin.x, top = mapping.image.origin.y;
  theme::graphics::Image result{width, height, {}};
  result.pixels.resize(std::size_t(width) * height, 0xff000000U);
  if (scale == 1 && std::floor(left) == left && std::floor(top) == top) {
    // The common 1280x720 acquisition in the 1650x720 signed slot only needs
    // opaque side bars. Keep all acquired pixels and avoid a resampling pass.
    for (unsigned y = 0; y < height; ++y) {
      const auto sy = int(y) - int(top);
      if (sy < 0 || sy >= int(source.height)) continue;
      const auto start = std::max(0, int(left));
      const auto source_start = std::max(0, -int(left));
      const auto count = std::min(int(width) - start, int(source.width) - source_start);
      if (count > 0)
        std::copy_n(source.pixels.begin() + std::size_t(sy) * source.width + source_start, count,
                    result.pixels.begin() + std::size_t(y) * width + start);
    }
    if (consumed_mapping) *consumed_mapping = mapping;
    return result;
  }
  struct Sample {
    unsigned first = 0, second = 0, weight = 0;
    bool visible = false;
  };
  const auto axis = [](unsigned count, unsigned extent, double origin, double scale) {
    std::vector<Sample> samples(count);
    for (unsigned i = 0; i < count; ++i) {
      const double coordinate = (double(i) + .5 - origin) / scale;
      if (coordinate < 0 || coordinate >= extent) continue;
      const double centre = std::clamp(coordinate - .5, 0., double(extent - 1));
      const auto first = unsigned(centre);
      samples[i] = {first, std::min(first + 1, extent - 1),
                    unsigned(std::lround((centre - first) * 256)), true};
    }
    return samples;
  };
  const auto xs = axis(width, source.width, left, scale);
  const auto ys = axis(height, source.height, top, scale);
  const auto* columns = xs.data();
  for (unsigned y = 0; y < height; ++y) {
    const auto row = ys[y];
    if (!row.visible) continue;
    const auto* first_row = source.pixels.data() + std::size_t(row.first) * source.width;
    const auto* second_row = source.pixels.data() + std::size_t(row.second) * source.width;
    auto* output = result.pixels.data() + std::size_t(y) * width;
    for (unsigned x = 0; x < width; ++x) {
      const auto column = columns[x];
      if (!column.visible) continue;
      const auto a = first_row[column.first];
      const auto b = first_row[column.second];
      const auto c = second_row[column.first];
      const auto d = second_row[column.second];
      std::uint32_t pixel = 0xff000000U;
      for (unsigned shift : {0U, 8U, 16U}) {
        const auto first =
            ((a >> shift) & 255) * (256 - column.weight) + ((b >> shift) & 255) * column.weight;
        const auto second =
            ((c >> shift) & 255) * (256 - column.weight) + ((d >> shift) & 255) * column.weight;
        pixel |= ((first * (256 - row.weight) + second * row.weight + 32768) >> 16) << shift;
      }
      output[x] = pixel;
    }
  }
  if (consumed_mapping) *consumed_mapping = mapping;
  return result;
}
}  // namespace headunit::camera
