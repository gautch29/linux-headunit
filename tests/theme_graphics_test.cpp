// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/lua_host.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace headunit::theme;
using Bytes = headunit::LuaHost::Bytes;
headunit::LuaHost::ResourceReader read_fixture(const std::filesystem::path& directory) {
  return [directory](const std::string& name) {
    std::ifstream file(directory / name, std::ios::binary);
    if (!file) throw std::runtime_error("missing synthetic test asset");
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };
}
namespace g = graphics;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F fn) {
  bool bad = false;
  try {
    fn();
  } catch (const std::exception&) {
    bad = true;
  }
  CHECK(bad);
}
std::shared_ptr<g::Layer> box(double x, double y, double w, double h, g::Color color) {
  auto p = std::make_shared<g::Layer>();
  p->set_frame({{x, y}, {w, h}});
  p->background = color;
  return p;
}
std::uint32_t pixel(const g::Image& image, int x, int y) {
  return image.pixels[y * image.width + x];
}
void near(std::uint32_t actual, std::uint32_t expected, unsigned tolerance = 1) {
  for (unsigned i = 0; i < 32; i += 8)
    if (std::abs(int((actual >> i) & 255) - int((expected >> i) & 255)) > int(tolerance)) {
      std::cerr << "pixel actual=" << std::hex << actual << " expected=" << expected << std::dec
                << " tolerance=" << tolerance << '\n';
      CHECK(false);
    }
}
void continuous_corner_pixels() {
  auto root = box(0, 0, 100, 100, {});
  auto popup = box(10, 10, 80, 80, {1, 0, 0, 1});
  popup->corner_radius = 30;
  root->insert(popup, 0);
  const auto circular = g::render(root, 100, 100);
  popup->continuous_corners = true;
  const auto continuous = g::render(root, 100, 100);
  CHECK(continuous.pixels != circular.pixels);
  near(pixel(continuous, 10, 10), 0);
  near(pixel(continuous, 50, 10), 0xffff0000);
  near(pixel(continuous, 50, 50), 0xffff0000);
  CHECK((pixel(continuous, 17, 17) >> 24) > (pixel(circular, 17, 17) >> 24));
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 100; ++x) {
      near(pixel(continuous, x, y), pixel(continuous, 99 - x, y), 8);
      near(pixel(continuous, x, y), pixel(continuous, y, x), 8);
    }
  popup->clip = true;
  popup->background = {};
  popup->insert(box(-20, -20, 120, 120, {0, 1, 0, 1}), 0);
  const auto clipped = g::render(root, 100, 100);
  near(pixel(clipped, 10, 10), 0);
  near(pixel(clipped, 50, 50), 0xff00ff00);
  near(pixel(clipped, 9, 50), 0);
  popup->corner_radius = 400;
  const auto oversized = g::render(root, 100, 100);
  popup->corner_radius = 40;
  CHECK(g::render(root, 100, 100).pixels == oversized.pixels);
  popup->corner_radius = 0;
  const auto square = g::render(root, 100, 100);
  popup->continuous_corners = false;
  CHECK(g::render(root, 100, 100).pixels == square.pixels);
  popup->corner_radius = -1;
  rejects([&] { (void)g::render(root, 100, 100); });
}
void transforms_hierarchy_and_alpha() {
  auto root = box(0, 0, 100, 100, {0, 0, 0, 1});
  auto group = box(10, 10, 40, 40, {});
  group->opacity = .5;
  root->insert(group, 0);
  auto red = box(0, 0, 30, 30, {1, 0, 0, 1}), blue = box(10, 0, 30, 30, {0, 0, 1, 1});
  group->insert(red, 0);
  group->insert(blue, 1);
  auto image = g::render(root, 100, 100);
  near(pixel(image, 15, 15), 0xff800000);
  near(pixel(image, 25, 15), 0xff000080);
  group->group_blending = false;
  image = g::render(root, 100, 100);
  near(pixel(image, 25, 15), 0xff400080);
  group->opacity = 1;
  group->clip = true;
  blue->set_frame({{30, 0}, {30, 30}});
  image = g::render(root, 100, 100);
  CHECK(pixel(image, 55, 15) == 0xff000000);
  group->bounds.origin = {5, 5};
  image = g::render(root, 100, 100);
  CHECK(pixel(image, 10, 10) == 0xffff0000);  // Bounds origin scrolls children inside fixed frame.
  rejects([&] { red->insert(root, 0); });
  CHECK(root->parent.expired());
  group->insert(red, group->children.size());
  CHECK(group->children.back() == red);
  red->detach();
  CHECK(red->parent.expired() && group->children.size() == 1);
  auto r = box(40, 40, 20, 10, {0, 1, 0, 1});
  r->transform = {0, 1, -1, 0, 0, 0};
  root->insert(r, 1);
  const auto frame = r->frame();
  CHECK(frame.origin.x == 45 && frame.origin.y == 35 && frame.size.width == 10 &&
        frame.size.height == 20);
  image = g::render(root, 100, 100);
  CHECK(pixel(image, 50, 40) == 0xff00ff00);
  r->set_frame({{25, 15}, frame.size});
  CHECK(r->frame().origin.x == 25 && r->frame().origin.y == 15);
  rejects([&] { r->set_frame({{25, 15}, {7, 9}}); });
  r->transform.tx = 3;
  r->transform.ty = -4;
  r->set_frame({{30, 60}, {0, 0}});
  CHECK(r->bounds.size.width == 0 && r->bounds.size.height == 0 && r->frame().origin.x == 30 &&
        r->frame().origin.y == 60);
  r->transform = {1, 0, 0, 1, 3, -4};
  r->set_frame({{12, 15}, {20, 10}});
  CHECK(r->frame().origin.x == 12 && r->frame().origin.y == 15);
}
void opaque_group_destination_out() {
  auto root = box(0, 0, 100, 100, {0, 0, 1, 1});
  auto gauge = box(10, 10, 70, 70, {0, 1, 0, 1});
  root->insert(gauge, 0);
  auto band = box(10, 10, 40, 30, {1, 0, 0, 1});
  gauge->insert(band, 0);
  auto cut = box(10, 0, 20, 30, {1, 1, 1, 1});
  cut->blend = g::Blend::dest_out;
  band->insert(cut, 0);
  auto image = g::render(root, 100, 100);
  // The child cuts only its own band's destination, revealing the intact gauge
  // face. Erasing the scene instead would reveal/clear the unrelated map below.
  near(pixel(image, 25, 25), 0xffff0000);
  near(pixel(image, 35, 25), 0xff00ff00);
  near(pixel(image, 5, 5), 0xff0000ff);
  band->group_blending = false;
  image = g::render(root, 100, 100);
  near(pixel(image, 35, 25), 0);  // Explicitly ungrouped blending retains its flattening semantics.
}
void path_gradient_and_trim() {
  auto root = box(0, 0, 100, 100, {0, 0, 0, 1});
  auto path = std::make_shared<g::Path>();
  path->append({g::SegmentType::move, {10, 20}});
  path->append({g::SegmentType::line, {90, 20}});
  auto shape = std::make_shared<g::Layer>(g::Kind::shape);
  shape->path = path;
  shape->fill = {};
  shape->stroke = {1, 1, 1, 1};
  shape->line_width = 4;
  shape->stroke_start = .25;
  shape->stroke_end = .75;
  root->insert(shape, 0);
  auto image = g::render(root, 100, 100);
  CHECK(pixel(image, 20, 20) == 0xff000000 && pixel(image, 40, 20) == 0xffffffff &&
        pixel(image, 80, 20) == 0xff000000);
  shape->stroke_start = -.0001;
  shape->stroke_end = 1.0001;
  image = g::render(root, 100, 100);
  CHECK(pixel(image, 11, 20) == 0xffffffff && pixel(image, 89, 20) == 0xffffffff);
  shape->stroke_start = 1.1;
  image = g::render(root, 100, 100);
  CHECK(pixel(image, 50, 20) == 0xff000000);
  shape->stroke_start = .25;
  shape->stroke_end = .75;
  auto grad = std::make_shared<g::Layer>(g::Kind::gradient);
  grad->set_frame({{0, 40}, {100, 40}});
  grad->gradient_start = {0, 0};
  grad->gradient_end = {1, 0};
  grad->stops = {{{1, 0, 0, 1}, 0}, {{0, 0, 1, 1}, 1}};
  root->insert(grad, 1);
  image = g::render(root, 100, 100);
  near(pixel(image, 49, 50), 0xff81007e, 2);
  auto ellipse = std::make_shared<g::Path>();
  ellipse->append({g::SegmentType::ellipse, {30, 30}, {40, 40}});
  shape->path = ellipse;
  shape->fill = {0, 1, 0, 1};
  shape->stroke = {};
  root->insert(shape, 2);
  image = g::render(root, 100, 100);
  CHECK(pixel(image, 50, 50) == 0xff00ff00 && pixel(image, 31, 31) == 0xff000000);
}
void circular_stroke_trimming() {
  constexpr double pi = 3.14159265358979323846;
  auto root = box(0, 0, 640, 640, {});
  auto shape = std::make_shared<g::Layer>(g::Kind::shape);
  shape->anchor = {0, 0};
  shape->stroke = {1, 1, 1, 1};
  shape->line_width = 94;
  root->insert(shape, 0);
  // Compare a trimmed wide gauge with an independently constructed short
  // circular arc. Matching only the other backend would preserve both bugs.
  for (const bool clockwise : {true, false})
    for (const auto cap : {g::Cap::butt, g::Cap::round, g::Cap::square})
      for (const double finish : {.13, .37, .83}) {
        g::Segment arc{g::SegmentType::arc,
                       {320, 320},
                       {},
                       {},
                       233,
                       clockwise ? .75 * pi : -.75 * pi,
                       clockwise ? 2.25 * pi : -2.25 * pi,
                       clockwise};
        shape->path = std::make_shared<g::Path>();
        shape->path->append(arc);
        shape->cap = cap;
        shape->stroke_start = .07;
        shape->stroke_end = finish;
        const auto actual = g::render(root, 640, 640);
        const auto start = arc.start;
        arc.start = start + (arc.end - start) * .07;
        arc.end = start + (arc.end - start) * finish;
        shape->path = std::make_shared<g::Path>();
        shape->path->append(arc);
        shape->stroke_start = 0;
        shape->stroke_end = 1;
        CHECK(actual.pixels == g::render(root, 640, 640).pixels);
      }
  shape->path = std::make_shared<g::Path>();
  shape->path->append({g::SegmentType::ellipse, {87, 87}, {466, 466}});
  shape->stroke_start = .13;
  shape->stroke_end = .74;
  const auto actual = g::render(root, 640, 640);
  shape->path = std::make_shared<g::Path>();
  shape->path->append(
      {g::SegmentType::arc, {320, 320}, {}, {}, 233, 2 * pi * .13, 2 * pi * .74, true});
  shape->stroke_start = 0;
  shape->stroke_end = 1;
  CHECK(actual.pixels == g::render(root, 640, 640).pixels);
  // Independent annular-sector coverage, concentrated near a nearly vertical
  // progress endpoint. The centreline stroke oracle alone shares Cairo's
  // wide-stroke flare; both radial edges must stay on their actual circles.
  const double endpoint = 4.84;
  shape->cap = g::Cap::butt;
  shape->stroke = {1, 1, 1, 1};
  shape->stroke_start = 0;
  shape->stroke_end = 1;
  shape->path = std::make_shared<g::Path>();
  shape->path->append({g::SegmentType::arc, {320, 320}, {}, {}, 233, .75 * pi, endpoint, true});
  const auto ribbon = g::render(root, 640, 640);
  unsigned max_error = 0;
  for (const double radius : {186., 280.})
    for (const double angle : {endpoint - .005, endpoint - .02, endpoint - .05, endpoint - .1}) {
      const int cx = std::floor(320 + radius * std::cos(angle)),
                cy = std::floor(320 + radius * std::sin(angle));
      for (int y = cy - 1; y <= cy + 1; ++y)
        for (int x = cx - 1; x <= cx + 1; ++x) {
          unsigned inside = 0;
          constexpr unsigned n = 64;
          for (unsigned sy = 0; sy < n; ++sy)
            for (unsigned sx = 0; sx < n; ++sx) {
              const auto dx = x + (sx + .5) / n - 320, dy = y + (sy + .5) / n - 320,
                         rr = std::hypot(dx, dy);
              auto a = std::atan2(dy, dx);
              if (a < .75 * pi) a += 2 * pi;
              inside += rr >= 186 && rr <= 280 && a >= .75 * pi && a <= endpoint;
            }
          const auto expected = std::lround(255. * inside / (n * n));
          max_error = std::max(max_error,
                               unsigned(std::abs(int(pixel(ribbon, x, y) >> 24) - int(expected))));
        }
    }
  CHECK(max_error <= 8);
}
void gauge_endpoint_alignment() {
  constexpr double pi = 3.14159265358979323846;
  auto root = box(0, 0, 640, 640, {});
  auto shape = std::make_shared<g::Layer>(g::Kind::shape);
  shape->anchor = {0, 0};
  shape->stroke = {1, 1, 1, 1};
  root->insert(shape, 0);
  // An ornament and its complete circular track must share both radial edges.
  // Compare against the mathematical circle, not another trimmed path that
  // could have the same approximation/cap bug. Exercise thin ornaments and
  // wide bands at oblique endpoints, in both directions.
  for (const double width : {6., 30., 94.})
    for (const double endpoint : {.23, 1.43, 3.81, 4.84})
      for (const bool clockwise : {true, false})
        for (const auto cap : {g::Cap::butt, g::Cap::round}) {
          const double radius = 233, half = width / 2;
          const double start = endpoint + (clockwise ? -1 : 1) * 1.2;
          shape->line_width = width;
          shape->cap = cap;
          shape->path = std::make_shared<g::Path>();
          shape->path->append(
              {g::SegmentType::arc, {320, 320}, {}, {}, radius, start, endpoint, clockwise});
          const auto image = g::render(root, 640, 640);
          const auto inside = [&](double x, double y) {
            const double r = std::hypot(x, y), a = std::atan2(y, x);
            auto along = std::fmod((clockwise ? a - start : start - a) + 2 * pi, 2 * pi);
            if (along < 0) along += 2 * pi;
            if (r >= radius - half && r <= radius + half && along <= 1.2) return true;
            if (cap == g::Cap::round)
              for (const double end : {start, endpoint})
                if (std::hypot(x - radius * std::cos(end), y - radius * std::sin(end)) <= half)
                  return true;
            return false;
          };
          for (const double r : {radius - half, radius + half})
            for (const double offset : {.005, .025, .08}) {
              const auto a = endpoint + (clockwise ? -1 : 1) * offset;
              const int cx = std::floor(320 + r * std::cos(a)),
                        cy = std::floor(320 + r * std::sin(a));
              for (int y = cy - 1; y <= cy + 1; ++y)
                for (int x = cx - 1; x <= cx + 1; ++x) {
                  unsigned covered = 0;
                  constexpr unsigned n = 64;
                  for (unsigned sy = 0; sy < n; ++sy)
                    for (unsigned sx = 0; sx < n; ++sx)
                      covered += inside(x + (sx + .5) / n - 320, y + (sy + .5) / n - 320);
                  const auto expected = std::lround(255. * covered / (n * n));
                  CHECK(std::abs(int(pixel(image, x, y) >> 24) - int(expected)) <= 8);
                }
            }
        }
}
void masks_and_relative_transforms() {
  auto root = box(0, 0, 100, 100, {0, 0, 0, 1});
  auto group = box(10, 20, 60, 60, {1, 0, 0, 1});
  root->insert(group, 0);
  auto mask = box(5, 6, 20, 30, {1, 1, 1, .5});
  group->set_mask(mask);
  auto frame = g::render(root, 100, 100);
  near(pixel(frame, 20, 30), 0xff800000);
  CHECK(pixel(frame, 40, 30) == 0xff000000);
  group->opacity = .5;
  frame = g::render(root, 100, 100);
  near(pixel(frame, 20, 30), 0xff400000);
  group->group_blending = false;
  frame = g::render(root, 100, 100);
  near(pixel(frame, 20, 30), 0xff400000);
  rejects([&] { mask->set_mask(root); });
  CHECK(root->parent.expired());
  const auto matrix = mask->transform_to(root);
  CHECK(matrix);
  auto p = matrix->apply({0, 0});
  CHECK(p.x == 15 && p.y == 26);
  const auto reverse = root->transform_to(mask);
  CHECK(reverse);
  p = reverse->apply(p);
  CHECK(p.x == 0 && p.y == 0);
  CHECK(!root->transform_to(box(0, 0, 1, 1, {})));
  mask->transform.a = 0;
  CHECK(!root->transform_to(mask));
  mask->transform.a = 1;
  // Alias arguments must survive detaching/reallocating the owning container.
  group->insert(group->mask, 0);
  CHECK(!group->mask && group->children[0] == mask);
  group->set_mask(group->children[0]);
  CHECK(group->children.empty() && group->mask == mask);
  group->set_mask(group->mask);
  CHECK(group->mask == mask);
  group->remove(group->mask);
  CHECK(!group->mask && mask->parent.expired());
  group->remove(nullptr);
  CHECK(group->children.empty());
  root->insert(box(0, 0, 1, 1, {}), 1);
  root->insert(root->children[0], 2);
  CHECK(root->children.back() == group);
  root->remove(root->children[0]);
  CHECK(root->children.size() == 1);
}
void conical_pixels() {
  auto gradient = std::make_shared<g::Layer>(g::Kind::gradient);
  gradient->set_frame({{0, 0}, {101, 101}});
  gradient->gradient_kind = g::GradientKind::conical;
  gradient->gradient_start = {.5, .5};
  gradient->gradient_end = {1, .5};
  gradient->stops = {{{1, 0, 0, 1}, 0},
                     {{0, 1, 0, 1}, .25},
                     {{0, 0, 1, 1}, .5},
                     {{1, 1, 0, 1}, .75},
                     {{1, 0, 0, 1}, 1}};
  auto image = g::render(gradient, 101, 101);
  CHECK(pixel(image, 80, 50) == 0xffff0000 && pixel(image, 50, 80) == 0xffffff00);
  CHECK(pixel(image, 20, 50) == 0xff0000ff && pixel(image, 50, 20) == 0xff00ff00);
  near(pixel(image, 80, 20), 0xff808000);
  gradient->gradient_end = {.5, 1};
  image = g::render(gradient, 101, 101);
  CHECK(pixel(image, 50, 80) == 0xffff0000);
  gradient->gradient_end = {1, .5};
  gradient->stops = {{{1, 0, 0, 1}, 0}, {{0, 0, 1, 0}, 1}};
  gradient->opacity = .5;
  image = g::render(gradient, 101, 101);
  near(pixel(image, 20, 50), 0x40400000);
  gradient->opacity = 1;
  gradient->stops = {
      {{1, 0, 0, 1}, .1}, {{0, 1, 0, 1}, .25}, {{0, 0, 1, 1}, .25}, {{1, 1, 1, 1}, .9}};
  image = g::render(gradient, 101, 101);
  CHECK(pixel(image, 50, 20) == 0xff0000ff && pixel(image, 80, 50) == 0xffff0000);
  auto root = box(0, 0, 130, 130, {0, 0, 0, 1});
  root->insert(gradient, 0);
  gradient->set_frame({{10, 20}, {101, 101}});
  gradient->set_mask(box(0, 0, 51, 101, {1, 1, 1, 1}));
  image = g::render(root, 130, 130);
  CHECK(pixel(image, 60, 40) == 0xff0000ff && pixel(image, 90, 70) == 0xff000000);
  gradient->gradient_end = gradient->gradient_start;
  rejects([&] { g::render(root, 130, 130); });
}
void alpha_masks() {
  g::Image image{7, 6, std::vector<std::uint32_t>(42)};
  image.pixels[2 * 7 + 1] = 0x11010203;
  image.pixels[4 * 7 + 5] = 0xff000000;
  g::AlphaMask mask(image, -3, 4);
  CHECK(mask.x() == -2 && mask.y() == 6 && mask.width() == 5 && mask.height() == 3);
  CHECK(mask.pixels().size() == 15 && mask.pixels()[0] == 17 && mask.pixels()[14] == 255);
  CHECK(mask.pixels()[1] == 0 && !mask.empty());
  image.pixels[2 * 7 + 1] = 0;
  CHECK(mask.pixels()[0] == 17);  //The source cannot mutate a cached mask.
  g::AlphaMask replacement(image, -3, 4);
  CHECK(replacement.x() == 2 && replacement.y() == 8 && replacement.width() == 1);
  image.pixels.assign(42, 0x00ffffff);
  g::AlphaMask transparent(image);
  CHECK(transparent.empty() && transparent.width() == 0);
  rejects([&] { g::AlphaMask bad(g::Image{0, 0, {}}); });
  rejects([&] { g::AlphaMask bad(g::Image{4097, 1, {}}); });
  rejects([&] { g::AlphaMask bad(g::Image{2, 2, {0}}); });
  rejects([&] { g::AlphaMask bad(image, 10000001, 0); });
}
std::uint32_t png_crc(const std::vector<std::uint8_t>& bytes, std::size_t begin) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = begin; i < bytes.size(); ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}
void png_append_number(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}
void png_append_chunk(std::vector<std::uint8_t>& out, const char* type,
                      const std::vector<std::uint8_t>& data) {
  png_append_number(out, std::uint32_t(data.size()));
  const auto begin = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  png_append_number(out, png_crc(out, begin));
}
// Minimal uncompressed RGBA PNG with an optional tEXt chunk before IDAT.
std::vector<std::uint8_t> rgba_png(const std::vector<std::array<std::uint8_t, 4>>& row,
                                   const std::string& text = {}) {
  std::vector<std::uint8_t> out{137, 80, 78, 71, 13, 10, 26, 10};
  std::vector<std::uint8_t> header;
  png_append_number(header, std::uint32_t(row.size()));
  png_append_number(header, 1);
  header.insert(header.end(), {8, 6, 0, 0, 0});
  png_append_chunk(out, "IHDR", header);
  if (!text.empty()) png_append_chunk(out, "tEXt", {text.begin(), text.end()});
  std::vector<std::uint8_t> raw{0};
  for (const auto& p : row)
    raw.insert(raw.end(), p.begin(), p.end());
  std::uint32_t a = 1, b = 0;
  for (auto byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  std::vector<std::uint8_t> zlib{0x78,
                                 0x01,
                                 1,
                                 std::uint8_t(raw.size()),
                                 std::uint8_t(raw.size() >> 8),
                                 std::uint8_t(~raw.size()),
                                 std::uint8_t(~raw.size() >> 8)};
  zlib.insert(zlib.end(), raw.begin(), raw.end());
  png_append_number(zlib, (b << 16) | a);
  png_append_chunk(out, "IDAT", zlib);
  png_append_chunk(out, "IEND", {});
  return out;
}
void png_alpha_declaration() {
  using namespace std::string_literals;
  const std::vector<std::array<std::uint8_t, 4>> straight{{255, 255, 255, 128}, {255, 0, 0, 255}};
  // Ordinary PNG data is straight alpha and is premultiplied while decoding.
  auto plain = g::decode_png(rgba_png(straight));
  CHECK(plain->width == 2 && plain->height == 1);
  CHECK(plain->pixels[0] == 0x80808080u);
  CHECK(plain->pixels[1] == 0xffff0000u);
  // Theme-pack images are already premultiplied and must not be multiplied again:
  // a white 50% edge stays 0x80808080 rather than darkening to 0x80404040.
  const std::vector<std::array<std::uint8_t, 4>> stored{{128, 128, 128, 128}, {0, 60, 0, 60}};
  const auto declared = "hasPremultipliedAlpha\0yes"s;
  auto pack = g::decode_png(rgba_png(stored, declared));
  CHECK(pack->pixels[0] == 0x80808080u);
  CHECK(pack->pixels[1] == 0x3c003c00u);
  auto undeclared = g::decode_png(rgba_png(stored, "hasPremultipliedAlpha\0no"s));
  CHECK(undeclared->pixels[0] == 0x80404040u);
  auto other = g::decode_png(rgba_png(stored, "Comment\0hasPremultipliedAlpha"s));
  CHECK(other->pixels[0] == 0x80404040u);
  // A colour above its own coverage is impossible when premultiplied; clamp it.
  auto invalid = g::decode_png(rgba_png({{200, 10, 90, 100}}, declared));
  CHECK(invalid->pixels[0] == 0x64640a5au);
  // A chunk length running past the input is rejected rather than read.
  auto truncated = rgba_png(stored, declared);
  truncated[33] = 0x7f;
  rejects([&] { (void)g::decode_png(truncated); });
  auto corrupt = rgba_png(straight);
  corrupt[corrupt.size() - 20] ^= 0xff;
  rejects([&] { (void)g::decode_png(corrupt); });
}
void script_drawing(const std::filesystem::path& directory) {
  const g::Image input{2, 2, {0xffff0000, 0x80008000, 0xff0000ff, 0xffffffff}};
  g::write_png(input, (directory / "image.png").string());
  headunit::LuaHost runtime(read_fixture(directory), 64 * 1024 * 1024,
                            {{"main", 100, 100, 1.0 / 60}});
  runtime.bind_custom_image_provider(
      [directory](std::string_view name, int appearance) -> std::optional<Bytes> {
        if (name != "symbol" || appearance != 2) return {};
        std::ifstream file(directory / "image.png", std::ios::binary);
        CHECK(bool(file));
        return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
      });
  runtime.execute(R"(
    local root=CP.DPLayer.create(); root.frame=CP.DPRect(0,0,100,100)
    root.backgroundColor=CP.DPColor(0,0,0,1)
    assert(root.bounds.size.width==100 and root.position.x==50)
    local shape=CP.DPShapeLayer.create(); local path=CP.DPPath.create()
    path:moveTo(CP.DPPoint(10,10)); path:quadTo(CP.DPPoint(40,10),CP.DPPoint(25,0))
    path:lineTo(CP.DPPoint(40,40)); path:lineTo(CP.DPPoint(10,40)); path:closeSubpath()
    assert(path.numPoints==5); shape.path=path; shape.fillColor=CP.DPColor(1,0,0,1)
    root:addSublayer(shape); assert(root:getSublayers()[1]==shape)
    assert(not pcall(function() shape:addSublayer(root) end))
    local asset=CP.createAssetImage('image.png'); local w,h=asset:getSize(); assert(w==2 and h==2)
    assert(CP.createCustomPNG('missing',2)==nil)
    local custom=CP.createCustomPNG('symbol',2); local cw,ch=custom:getSize(); assert(cw==2 and ch==2)
    assert(not pcall(CP.createCustomPNG,'symbol',256))
    local owner,size=asset:getData(); assert(owner==asset and size==16)
    local image=CP.DPImageLayer.create(); image.frame=CP.DPRect(50,50,20,20)
    image.texture=CP.DPTexture.createFromImage(asset); root:addSublayer(image)
    local t=CP.DPAffineTransform.translate(10,20):multiply(CP.DPAffineTransform.scale(2,3))
    assert(t.tx==20 and t.ty==60)
    assert(not pcall(function() root.opacity=0/0 end))
    local ok,err=pcall(CP.DPPoint,'bad',0)
    assert(not ok and string.find(err,'number expected'))
    assert(not pcall(CP.setRootLayer,'different',root))
    CP.setRootLayer('main',root)
  )");
  auto frame = runtime.render_reference();
  CHECK(pixel(frame, 20, 20) == 0xffff0000);
  CHECK(pixel(frame, 80, 80) == 0xff000000);
  CHECK(((pixel(frame, 54, 54) >> 16) & 255) > 240);
  rejects([&] { (void)g::decode_png(std::vector<std::uint8_t>(33)); });
  g::write_png(frame, (directory / "render.png").string());
}
void rendering_faults(const std::filesystem::path& directory) {
  headunit::LuaHost runtime(read_fixture(directory), 64 * 1024 * 1024,
                            {{"main", 100, 100, 1.0 / 60}});
  runtime.connected(true);
  runtime.execute(R"(
    local root=CP.DPGradientLayer.create(); root.frame=CP.DPRect(0,0,100,100)
    root.blendMode=CP.DPLayer.BLEND.COMPONENTMASK
    root:setColors({CP.DPColor(1,0,0,1),CP.DPColor(0,0,1,1)})
    CP.setRootLayer('main',root)
    CP.sendMessage(CP.createPlist({test=true}))
  )");
  rejects([&] { (void)runtime.render_reference(); });
  CHECK(runtime.faulted() && runtime.take_outbound().empty());
  rejects([&] { runtime.execute("return 1"); });
}
void video_layers(const std::filesystem::path& directory) {
  headunit::LuaHost runtime(read_fixture(directory), 64 * 1024 * 1024, {{"main", 8, 8, 1.0 / 60}});
  auto input = runtime.add_video_input({"phone", 2, 2});
  rejects([&] { runtime.add_video_input({"phone", 2, 2}); });
  runtime.execute(R"(
    local inputs=CP.enumerateInputStreams()
    assert(#inputs==1 and inputs[1].identifier=='phone' and inputs[1].width==2)
    assert(inputs[1].streamMode==0 and inputs[1].parameterMode==1 and inputs[1].type==3)
    assert(not pcall(CP.DPVideoLayer.create,'unnegotiated'))
    movie=CP.DPVideoLayer.create('phone');movie.frame=CP.DPRect(0,0,8,8)
    movie.backgroundColor=CP.DPColor(0,0,0,1)
    assert(movie:getClassName()=='DPVideoLayer')
    assert(not movie.active and movie.streamState==CP.DPVideoLayer.STREAMSTATE.UNSUBSCRIBED)
    movie:subscribe();assert(movie.active and movie.streamState==CP.DPVideoLayer.STREAMSTATE.SUBSCRIBED)
    assert(movie:getCurrentPTS()==nil and movie:getNextPTS()==nil and CP.PTS==nil)
    assert(not pcall(function() movie.streamState=123 end))
    assert(not pcall(function() CP.DPLayer.create():subscribe() end))
    CP.setRootLayer('main',movie)
  )");
  // A local camera is independent of the phone stream. The installed ABI
  // reports its authored camera type, with the fixed buffered host capabilities.
  auto camera = runtime.add_video_input(
      {"camera", 2, 2, 1.0 / 30, headunit::theme::video::InputType::camera});
  CHECK(camera->input().type == headunit::theme::video::InputType::camera);
  rejects([&] {
    runtime.add_video_input(
        {"invalid-type", 2, 2, 1.0 / 30, static_cast<headunit::theme::video::InputType>(5)});
  });
  runtime.execute(R"(
    local inputs=CP.enumerateInputStreams()
    assert(#inputs==2)
    local types={}
    for _,input in ipairs(inputs) do
      types[input.identifier]=input.type
      assert(input.streamMode==0 and input.parameterMode==1)
      assert(input.opacityCapability==0 and input.positionCapability==0 and input.sizeCapability==0)
      if input.identifier=='camera' then assert(input.frameDuration==1/30) end
    end
    assert(types.camera==0 and types.phone==3)
  )");
  CHECK(input->offer(42.5, g::Image{2, 2, std::vector<std::uint32_t>(4, 0xffff0000)}));
  runtime.execute(
      "assert(movie:getNextPTS()==42.5 and "
      "movie:getCurrentPTS()==nil);assert(movie.streamState==CP.DPVideoLayer.STREAMSTATE."
      "DATAAVAILABLE)",
      "video-state-1");
  CHECK(pixel(runtime.render_reference(), 4, 4) == 0xff000000);  // Queued is not composed.
  auto frame = input->prepare();
  CHECK(frame && frame->pts == 42.5 && !input->current_pts());
  CHECK(pixel(runtime.render_reference(), 4, 4) == 0xffff0000);
  runtime.execute("assert(movie:getCurrentPTS()==nil and CP.PTS==nil)",
                  "video-state-2");  // Render is not presentation.
  input->presented(frame->token);
  runtime.execute("assert(movie:getCurrentPTS()==42.5 and movie:getNextPTS()==nil)",
                  "video-state-3");
  rejects([&] { input->presented(frame->token); });
  for (int i = 0; i < 3; ++i)
    CHECK(input->offer(43 + i, g::Image{2, 2, std::vector<std::uint32_t>(4, 0xff0000ff)}));
  g::Image retained{2, 2, std::vector<std::uint32_t>(4, 0xff00ff00)};
  CHECK(!input->offer(46, std::move(retained)) && retained.pixels.size() == 4);
  auto next = input->prepare();
  CHECK(next && next->pts == 43);
  CHECK(pixel(runtime.render_reference(), 4, 4) == 0xff0000ff);
  input->cancel();
  CHECK(pixel(runtime.render_reference(), 4, 4) == 0xffff0000);
  rejects([&] { input->presented(next->token); });
  runtime.execute("movie:unsubscribe();assert(not movie.active and movie:getCurrentPTS()==nil)",
                  "video-state-4");
  CHECK(pixel(runtime.render_reference(), 4, 4) == 0xff000000);
  runtime.execute(
      "movie:subscribe();assert(movie:getCurrentPTS()==42.5 and movie:getNextPTS()==43)",
      "video-state-5");
  runtime.execute(
      "movie:flush();assert(movie.active and movie:getCurrentPTS()==nil and "
      "movie:getNextPTS()==nil and movie.streamState==CP.DPVideoLayer.STREAMSTATE.SUBSCRIBED)",
      "video-flush");
  CHECK(!input->prepared(next->token));
  rejects([&] { input->presented(next->token); });
  CHECK(pixel(runtime.render_reference(), 4, 4) == 0xff000000);
  rejects([&] { input->offer(45, g::Image{2, 2, std::vector<std::uint32_t>(4)}); });
  CHECK(input->offer(46, g::Image{2, 2, std::vector<std::uint32_t>(4, 0xff00ff00)}));
  const auto flushed = input->prepare();
  CHECK(flushed && flushed->token > next->token && input->prepared(flushed->token));
  runtime.execute("movie:flush();assert(movie:getNextPTS()==nil)", "video-flush-staged");
  CHECK(!input->prepared(flushed->token));
  rejects([&] { input->presented(flushed->token); });
  CHECK(input->offer(47, g::Image{2, 2, std::vector<std::uint32_t>(4, 0xff00ff00)}));
  const auto resumed = input->prepare();
  CHECK(resumed && resumed->token > flushed->token);
  input->presented(resumed->token);
  runtime.execute("assert(movie:getCurrentPTS()==47 and movie:getNextPTS()==nil)",
                  "video-flush-resumed");
  input->close();
  runtime.execute(
      "assert(not movie.active and movie.streamState==CP.DPVideoLayer.STREAMSTATE.CLOSED and "
      "movie:getNextPTS()==nil)",
      "video-state-6");
  runtime.execute("assert(not pcall(function() movie:flush() end))", "video-flush-closed");
  rejects([&] { (void)input->offer(46, std::move(retained)); });
  CHECK(pixel(runtime.render_reference(), 4, 4) == 0xff000000);
}
void video_ownership() {
  auto memory = std::make_shared<video::Memory>();
  video::Stream stream({"phone", 2, 2}, memory);
  CHECK(stream.offer(1, g::Image{2, 2, std::vector<std::uint32_t>(4)}));
  CHECK(memory->bytes == 16);
  {
    const auto frame = stream.prepare().value();
    stream.presented(frame.token);
    rejects([&] { stream.offer(1, g::Image{2, 2, std::vector<std::uint32_t>(4)}); });
    rejects([&] { stream.offer(NAN, g::Image{2, 2, std::vector<std::uint32_t>(4)}); });
    rejects([&] { stream.offer(2, g::Image{3, 2, std::vector<std::uint32_t>(6)}); });
    CHECK(memory->bytes == 16);
    stream.close();
    CHECK(memory->bytes == 16);  // Snapshot still owns its pixels.
  }
  CHECK(memory->bytes == 0);
}
void native_video_ownership(const std::filesystem::path& directory) {
  auto memory = std::make_shared<video::Memory>();
  video::Stream stream({"native", 2, 2}, memory);
  auto owner = std::make_shared<int>(7);
  std::weak_ptr<int> weak = owner;
  auto native = std::make_shared<video::NativeFrame>();
  native->metadata.width = 2;
  native->metadata.height = 2;
  native->lifetime = owner;
  CHECK(stream.offer_native(1, native));
  CHECK(memory->bytes == 0 && memory->native_frames == 1);
  auto prepared = stream.prepare();
  CHECK(prepared && !prepared->image && prepared->native.get() == native.get());
  rejects([&] { stream.image(); });
  CHECK(!stream.current_pts());
  stream.cancel();
  CHECK(!stream.native());
  rejects([&] { stream.presented(prepared->token); });
  CHECK(stream.prepare()->token == prepared->token);
  stream.presented(prepared->token);
  CHECK(stream.current_pts() == 1 && stream.native().get() == native.get());
  rejects([&] { stream.offer_native(1, native); });
  rejects([&] { stream.offer_native(NAN, native); });
  rejects([&] { stream.offer_native(2, {}); });
  rejects([&] { stream.offer(2, g::Image{2, 2, std::vector<std::uint32_t>(4)}); });
  for (int i = 2; i < 5; ++i)
    CHECK(stream.offer_native(i, native));
  CHECK(!stream.offer_native(5, native) && memory->native_frames == 4);
  stream.close();
  CHECK(memory->native_frames == 1);
  native.reset();
  owner.reset();
  CHECK(!weak.expired());
  prepared.reset();
  CHECK(weak.expired() && memory->native_frames == 0);

  headunit::LuaHost runtime(read_fixture(directory), 64 * 1024 * 1024, {{"main", 8, 8, 1.0 / 60}});
  auto input = runtime.add_video_input({"native", 2, 2});
  runtime.execute(R"(
    movie=CP.DPVideoLayer.create('native');movie.frame=CP.DPRect(0,0,8,8);movie:subscribe()
    CP.setRootLayer('main',movie)
  )");
  native = std::make_shared<video::NativeFrame>();
  native->metadata.width = 2;
  native->metadata.height = 2;
  native->lifetime = std::make_shared<int>(1);
  CHECK(input->offer_native(2, native));
  prepared = input->prepare();
  CHECK(runtime.scene_root()->video->native().get() == native.get());
  runtime.execute("assert(movie:getCurrentPTS()==nil and movie:getNextPTS()==2 and CP.PTS==nil)");
  input->presented(prepared->token);
  runtime.presented(2);
  runtime.tick(2);
  runtime.execute("assert(movie:getCurrentPTS()==2 and movie:getNextPTS()==nil and CP.PTS==2)");
  runtime.execute("movie:unsubscribe()");
  CHECK(!runtime.scene_root()->video->native());
  runtime.execute("movie:subscribe()");
  input->close();
  CHECK(!runtime.scene_root()->video->native());

  // Bound native samples across all inputs, including snapshots held after close.
  native = std::make_shared<video::NativeFrame>();
  native->metadata.width = 2;
  native->metadata.height = 2;
  native->lifetime = std::make_shared<int>(1);
  std::vector<video::Frame> held;
  for (std::size_t i = 0; i < video::Memory::native_limit; ++i) {
    video::Stream next({"bounded", 2, 2}, memory);
    CHECK(next.offer_native(1, native));
    held.push_back(*next.prepare());
  }
  video::Stream bounded({"extra", 2, 2}, memory);
  CHECK(!bounded.offer_native(1, native));
  held.clear();
  CHECK(memory->native_frames == 0);
  CHECK(bounded.offer_native(1, native));
}
void video_queue_deadlines() {
  auto memory = std::make_shared<video::Memory>();
  video::Stream stream({"queue", 2, 2}, memory);
  auto native = std::make_shared<video::NativeFrame>();
  native->metadata.width = 2;
  native->metadata.height = 2;
  native->lifetime = std::make_shared<int>(1);
  for (int i = 1; i <= 3; ++i)
    CHECK(stream.offer_native(i, native));
  CHECK(stream.discard_obsolete(1.5) == 0 && memory->native_frames == 3);
  CHECK(stream.discard_obsolete(2.5) == 1 && memory->native_frames == 2 && stream.next_pts() == 2 &&
        !stream.current_pts());
  const auto submitted = stream.prepare();
  CHECK(submitted && submitted->pts == 2);
  CHECK(stream.offer_native(4, native));
  CHECK(stream.discard_obsolete(4) == 1 && stream.next_pts() == 2 && !stream.current_pts());
  CHECK(stream.prepare()->token == submitted->token);  // In-flight source stays immutable.
  stream.presented(submitted->token);
  CHECK(stream.current_pts() == 2 && stream.next_pts() == 4);
  CHECK(stream.offer_native(5, native));
  CHECK(stream.discard_obsolete(4.5) == 0);
  CHECK(stream.prepare()->pts == 4);
  CHECK(stream.discard_obsolete(99) == 0);
  stream.cancel();
  CHECK(stream.discard_obsolete(99) == 1 && stream.current_pts() == 2 && stream.next_pts() == 5);
  const auto last = stream.prepare();
  CHECK(last && last->token > submitted->token && last->pts == 5);
  rejects([&] { stream.presented(submitted->token); });
  stream.presented(last->token);
  CHECK(stream.current_pts() == 5 && !stream.next_pts());
  CHECK(stream.offer_native(6, native));
  const auto old_token = stream.prepare()->token;
  stream.discard_all();
  CHECK(!stream.closed() && !stream.current_pts() && !stream.next_pts() && !stream.native());
  CHECK(memory->native_frames == 2);  // Only the explicit snapshots above still own leases.
  rejects([&] { stream.presented(old_token); });
  rejects([&] { stream.offer_native(6, native); });
  rejects([&] { stream.offer(7, g::Image{2, 2, std::vector<std::uint32_t>(4)}); });
  CHECK(stream.offer_native(7, native));
  const auto resumed = stream.prepare();
  CHECK(resumed->token > old_token);
  stream.presented(resumed->token);
  CHECK(stream.current_pts() == 7);
  rejects([&] { stream.discard_obsolete(NAN); });
  rejects([&] { stream.discard_obsolete(-1); });
  stream.close();
  rejects([&] { stream.discard_obsolete(5); });
}
void video_multiple_submissions() {
  auto memory = std::make_shared<video::Memory>();
  video::Stream stream({"overlap", 2, 2}, memory);
  const auto offer = [&](double pts, std::uint32_t color) {
    CHECK(stream.offer(pts, g::Image{2, 2, std::vector<std::uint32_t>(4, color)}));
  };
  offer(1, 0xffff0000);
  offer(2, 0xff00ff00);
  offer(3, 0xff0000ff);
  auto first = stream.prepare_next(), second = stream.prepare_next();
  CHECK(first && second && first->token != second->token && !stream.prepare_next());
  CHECK(stream.prepared(first->token) && stream.prepared(second->token));
  CHECK(!stream.current_pts() && stream.next_unprepared_pts() == 3);
  CHECK(stream.composition_token() == second->token && stream.image()->pixels[0] == 0xff00ff00);
  CHECK(stream.discard_obsolete(99) == 0);  // Both in-flight frames are immutable.
  rejects([&] { stream.presented(second->token); });
  rejects([&] { stream.cancel(second->token); });
  stream.presented(first->token);
  CHECK(stream.current_pts() == 1 && stream.prepared(second->token) &&
        stream.image()->pixels[0] == 0xff00ff00);
  auto third = stream.prepare_next();
  CHECK(third && third->pts == 3 && stream.composition_token() == third->token);
  stream.cancel(second->token);  // A rejected old frame must not reappear behind the newer one.
  CHECK(stream.current_pts() == 1 && !stream.prepared(second->token) &&
        stream.prepared(third->token));
  CHECK(stream.next_pts() == 3 && !stream.next_unprepared_pts() && !stream.prepare_next());
  stream.presented(third->token);
  CHECK(stream.current_pts() == 3 && stream.image()->pixels[0] == 0xff0000ff);
  offer(4, 0xffffffff);
  offer(5, 0xff000000);
  first = stream.prepare_next();
  stream.cancel(first->token);
  CHECK(stream.prepare_next()->token == first->token);  // Single-frame retry remains valid.
  second = stream.prepare_next();
  CHECK(second && second->pts == 5);
  stream.flush();
  CHECK(!stream.composition_token() && !stream.prepared(first->token) &&
        !stream.prepared(second->token));
  offer(6, 0xff123456);
  CHECK(stream.prepare_next()->token > second->token);
  stream.close();
  CHECK(!stream.composition_token() && !stream.can_offer_native());
  first.reset();
  second.reset();
  third.reset();
  CHECK(memory->bytes == 0);

  // A receipt for the older DMA-buf must not release the newer source or allow
  // current PTS to advance before its own receipt. No copies of native pixels.
  video::Stream native_stream({"native-overlap", 2, 2}, memory);
  std::weak_ptr<int> old_life, new_life;
  const auto native_offer = [&](double pts, std::weak_ptr<int>& weak) {
    auto native = std::make_shared<video::NativeFrame>();
    native->metadata.width = 2;
    native->metadata.height = 2;
    auto life = std::make_shared<int>(int(pts));
    weak = life;
    native->lifetime = life;
    CHECK(native_stream.offer_native(pts, native));
  };
  native_offer(1, old_life);
  native_offer(2, new_life);
  const auto a = native_stream.prepare_next()->token, b = native_stream.prepare_next()->token;
  CHECK(memory->native_frames == 2 && !old_life.expired() && !new_life.expired());
  native_stream.cancel(a);
  CHECK(old_life.expired() && !new_life.expired() && memory->native_frames == 1);
  CHECK(native_stream.native() && !native_stream.current_pts());
  native_stream.presented(b);
  CHECK(native_stream.current_pts() == 2 && memory->native_frames == 1);
  native_stream.discard_all();
  CHECK(new_life.expired() && memory->native_frames == 0);
}
g::Image blur_oracle(const g::Image& input, double sigma, bool clamp, bool normalize) {
  // Independent direct 2-D convolution, not the renderer's two rounded passes.
  auto output = input;
  const int extent = int(std::ceil(3 * sigma));
  for (int y = 0; y < int(input.height); ++y)
    for (int x = 0; x < int(input.width); ++x) {
      double sum[4]{}, weight_sum = 0, all_weights = 0;
      for (int dy = -extent; dy <= extent; ++dy)
        for (int dx = -extent; dx <= extent; ++dx) {
          const auto weight = std::exp(-double(dx * dx + dy * dy) / (2 * sigma * sigma));
          all_weights += weight;
          int sx = x + dx, sy = y + dy;
          if (clamp) {
            sx = std::clamp(sx, 0, int(input.width) - 1);
            sy = std::clamp(sy, 0, int(input.height) - 1);
          }
          if (sx < 0 || sy < 0 || sx >= int(input.width) || sy >= int(input.height)) continue;
          weight_sum += weight;
          const auto px = pixel(input, sx, sy);
          for (unsigned c = 0; c < 4; ++c)
            sum[c] += ((px >> (8 * c)) & 255) * weight;
        }
      std::uint32_t value = 0;
      for (unsigned c = 0; c < 4; ++c)
        value |= std::uint32_t(std::lround(sum[c] / (normalize ? weight_sum : all_weights)))
                 << (8 * c);
      output.pixels[y * input.width + x] = value;
    }
  return output;
}
void backdrop_pixels() {
  auto root = box(0, 0, 32, 24, {0, 0, 1, 1});
  root->insert(box(0, 0, 16, 24, {1, 0, 0, 1}), 0);
  const auto background = g::render(root, 32, 24);
  auto panel = std::make_shared<g::Layer>(g::Kind::backdrop);
  panel->set_frame({{8, 4}, {16, 16}});
  auto blur = std::make_shared<g::BlurFilter>();
  blur->radius = 2;
  blur->normalized_edges = true;
  panel->add_filter(blur);
  root->insert(panel, 1);
  auto expected = blur_oracle(background, 2, false, true), actual = g::render(root, 32, 24);
  g::RenderCosts costs;
  CHECK(g::render(root, 32, 24, &costs).pixels == actual.pixels);
  CHECK(costs.visited_layers > 0 && costs.blur_taps > 0 && costs.backdrop_ns > 0);
  g::render({}, 1, 1, &costs);
  CHECK(costs.visited_layers == 0 && costs.blur_taps == 0 && costs.backdrop_ns == 0);
  for (int y = 0; y < 24; ++y)
    for (int x = 0; x < 32; ++x)
      near(pixel(actual, x, y),
           pixel(x >= 8 && x < 24 && y >= 4 && y < 20 ? expected : background, x, y), 1);
  root->insert(box(14, 9, 4, 4, {0, 1, 0, 1}), 2);
  actual = g::render(root, 32, 24);
  CHECK(pixel(actual, 15, 10) == 0xff00ff00);
  near(pixel(actual, 15, 7), pixel(expected, 15, 7));
  root->remove(root->children.back());
  // Sample through a cropped, translated parent group, then apply its mask and
  // opacity exactly once. Prior content in that group participates in the blur.
  auto parent = box(4, 3, 24, 18, {});
  parent->clip = true;
  root->insert(parent, 1);
  auto yellow = box(8, 5, 2, 4, {1, 1, 0, 1});
  parent->insert(yellow, 0);
  panel->detach();
  panel->set_frame({{4, 1}, {16, 16}});
  parent->insert(panel, 1);
  blur->enabled = false;
  auto backdrop = g::render(root, 32, 24);
  blur->enabled = true;
  auto filtered = blur_oracle(backdrop, 2, false, true);
  parent->opacity = .5;
  parent->set_mask(box(0, 0, 12, 18, {1, 1, 1, .5}));
  actual = g::render(root, 32, 24);
  for (int y = 4; y < 20; ++y)
    for (int x = 8; x < 16; ++x) {
      std::uint32_t px = 0;
      for (unsigned c = 0; c < 4; ++c)
        px |= std::uint32_t(std::lround(.75 * ((pixel(background, x, y) >> (8 * c)) & 255) +
                                        .25 * ((pixel(filtered, x, y) >> (8 * c)) & 255)))
              << (8 * c);
      near(pixel(actual, x, y), px, 2);
    }
  CHECK(pixel(actual, 20, 10) == pixel(background, 20, 10));
  // Surface-edge policy and premultiplied alpha, including transparent input.
  root = box(0, 0, 12, 10, {1, 0, 0, .5});
  auto edge = box(0, 0, 2, 10, {0, 0, 1, .5});
  root->insert(edge, 0);
  panel->detach();
  panel->set_frame({{0, 0}, {12, 10}});
  root->insert(panel, 1);
  blur->enabled = false;
  const auto translucent = g::render(root, 12, 10);
  blur->enabled = true;
  for (const auto policy : {0, 1, 2}) {
    blur->normalized_edges = policy == 1;
    blur->hard_edges = policy == 2;
    expected = blur_oracle(translucent, 2, blur->hard_edges, blur->normalized_edges);
    actual = g::render(root, 12, 10);
    for (std::size_t i = 0; i < actual.pixels.size(); ++i)
      near(actual.pixels[i], expected.pixels[i], 1);
  }
  panel->opacity = .5;
  rejects([&] { g::render(root, 12, 10); });
  panel->opacity = 1;
  auto replacement = std::make_shared<g::Layer>(g::Kind::backdrop);
  replacement->add_filter(blur);
  CHECK(panel->filters.empty() && replacement->filters.size() == 1);
  replacement->add_filter(blur);
  CHECK(replacement->filters.size() == 1);
  replacement->remove_filter(replacement->filters[0]);
  CHECK(blur->parent.expired());
  rejects([&] { root->add_filter(blur); });
  blur->radius = 129;
  panel->add_filter(blur);
  rejects([&] { g::render(root, 12, 10); });
  blur->radius = 2;
  panel->transform = {1, .5, 0, 1, 0, 0};
  rejects([&] { g::render(root, 12, 10); });
  auto bounded = std::make_shared<g::Layer>(g::Kind::backdrop);
  bounded->set_frame({{0, 0}, {512, 512}});
  for (int i = 0; i < 8; ++i) {
    auto f = std::make_shared<g::BlurFilter>();
    f->radius = 128;
    bounded->add_filter(f);
  }
  rejects([&] { bounded->add_filter(std::make_shared<g::BlurFilter>()); });
  bool work_limited = false;
  try {
    g::render(bounded, 512, 512);
  } catch (const std::exception& e) {
    work_limited = std::string(e.what()).find("work budget") != std::string::npos;
  }
  CHECK(work_limited);
}
void script_filters(const std::filesystem::path& directory) {
  headunit::LuaHost runtime(read_fixture(directory), 64 * 1024 * 1024,
                            {{"main", 32, 24, 1.0 / 60}});
  runtime.execute(R"(
    local root=CP.DPLayer.create();root.frame=CP.DPRect(0,0,32,24);root.backgroundColor=CP.DPColor(0,0,1,1)
    local red=CP.DPLayer.create();red.frame=CP.DPRect(0,0,16,24);red.backgroundColor=CP.DPColor(1,0,0,1);root:addSublayer(red)
    local panel=CP.DPBackdropLayer.create();panel.frame=CP.DPRect(8,4,16,16)
    assert(panel:getClassName()=='DPBackdropLayer')
    blur=CP.DPBlurFilter.create();blur.radius=2;blur.normalizedEdges=true
    assert(blur:getClassName()=='DPBlurFilter' and blur.radius==2 and blur.enabled and not blur.hardEdges)
    assert(not pcall(function() blur.radius=-1 end));assert(not pcall(function() blur.radius=0/0 end))
    assert(not pcall(function() blur.enabled=1 end));assert(not pcall(function() blur.unrecognized=3 end))
    assert(not pcall(function() root:addFilter(blur) end))
    panel:addFilter(blur);panel:addFilter(blur);root:addSublayer(panel);CP.setRootLayer('main',root)
  )");
  auto actual = runtime.render_reference();
  CHECK(pixel(actual, 15, 10) != 0xffff0000 && pixel(actual, 16, 10) != 0xff0000ff);
  runtime.execute("blur.enabled=false");
  CHECK(pixel(runtime.render_reference(), 15, 10) == 0xffff0000);
  runtime.execute("blur.enabled=true;blur:removeFromLayer();blur:removeFromLayer()");
  CHECK(pixel(runtime.render_reference(), 15, 10) == 0xffff0000);
}
int main() {
  char name[] = "/tmp/headunit-graphics-test-XXXXXX";
  const auto* path = ::mkdtemp(name);
  if (!path) return 1;
  try {
    transforms_hierarchy_and_alpha();
    continuous_corner_pixels();
    opaque_group_destination_out();
    path_gradient_and_trim();
    circular_stroke_trimming();
    gauge_endpoint_alignment();
    masks_and_relative_transforms();
    script_drawing(path);
    rendering_faults(path);
    video_layers(path);
    video_ownership();
    native_video_ownership(path);
    video_queue_deadlines();
    video_multiple_submissions();
    backdrop_pixels();
    script_filters(path);
    conical_pixels();
    alpha_masks();
    png_alpha_declaration();
    std::filesystem::remove_all(path);
    std::cout << "graphics pixels, hierarchy, transforms, alpha, paths, gradients and script "
                 "drawing passed\n";
  } catch (const std::exception& e) {
    std::filesystem::remove_all(path);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
