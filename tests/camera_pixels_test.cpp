// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_pixels.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace c = headunit::camera;
namespace v = headunit::theme::video;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F&& action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid packed capture accepted");
}
c::PackedCaptureFormat format(c::CapturePixelFormat pixel = c::CapturePixelFormat::yuyv) {
  return {64,   64, pixel == c::CapturePixelFormat::yuyv ? 128U : 192U, pixel, v::YuvMatrix::bt601,
          false};
}
std::size_t visible_end(const c::PackedCaptureFormat& f) {
  return std::size_t(f.stride) * (f.height - 1) +
         f.width * (f.pixel_format == c::CapturePixelFormat::yuyv ? 2U : 3U);
}
void expect_color(std::uint32_t actual, std::uint32_t expected, unsigned tolerance,
                  const char* why) {
  check((actual >> 24) == 255, "copied camera image is not opaque");
  for (unsigned shift : {0U, 8U, 16U}) {
    const auto got = (actual >> shift) & 255U, want = (expected >> shift) & 255U;
    if (got > want + tolerance || want > got + tolerance)
      throw std::runtime_error(std::string(why) + ": actual=" + std::to_string(actual) +
                               ", expected=" + std::to_string(expected));
  }
}
std::vector<std::uint8_t> solid_yuyv(const c::PackedCaptureFormat& f, unsigned y, unsigned u,
                                     unsigned v) {
  std::vector<std::uint8_t> bytes(visible_end(f), 0xe7);
  for (unsigned row = 0; row < f.height; ++row)
    for (unsigned column = 0; column < f.width; column += 2) {
      auto* p = bytes.data() + std::size_t(row) * f.stride + column * 2;
      p[0] = std::uint8_t(y);
      p[1] = std::uint8_t(u);
      p[2] = std::uint8_t(y);
      p[3] = std::uint8_t(v);
    }
  return bytes;
}
void validation() {
  auto f = format();
  c::validate_packed_capture_format(f);
  for (const auto& [width, height] :
       std::array<std::pair<unsigned, unsigned>, 8>{{{0, 64},
                                                     {63, 64},
                                                     {1921, 64},
                                                     {64, 0},
                                                     {64, 63},
                                                     {64, 1081},
                                                     {std::numeric_limits<unsigned>::max(), 64},
                                                     {64, std::numeric_limits<unsigned>::max()}}}) {
    auto bad = f;
    bad.width = width;
    bad.height = height;
    rejects([&] { c::validate_packed_capture_format(bad); });
  }
  for (const auto stride : {0U, 127U, std::numeric_limits<unsigned>::max()}) {
    auto bad = f;
    bad.stride = stride;
    rejects([&] { c::validate_packed_capture_format(bad); });
  }
  auto bad = f;
  bad.width = 65;
  bad.stride = 130;
  rejects([&] { c::validate_packed_capture_format(bad); });
  bad = f;
  bad.matrix = v::YuvMatrix::bt2020;
  rejects([&] { c::validate_packed_capture_format(bad); });
  bad.matrix = static_cast<v::YuvMatrix>(99);
  rejects([&] { c::validate_packed_capture_format(bad); });
  bad = f;
  bad.pixel_format = static_cast<c::CapturePixelFormat>(99);
  rejects([&] { c::validate_packed_capture_format(bad); });
  f.stride = unsigned(c::maximum_packed_capture_storage / f.height);
  c::validate_packed_capture_format(f);
  ++f.stride;
  rejects([&] { c::validate_packed_capture_format(f); });
  f = format(c::CapturePixelFormat::rgb24);
  f.width = 65;
  f.stride = 195;
  c::validate_packed_capture_format(f);
  // RGB bytes do not use a YUV matrix or range conversion.
  f.matrix = v::YuvMatrix::bt2020;
  c::validate_packed_capture_format(f);
}
void storage_bounds() {
  for (const auto kind :
       {c::CapturePixelFormat::yuyv, c::CapturePixelFormat::rgb24, c::CapturePixelFormat::bgr24}) {
    auto f = format(kind);
    f.stride += 17;
    std::vector<std::uint8_t> bytes(visible_end(f), 128);
    const auto copied = c::copy_capture_image(f, bytes);
    check(copied.width == f.width && copied.height == f.height &&
              copied.pixels.size() == std::size_t(f.width) * f.height,
          "capture image dimensions/storage changed");
    rejects([&] {
      c::copy_capture_image(f, std::span<const std::uint8_t>(bytes).first(bytes.size() - 1));
    });
    rejects([&] { c::copy_capture_image(f, {}); });
    bytes.resize(std::size_t(f.stride) * f.height, 0x17);
    check(c::copy_capture_image(f, bytes).pixels == copied.pixels,
          "last-row padding affected visible pixels");
    bytes.push_back(0);
    rejects([&] { c::copy_capture_image(f, bytes); });
  }
  auto f = format();
  f.width = 1920;
  f.height = 1080;
  f.stride = 3840;
  auto bytes = solid_yuyv(f, 235, 128, 128);
  auto copied = c::copy_capture_image(f, bytes);
  check(copied.pixels.size() == 1920U * 1080U && copied.pixels.front() == 0xffffffffU &&
            copied.pixels.back() == 0xffffffffU,
        "maximum supported capture does not include its corners");
}
void rgb_order_and_padding() {
  for (const auto kind : {c::CapturePixelFormat::rgb24, c::CapturePixelFormat::bgr24}) {
    auto f = format(kind);
    f.width = 65;
    f.height = 67;
    f.stride = 208;
    std::vector<std::uint8_t> bytes(visible_end(f), 0xab);
    for (unsigned row = 0; row < f.height; ++row)
      for (unsigned column = 0; column < f.width; ++column) {
        auto* p = bytes.data() + std::size_t(row) * f.stride + column * 3;
        const auto red = std::uint8_t(column + 1), green = std::uint8_t(row + 2),
                   blue = std::uint8_t(column + row + 3);
        p[0] = kind == c::CapturePixelFormat::rgb24 ? red : blue;
        p[1] = green;
        p[2] = kind == c::CapturePixelFormat::rgb24 ? blue : red;
      }
    const auto image = c::copy_capture_image(f, bytes);
    expect_color(image.pixels.front(), 0xff010203U, 0, "top-left RGB corner");
    expect_color(image.pixels[f.width - 1], 0xff410243U, 0, "top-right RGB corner");
    expect_color(image.pixels[std::size_t(f.height - 1) * f.width], 0xff014445U, 0,
                 "bottom-left RGB corner");
    expect_color(image.pixels.back(), 0xff414485U, 0, "bottom-right RGB corner");
    // Alter every available padding byte without touching the visible rows.
    for (unsigned row = 0; row + 1 < f.height; ++row)
      std::fill(bytes.begin() + std::size_t(row) * f.stride + f.width * 3,
                bytes.begin() + std::size_t(row + 1) * f.stride, 0x3d);
    check(c::copy_capture_image(f, bytes).pixels == image.pixels,
          "row padding leaked into RGB capture");
    std::fill(bytes.begin(), bytes.end(), 0);
    expect_color(image.pixels.back(), 0xff414485U, 0, "source reuse changed an owned RGB image");
  }
}
void yuyv_layout_and_neutral() {
  for (const auto matrix : {v::YuvMatrix::bt601, v::YuvMatrix::bt709})
    for (const bool full : {false, true}) {
      auto f = format();
      f.matrix = matrix;
      f.full_range = full;
      f.stride += 19;
      auto bytes = solid_yuyv(f, full ? 0 : 16, 128, 128);
      // Two luma values share chroma in each Y0,U,Y1,V container.
      for (unsigned row = 0; row < f.height; ++row)
        for (unsigned column = 0; column < f.width; column += 2)
          bytes[std::size_t(row) * f.stride + column * 2 + 2] = full ? 255 : 235;
      const auto image = c::copy_capture_image(f, bytes);
      for (unsigned row = 0; row < f.height; ++row)
        for (unsigned column = 0; column < f.width; ++column)
          expect_color(image.pixels[std::size_t(row) * f.width + column],
                       column % 2 ? 0xffffffffU : 0xff000000U, 0,
                       "YUYV byte order or neutral range");
      for (unsigned row = 0; row + 1 < f.height; ++row)
        std::fill(bytes.begin() + std::size_t(row) * f.stride + f.width * 2,
                  bytes.begin() + std::size_t(row + 1) * f.stride, 0);
      check(c::copy_capture_image(f, bytes).pixels == image.pixels,
            "row padding leaked into YUYV capture");
      if (!full) {
        auto below = solid_yuyv(f, 0, 128, 128), above = solid_yuyv(f, 255, 128, 128);
        expect_color(c::copy_capture_image(f, below).pixels.front(), 0xff000000U, 0,
                     "below-black luma saturation");
        expect_color(c::copy_capture_image(f, above).pixels.back(), 0xffffffffU, 0,
                     "above-white luma saturation");
      }
    }
}
struct ColorBar {
  unsigned y, u, v;
  std::uint32_t rgb;
};
void color_bars() {
  // Quantized 100% R/G/B/Y/C/M reference bars, not an inverse-coefficient
  // calculation duplicated from the converter. One LSB covers quantization.
  // Definitions: kernel V4L2 colorspaces-details and packed-yuv documentation.
  constexpr std::array<ColorBar, 6> bt601_limited{{{81, 90, 240, 0xffff0000U},
                                                   {145, 54, 34, 0xff00ff00U},
                                                   {41, 240, 110, 0xff0000ffU},
                                                   {210, 16, 146, 0xffffff00U},
                                                   {170, 166, 16, 0xff00ffffU},
                                                   {106, 202, 222, 0xffff00ffU}}};
  constexpr std::array<ColorBar, 6> bt601_full{{{76, 85, 255, 0xffff0000U},
                                                {150, 44, 21, 0xff00ff00U},
                                                {29, 255, 107, 0xff0000ffU},
                                                {226, 1, 149, 0xffffff00U},
                                                {179, 171, 0, 0xff00ffffU},
                                                {105, 212, 235, 0xffff00ffU}}};
  constexpr std::array<ColorBar, 6> bt709_limited{{{63, 102, 240, 0xffff0000U},
                                                   {173, 42, 26, 0xff00ff00U},
                                                   {32, 240, 118, 0xff0000ffU},
                                                   {219, 16, 138, 0xffffff00U},
                                                   {188, 154, 16, 0xff00ffffU},
                                                   {78, 214, 230, 0xffff00ffU}}};
  constexpr std::array<ColorBar, 6> bt709_full{{{54, 99, 255, 0xffff0000U},
                                                {182, 30, 12, 0xff00ff00U},
                                                {18, 255, 116, 0xff0000ffU},
                                                {237, 1, 140, 0xffffff00U},
                                                {201, 157, 1, 0xff00ffffU},
                                                {73, 226, 244, 0xffff00ffU}}};
  for (const auto matrix : {v::YuvMatrix::bt601, v::YuvMatrix::bt709})
    for (const bool full : {false, true}) {
      auto f = format();
      f.matrix = matrix;
      f.full_range = full;
      const auto& bars = matrix == v::YuvMatrix::bt601 ? (full ? bt601_full : bt601_limited)
                                                       : (full ? bt709_full : bt709_limited);
      for (const auto& bar : bars) {
        const auto bytes = solid_yuyv(f, bar.y, bar.u, bar.v);
        const auto image = c::copy_capture_image(f, bytes);
        expect_color(image.pixels.front(), bar.rgb, 1, "canonical color bar");
        expect_color(image.pixels.back(), bar.rgb, 1, "canonical color bar final corner");
      }
    }
  auto f = format();
  const auto bytes = solid_yuyv(f, 81, 90, 240);
  const auto sd = c::copy_capture_image(f, bytes);
  f.matrix = v::YuvMatrix::bt709;
  const auto hd = c::copy_capture_image(f, bytes);
  expect_color(sd.pixels.front(), 0xfffe0000U, 0, "BT.601 matrix-specific vector");
  expect_color(hd.pixels.front(), 0xffff1800U, 0, "BT.709 matrix-specific vector");
  check(sd.pixels != hd.pixels, "BT.601 and BT.709 were treated as interchangeable");
}
void queue_ownership() {
  const auto f = format(c::CapturePixelFormat::rgb24);
  std::vector<std::uint8_t> bytes(visible_end(f), 0);
  bytes[0] = 23;
  bytes[1] = 45;
  bytes[2] = 67;
  auto first = c::copy_capture_image(f, bytes);
  c::Configuration config;
  config.enabled = true;
  config.source = c::SourceKind::external;
  config.width = f.width;
  config.height = f.height;
  config.canvas_width = f.width;
  config.canvas_height = f.height;
  config.destination = {0, 0, f.width, f.height};
  c::Queue queue(config);
  check(queue.offer_image(first, 100, 100, false) && first.pixels.empty(),
        "capture copy was not moved into the queue");
  const auto retained = queue.stage(100);
  check(bool(retained), "capture copy not staged");
  check(queue.complete(retained->sequence, true), "capture copy not presented by queue");
  bytes[0] = 77;
  bytes[1] = 88;
  bytes[2] = 99;
  auto second = c::copy_capture_image(f, bytes);
  check(queue.offer_image(second, 101, 101, false), "second capture copy rejected");
  const auto next = queue.stage(101);
  check(bool(next), "second capture not staged");
  check(queue.complete(next->sequence, true), "second capture not completed");
  queue.discard();
  std::fill(bytes.begin(), bytes.end(), 0xff);
  expect_color(retained->image->pixels.front(), 0xff172d43U, 0,
               "retained camera image mutated after buffer reuse");
  expect_color(next->image->pixels.front(), 0xff4d5863U, 0,
               "independent camera image shares source storage");
  check(retained->image != next->image && !retained->native && !retained->simulated,
        "CPU capture ownership was confused with a native/simulated frame");
}
void aspect_adaptation() {
  headunit::theme::graphics::Image image{128, 64, {}};
  image.pixels.resize(128 * 64, 0xff00ff00U);
  for (unsigned y = 0; y < 64; ++y) {
    std::fill_n(image.pixels.begin() + y * 128, 32, 0xffff0000U);
    std::fill_n(image.pixels.begin() + y * 128 + 96, 32, 0xff0000ffU);
  }
  auto contain = c::adapt_capture_image(image, 128, 128, c::CaptureFit::contain);
  check(contain.pixels[31 * 128 + 64] == 0xff000000U &&
            contain.pixels[32 * 128 + 16] == 0xffff0000U &&
            contain.pixels[95 * 128 + 112] == 0xff0000ffU &&
            contain.pixels[96 * 128 + 64] == 0xff000000U,
        "contain stretched or clipped the camera field of view");
  auto cover = c::adapt_capture_image(image, 64, 64, c::CaptureFit::cover);
  check(std::all_of(cover.pixels.begin(), cover.pixels.end(),
                    [](auto p) { return p == 0xff00ff00U; }),
        "cover did not remove the centred side crop");
  check(c::adapt_capture_image(image, 128, 64, c::CaptureFit::contain).pixels == image.pixels,
        "identity adaptation changed acquired pixels");
  // Bilinear enlargement: fixed source black/white boundary has intermediate pixels.
  std::fill(image.pixels.begin(), image.pixels.end(), 0xff000000U);
  for (unsigned y = 0; y < 64; ++y)
    std::fill_n(image.pixels.begin() + y * 128 + 64, 64, 0xffffffffU);
  c::CaptureMapping consumed;
  auto smooth = c::adapt_capture_image(image, 256, 128, c::CaptureFit::contain, &consumed);
  check(smooth.pixels[127] == 0xff404040U && smooth.pixels[128] == 0xffbfbfbfU,
        "adaptation failed to interpolate pixel centres");
  check(consumed.scale == 2 && consumed.source.size.width == 128 &&
            consumed.destination.size.width == 256 && consumed.destination.size.height == 128,
        "successful resampling omitted its consumed mapping");
  rejects([&] { c::adapt_capture_image(image, 0, 64, c::CaptureFit::contain); });
  rejects([&] { c::adapt_capture_image(image, 1921, 64, c::CaptureFit::contain); });
  rejects([&] { c::adapt_capture_image(image, 64, 64, c::CaptureFit(99)); });
  image.pixels.pop_back();
  consumed.scale = 17;
  rejects([&] { c::adapt_capture_image(image, 64, 64, c::CaptureFit::cover, &consumed); });
  check(consumed.scale == 17, "failed adaptation overwrote consumed geometry output");
}
void fit_geometry_and_pixels() {
  const auto rect = [](const auto& got, double x, double y, double width, double height) {
    check(std::abs(got.origin.x - x) < 1e-9 && std::abs(got.origin.y - y) < 1e-9 &&
              std::abs(got.size.width - width) < 1e-9 && std::abs(got.size.height - height) < 1e-9,
          "capture mapping differs from independent fit rectangle");
  };
  // Current USB acquisition and signed input delivery. This is not scene fit.
  auto m = c::capture_mapping(640, 360, 1650, 720, c::CaptureFit::contain);
  check(m.scale == 2, "USB contain scale changed");
  rect(m.image, 185, 0, 1280, 720);
  rect(m.destination, 185, 0, 1280, 720);
  rect(m.source, 0, 0, 640, 360);
  m = c::capture_mapping(640, 360, 1650, 720, c::CaptureFit::cover);
  check(m.scale == 2.578125, "USB cover scale changed");
  rect(m.image, 0, -104.0625, 1650, 928.125);
  rect(m.destination, 0, 0, 1650, 720);
  rect(m.source, 0, 40.36363636363636, 640, 279.27272727272725);
  m = c::capture_mapping(64, 128, 128, 64, c::CaptureFit::contain);
  rect(m.destination, 48, 0, 32, 64);
  rect(m.source, 0, 0, 64, 128);
  m = c::capture_mapping(64, 128, 128, 64, c::CaptureFit::cover);
  rect(m.image, 0, -96, 128, 256);
  rect(m.source, 0, 48, 64, 32);
  // Half-pixel edges remain fractional. Existing half-open pixel-centre
  // sampling is preserved, rather than rounding placement to an integer.
  m = c::capture_mapping(64, 64, 65, 64, c::CaptureFit::contain);
  rect(m.image, .5, 0, 64, 64);
  headunit::theme::graphics::Image solid{64, 64, std::vector<std::uint32_t>(4096, 0xff123456)};
  const auto odd = c::adapt_capture_image(solid, 65, 64, c::CaptureFit::contain);
  for (unsigned y = 0; y < 64; ++y) {
    check(odd.pixels[y * 65] == 0xff123456 && odd.pixels[y * 65 + 63] == 0xff123456 &&
              odd.pixels[y * 65 + 64] == 0xff000000,
          "fractional contain changed pixel-centre boundary policy");
  }
  // Portrait source with independently recognizable borders and centre.
  headunit::theme::graphics::Image portrait{64, 128, std::vector<std::uint32_t>(64 * 128)};
  for (unsigned y = 0; y < 128; ++y)
    for (unsigned x = 0; x < 64; ++x)
      portrait.pixels[y * 64 + x] = y < 32 ? 0xffff0000 : y >= 96 ? 0xff0000ff : 0xff00ff00;
  const auto cover = c::adapt_capture_image(portrait, 128, 64, c::CaptureFit::cover);
  check(
      std::all_of(cover.pixels.begin(), cover.pixels.end(), [](auto p) { return p == 0xff00ff00; }),
      "portrait cover stretched source or retained removed border pixels");
  const auto contain = c::adapt_capture_image(portrait, 128, 64, c::CaptureFit::contain);
  check(contain.pixels[47] == 0xff000000 && contain.pixels[48] == 0xffff0000 &&
            contain.pixels[63 * 128 + 79] == 0xff0000ff &&
            contain.pixels[63 * 128 + 80] == 0xff000000,
        "portrait contain lost full field or opaque bars");
  // Bounds/aspect/centering over formats, without allocating pixel images.
  for (const auto& [sw, sh] : std::array<std::pair<unsigned, unsigned>, 5>{
           {{64, 1080}, {1920, 64}, {640, 360}, {65, 127}, {1920, 1080}}})
    for (const auto& [dw, dh] : std::array<std::pair<unsigned, unsigned>, 5>{
             {{64, 64}, {1650, 720}, {1024, 768}, {65, 127}, {1920, 1080}}})
      for (auto fit : {c::CaptureFit::contain, c::CaptureFit::cover}) {
        const auto mapping = c::capture_mapping(sw, sh, dw, dh, fit);
        const auto& s = mapping.source;
        const auto& d = mapping.destination;
        check(s.origin.x >= -1e-9 && s.origin.y >= -1e-9 &&
                  s.origin.x + s.size.width <= sw + 1e-9 &&
                  s.origin.y + s.size.height <= sh + 1e-9 && d.origin.x >= -1e-9 &&
                  d.origin.y >= -1e-9 && d.origin.x + d.size.width <= dw + 1e-9 &&
                  d.origin.y + d.size.height <= dh + 1e-9 &&
                  std::abs(s.origin.x + s.size.width / 2 - sw / 2.) < 1e-9 &&
                  std::abs(d.origin.y + d.size.height / 2 - dh / 2.) < 1e-9 &&
                  std::abs(d.size.width / s.size.width - d.size.height / s.size.height) < 1e-9,
              "fit mapping exceeded bounds, lost centre or stretched aspect");
      }
  for (unsigned bad : {0U, 63U, 1921U, std::numeric_limits<unsigned>::max()}) {
    rejects([&] { c::capture_mapping(bad, 64, 64, 64, c::CaptureFit::contain); });
    rejects([&] { c::capture_mapping(64, 64, bad, 64, c::CaptureFit::cover); });
  }
  for (unsigned bad : {0U, 63U, 1081U, std::numeric_limits<unsigned>::max()}) {
    rejects([&] { c::capture_mapping(64, bad, 64, 64, c::CaptureFit::contain); });
    rejects([&] { c::capture_mapping(64, 64, 64, bad, c::CaptureFit::cover); });
  }
  rejects([&] { c::capture_mapping(64, 64, 64, 64, c::CaptureFit(99)); });
}
// Full software-image corpus checked against an independent scalar contract.
// Private qualification also compares every output pixel and mapping bit to the
// unchanged original mapper compiled separately with GCC and Clang.
struct AdaptationSize {
  unsigned source_width, source_height, delivery_width, delivery_height;
};
constexpr std::array<AdaptationSize, 10> adaptation_sizes{{{640, 360, 1650, 720},
                                                           {1280, 720, 1650, 720},
                                                           {64, 128, 128, 64},
                                                           {64, 64, 65, 64},
                                                           {65, 127, 1024, 768},
                                                           {1920, 64, 64, 1080},
                                                           {64, 1080, 1920, 64},
                                                           {1920, 1080, 1920, 1080},
                                                           {64, 64, 1920, 1080},
                                                           {127, 65, 1919, 1079}}};
auto adaptation_source(AdaptationSize s) {
  headunit::theme::graphics::Image image{s.source_width, s.source_height, {}};
  image.pixels.resize(std::size_t(s.source_width) * s.source_height);
  std::uint32_t value = 0x29e5a71bU ^ s.source_width ^ (s.source_height << 4) ^
                        (s.delivery_width << 8) ^ (s.delivery_height << 16);
  for (unsigned y = 0; y < s.source_height; ++y)
    for (unsigned x = 0; x < s.source_width; ++x) {
      value = value * 1664525U + 1013904223U;
      auto pixel = value;  // Includes varying source alpha and asymmetric RGB.
      if (x == 0) pixel = 0xffff0000U;
      if (x + 1 == s.source_width) pixel = 0xff0000ffU;
      if (y == 0) pixel = 0xff00ff00U;
      if (y + 1 == s.source_height) pixel = 0xffffffffU;
      image.pixels[std::size_t(y) * s.source_width + x] = pixel;
    }
  return image;
}
std::uint64_t adaptation_hash(const std::vector<std::uint32_t>& pixels) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (auto pixel : pixels)
    for (unsigned shift : {0U, 8U, 16U, 24U}) {
      hash ^= (pixel >> shift) & 255U;
      hash *= 1099511628211ULL;
    }
  return hash;
}
// Deliberately scalar software oracle: derive the four taps directly for each
// output pixel, without the production row-address or axis-array optimization.
std::uint32_t reference_adaptation_pixel(const headunit::theme::graphics::Image& source, unsigned x,
                                         unsigned y, const c::CaptureMapping& mapping) {
  if (mapping.scale == 1 && std::floor(mapping.image.origin.x) == mapping.image.origin.x &&
      std::floor(mapping.image.origin.y) == mapping.image.origin.y) {
    const auto ix = int(x) - int(mapping.image.origin.x);
    const auto iy = int(y) - int(mapping.image.origin.y);
    if (ix < 0 || iy < 0 || ix >= int(source.width) || iy >= int(source.height)) return 0xff000000U;
    return source.pixels[std::size_t(iy) * source.width + unsigned(ix)];
  }
  const double sx = (double(x) + .5 - mapping.image.origin.x) / mapping.scale;
  const double sy = (double(y) + .5 - mapping.image.origin.y) / mapping.scale;
  if (sx < 0 || sx >= source.width || sy < 0 || sy >= source.height) return 0xff000000U;
  const double cx = std::clamp(sx - .5, 0., double(source.width - 1));
  const double cy = std::clamp(sy - .5, 0., double(source.height - 1));
  const auto x0 = unsigned(cx);
  const auto y0 = unsigned(cy);
  const auto x1 = std::min(x0 + 1, source.width - 1);
  const auto y1 = std::min(y0 + 1, source.height - 1);
  const auto wx = unsigned(std::lround((cx - x0) * 256));
  const auto wy = unsigned(std::lround((cy - y0) * 256));
  const auto a = source.pixels[std::size_t(y0) * source.width + x0];
  const auto b = source.pixels[std::size_t(y0) * source.width + x1];
  const auto d = source.pixels[std::size_t(y1) * source.width + x1];
  const auto e = source.pixels[std::size_t(y1) * source.width + x0];
  std::uint32_t result = 0xff000000U;
  for (unsigned shift : {0U, 8U, 16U}) {
    const auto upper = ((a >> shift) & 255U) * (256 - wx) + ((b >> shift) & 255U) * wx;
    const auto lower = ((e >> shift) & 255U) * (256 - wx) + ((d >> shift) & 255U) * wx;
    result |= ((upper * (256 - wy) + lower * wy + 32768) >> 16) << shift;
  }
  return result;
}

void pixel_exact_adaptation_vectors() {
  unsigned index = 0;
  for (const auto size : adaptation_sizes)
    for (const auto fit : {c::CaptureFit::contain, c::CaptureFit::cover}) {
      const auto source = adaptation_source(size);
      const auto mapping = c::capture_mapping(size.source_width, size.source_height,
                                              size.delivery_width, size.delivery_height, fit);
      const auto result =
          c::adapt_capture_image(source, size.delivery_width, size.delivery_height, fit);
      for (unsigned y = 0; y < size.delivery_height; ++y)
        for (unsigned x = 0; x < size.delivery_width; ++x)
          check(result.pixels[std::size_t(y) * size.delivery_width + x] ==
                    reference_adaptation_pixel(source, x, y, mapping),
                "adaptation changed scalar source samples, bars, alpha or rounding");
      std::cout << "adaptation-case " << index++ << " hash " << adaptation_hash(result.pixels)
                << '\n';
    }
}
int main() try {
  validation();
  storage_bounds();
  rgb_order_and_padding();
  yuyv_layout_and_neutral();
  color_bars();
  queue_ownership();
  aspect_adaptation();
  fit_geometry_and_pixels();
  pixel_exact_adaptation_vectors();
  std::cout << "packed camera pixels: 9 groups passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
