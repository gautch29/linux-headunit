// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/lua_host.hpp>

#include <cairo.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace graphics = headunit::theme::graphics;
constexpr int width = 1280, height = 480;
constexpr std::uint32_t background = 0x121b23, panel = 0x1a2933, white = 0xeff4f2, muted = 0xa8bcc3,
                        accent = 0xf6c46e;

struct Canvas {
  Canvas(int w, int h)
      : surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h), cairo_surface_destroy),
        context(cairo_create(surface.get()), cairo_destroy) {
    if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS)
      throw std::runtime_error("Cannot allocate preview canvas");
  }

  void color(std::uint32_t rgb) {
    cairo_set_source_rgb(context.get(), ((rgb >> 16) & 255) / 255., ((rgb >> 8) & 255) / 255.,
                         (rgb & 255) / 255.);
  }

  void rect(double x, double y, double w, double h, std::uint32_t rgb) {
    color(rgb);
    cairo_rectangle(context.get(), x, y, w, h);
    cairo_fill(context.get());
  }

  void text(double x, double y, const char* label, double size, std::uint32_t rgb) {
    color(rgb);
    cairo_select_font_face(context.get(), "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           size >= 26 ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context.get(), size);
    cairo_move_to(context.get(), x, y);
    cairo_show_text(context.get(), label);
  }

  void path(std::initializer_list<graphics::Point> points, std::uint32_t rgb, double line_width) {
    auto* cr = context.get();
    color(rgb);
    cairo_move_to(cr, points.begin()->x, points.begin()->y);
    for (auto point = points.begin() + 1; point != points.end(); ++point) {
      cairo_line_to(cr, point->x, point->y);
    }
    if (line_width > 0) {
      cairo_set_line_width(cr, line_width);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_stroke(cr);
    } else {
      cairo_close_path(cr);
      cairo_fill(cr);
    }
  }

  std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface;
  std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context;
};

graphics::Image route_frame(int w, int h) {
  Canvas map(w, h);
  cairo_scale(map.context.get(), w / 800., h / 340.);
  map.rect(0, 0, 800, 340, 0x223a40);
  map.path({{592, 0}, {532, 75}, {560, 138}, {679, 185}, {618, 340}, {800, 340}, {800, 0}},
           0x163040, 0);
  map.path({{0, 52}, {128, 58}, {189, 155}, {75, 208}, {0, 174}}, 0x30483d, 0);
  map.path({{291, 0}, {334, 99}, {490, 74}, {548, 0}}, 0x30483d, 0);
  map.path({{145, 340}, {216, 261}, {417, 271}, {480, 340}}, 0x30483d, 0);
  map.path({{0, 91}, {139, 101}, {247, 177}, {380, 164}, {541, 103}}, 0x607476, 8);
  map.path({{202, 0}, {236, 98}, {289, 131}, {374, 230}, {430, 340}}, 0x607476, 8);
  map.path({{0, 274}, {132, 245}, {250, 247}, {391, 217}, {550, 237}, {642, 273}}, 0x607476, 8);
  map.path({{93, 340}, {146, 262}, {182, 191}, {223, 87}, {253, 0}}, 0x526669, 5);
  map.path({{476, 340}, {507, 254}, {472, 177}, {495, 93}, {512, 0}}, 0x526669, 5);
  map.path({{290, 340}, {314, 270}, {403, 242}, {441, 139}, {415, 0}}, 0x526669, 5);
  map.path({{264, 240}, {391, 217}, {469, 227}, {506, 161}, {495, 93}, {548, 42}}, accent, 11);
  map.path({{242, 245}, {284, 220}, {279, 254}}, white, 0);
  map.text(24, 48, "RIVERSIDE", 19, muted);
  map.text(612, 127, "NORTH INLET", 19, muted);
  map.text(358, 202, "Harbour Road", 17, white);
  map.rect(20, 283, 127, 38, 0x17252f);
  map.text(32, 308, "SYNTHETIC MAP", 12, muted);

  cairo_surface_flush(map.surface.get());
  graphics::Image image{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                        std::vector<std::uint32_t>(static_cast<std::size_t>(w) * h)};
  const auto* source = cairo_image_surface_get_data(map.surface.get());
  const auto stride = cairo_image_surface_get_stride(map.surface.get());
  for (int row = 0; row < h; ++row) {
    std::memcpy(image.pixels.data() + static_cast<std::size_t>(row) * w, source + row * stride,
                static_cast<std::size_t>(w) * sizeof(std::uint32_t));
  }
  return image;
}

headunit::LuaHost::Bytes display_artwork(bool cluster) {
  Canvas scene(width, height);
  scene.rect(0, 0, width, height, background);
  scene.rect(44, 30, 5, 25, accent);
  scene.text(64, 51, cluster ? "Ultra / Instrument cluster" : "Ultra / Main display", 27, white);
  scene.text(1040, 49, "SIMULATED INPUTS", 17, muted);
  scene.rect(44, 71, 1192, 1, 0x394952);
  if (cluster) {
    scene.rect(44, 103, 290, 290, panel);
    scene.text(71, 150, "ROAD SPEED", 19, muted);
    scene.text(66, 300, "42", 146, white);
    scene.text(76, 338, "km/h", 26, muted);
    scene.rect(946, 103, 290, 290, panel);
    scene.text(974, 150, "ENGINE SPEED", 19, muted);
    scene.text(970, 226, "2,370", 64, white);
    scene.text(978, 258, "rpm", 23, muted);
    scene.rect(978, 290, 226, 6, 0x43545a);
    scene.rect(978, 290, 98, 6, accent);
    scene.text(978, 352, "D   |   FUEL 62%", 21, muted);
    scene.text(44, 443, "NEXT TURN  /  600 m", 20, accent);
    scene.text(363, 443, "Harbour Road", 24, white);
  } else {
    scene.rect(44, 103, 328, 324, panel);
    scene.text(71, 149, "NEXT MANEUVER", 20, muted);
    scene.path({{74, 221}, {105, 196}, {105, 210}, {149, 210}, {149, 234}, {105, 234}, {105, 247}},
               accent, 0);
    scene.text(71, 305, "600 m", 56, white);
    scene.text(72, 351, "Harbour Road", 29, white);
    scene.text(73, 392, "Turn left", 23, muted);
    scene.text(44, 460, "ETA  18:42", 19, muted);
    scene.text(402, 460, "Riverside route  /  4.8 km remaining", 19, muted);
  }

  headunit::LuaHost::Bytes png;
  const auto write = [](void* output, const unsigned char* bytes, unsigned int count) {
    auto& buffer = *static_cast<headunit::LuaHost::Bytes*>(output);
    try {
      buffer.insert(buffer.end(), bytes, bytes + count);
      return CAIRO_STATUS_SUCCESS;
    } catch (...) {
      return CAIRO_STATUS_WRITE_ERROR;
    }
  };
  if (cairo_surface_write_to_png_stream(scene.surface.get(), write, &png) != CAIRO_STATUS_SUCCESS)
    throw std::runtime_error("Cannot encode the preview artwork");
  return png;
}

void render(const std::filesystem::path& path, bool cluster) {
  const std::string display = cluster ? "preview-cluster" : "preview-main";
  headunit::LuaHost host(
      [artwork = display_artwork(cluster)](const std::string& name) {
        return name == "fascia.png" ? artwork : headunit::LuaHost::Bytes{};
      },
      64 * 1024 * 1024, {{display, width, height, 1. / 60}});
  const int video_width = cluster ? 556 : 834;
  const int video_height = cluster ? 290 : 324;
  auto input = host.add_video_input({"preview-route", static_cast<std::uint32_t>(video_width),
                                     static_cast<std::uint32_t>(video_height)});
  host.execute(R"(
    local display = CP.enumerateOutputStreams()[1].identifier
    local cluster = display == 'preview-cluster'
    local root = CP.DPLayer.create()
    root.frame = CP.DPRect(0, 0, 1280, 480)
    local panel = CP.DPImageLayer.create()
    panel.frame = root.frame
    panel.texture = CP.DPTexture.createFromImage(CP.createAssetImage('fascia.png'))
    root:addSublayer(panel)
    local route = CP.DPVideoLayer.create('preview-route')
    route.frame = cluster and CP.DPRect(363, 103, 556, 290) or CP.DPRect(402, 103, 834, 324)
    route:subscribe()
    root:addSublayer(route)
    CP.setRootLayer(display, root)
  )",
               "ultra-preview");
  if (!input->offer(1., route_frame(video_width, video_height)) || !input->prepare())
    throw std::runtime_error("Cannot stage the synthetic video frame");
  graphics::write_png(host.render_reference(), path.string());
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2)
    throw std::invalid_argument("usage: headunit-ultra-scene-preview OUTPUT_DIRECTORY");
  const std::filesystem::path output(argv[1]);
  std::filesystem::create_directories(output);
  render(output / "ultra-main.png", false);
  render(output / "ultra-cluster.png", true);
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
