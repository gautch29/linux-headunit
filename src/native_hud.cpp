// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_hud.hpp>
#include <cairo.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace headunit {
namespace {
constexpr double pi = 3.141592653589793;
constexpr std::array<HudPoint, 4> unit{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
using Matrix = std::array<double, 9>;
bool inverse_quad(const std::array<HudPoint, 4>& quad, Matrix& matrix) {
  // Eight bounded equations map the measured destination back to a unit
  // canvas. No mesh, history or optical assumptions enter the renderer.
  std::array<std::array<double, 9>, 8> equations{};
  for (unsigned i = 0; i < 4; ++i) {
    const auto [x, y] = quad[i];
    const auto [u, v] = unit[i];
    equations[2 * i] = {x, y, 1, 0, 0, 0, -u * x, -u * y, u};
    equations[2 * i + 1] = {0, 0, 0, x, y, 1, -v * x, -v * y, v};
  }
  for (unsigned col = 0; col < 8; ++col) {
    unsigned pivot = col;
    for (unsigned row = col + 1; row < 8; ++row)
      if (std::abs(equations[row][col]) > std::abs(equations[pivot][col])) pivot = row;
    if (std::abs(equations[pivot][col]) < 1e-10) return false;
    std::swap(equations[col], equations[pivot]);
    const double divisor = equations[col][col];
    for (unsigned j = col; j < 9; ++j)
      equations[col][j] /= divisor;
    for (unsigned row = 0; row < 8; ++row)
      if (row != col) {
        const double factor = equations[row][col];
        for (unsigned j = col; j < 9; ++j)
          equations[row][j] -= factor * equations[col][j];
      }
  }
  for (unsigned i = 0; i < 8; ++i)
    matrix[i] = equations[i][8];
  matrix[8] = 1;
  return true;
}
std::string bounded_text(std::string_view input) {
  std::string out;
  out.reserve(std::min(input.size(), std::size_t(256)));
  for (std::size_t at = 0; at < input.size() && at < 256;) {
    const auto first = static_cast<unsigned char>(input[at]);
    unsigned count = first < 128                      ? 1
                     : first >= 0xc2 && first <= 0xdf ? 2
                     : first >= 0xe0 && first <= 0xef ? 3
                     : first >= 0xf0 && first <= 0xf4 ? 4
                                                      : 0;
    if (!count || at + count > input.size()) return {};
    if (at + count > 256) break;
    std::uint32_t code = first & ((1u << (8 - count - 1)) - 1);
    for (unsigned j = 1; j < count; ++j) {
      const auto next = static_cast<unsigned char>(input[at + j]);
      if ((next & 0xc0) != 0x80) return {};
      code = (code << 6) | (next & 63);
    }
    if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff))
      return {};
    if (count == 1 && (first < 32 || first == 127))
      out.push_back(' ');
    else
      out.append(input.substr(at, count));
    at += count;
  }
  return out;
}
NavigationSnapshot navigation_model(const NavigationSnapshot& input, Clock::time_point now) {
  if (!input.available || !input.route_state || *input.route_state < 1 || *input.route_state > 6 ||
      (input.updated_at && *input.updated_at > now))
    return {};
  NavigationSnapshot out;
  out.available = true;
  out.route_state = input.route_state;
  out.updated_at = input.updated_at;
  out.source = bounded_text(input.source);
  out.current_road = bounded_text(input.current_road);
  out.destination = bounded_text(input.destination);
  out.remaining_m = input.remaining_m;
  out.remaining_seconds = input.remaining_seconds;
  out.arrival_unix_seconds = input.arrival_unix_seconds;
  if (*input.route_state == 1 || *input.route_state == 6) {
    out.distance_m = input.distance_m;
    if (input.maneuver) {
      const auto& m = *input.maneuver;
      out.maneuver = NavigationManeuver{m.index, m.type, bounded_text(m.instruction),
                                        bounded_text(m.road), m.exit_angle};
    }
  }
  return out;
}
void dimensions(unsigned width, unsigned height) {
  if (width < 160 || width > 4096 || height < 120 || height > 2160)
    throw std::invalid_argument("native HUD size outside bounds");
}
void label(cairo_t* c, double x, double y, double size, const std::string& value, double max_width,
           bool bold = true, double brightness = 1) {
  cairo_select_font_face(c, "sans", CAIRO_FONT_SLANT_NORMAL,
                         bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(c, size);
  cairo_text_extents_t ext{};
  cairo_text_extents(c, value.c_str(), &ext);
  if (ext.x_advance > max_width && ext.x_advance > 0) {
    cairo_set_font_size(c, size * max_width / ext.x_advance);
    cairo_text_extents(c, value.c_str(), &ext);
  }
  cairo_set_source_rgb(c, brightness, brightness, brightness);
  cairo_move_to(c, x - ext.x_advance / 2, y);
  cairo_show_text(c, value.c_str());
  cairo_new_path(c);
}
std::string reading(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.0f", value);
  return text;
}
void arrow_head(cairo_t* c, double x, double y, double dx, double dy) {
  const double length = std::hypot(dx, dy), ux = dx / length, uy = dy / length;
  cairo_move_to(c, x - 22 * ux + 15 * uy, y - 22 * uy - 15 * ux);
  cairo_line_to(c, x, y);
  cairo_line_to(c, x - 22 * ux - 15 * uy, y - 22 * uy + 15 * ux);
}
void direction(cairo_t* c, std::uint8_t type) {
  cairo_set_source_rgb(c, .66, .91, 1);
  cairo_set_line_width(c, 10);
  cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(c, CAIRO_LINE_JOIN_ROUND);
  if (type == 1 || type == 2) {
    const double d = type == 1 ? -1 : 1;
    cairo_move_to(c, 0, 40);
    cairo_line_to(c, 0, -20);
    cairo_curve_to(c, 0, -38, 9 * d, -45, 28 * d, -45);
    cairo_line_to(c, 70 * d, -45);
    arrow_head(c, 70 * d, -45, d, 0);
  } else if (type == 13 || type == 14 || type == 49 || type == 50) {
    const double d = (type == 13 || type == 49) ? -1 : 1;
    cairo_move_to(c, 0, 40);
    cairo_line_to(c, 0, 5);
    cairo_curve_to(c, 0, -15, 40 * d, -32, 52 * d, -57);
    arrow_head(c, 52 * d, -57, d, -1.5);
  } else if (type == 4) {
    cairo_move_to(c, 25, 40);
    cairo_line_to(c, 25, -30);
    cairo_curve_to(c, 25, -70, -25, -70, -25, -30);
    cairo_line_to(c, -25, 13);
    arrow_head(c, -25, 13, 0, 1);
  } else if (type == 0 || type == 3 || type == 5 || type == 11) {
    cairo_move_to(c, 0, 40);
    cairo_line_to(c, 0, -65);
    arrow_head(c, 0, -65, 0, -1);
  } else if (type == 10 || type == 12) {
    cairo_move_to(c, -25, 40);
    cairo_line_to(c, -25, -65);
    cairo_line_to(c, 35, -55);
    cairo_line_to(c, 35, -15);
    cairo_line_to(c, -25, -25);
  }  // Unknown/roundabout/exit geometry is never invented as a turn arrow.
  cairo_stroke(c);
}
void speed_sign(cairo_t* c, const NativeHudModel& model) {
  const auto sign = model.locale.sign;
  const auto value = reading(hud_speed_value(model.speed_limit->kmh, model.locale.unit));
  if (sign == HudSpeedSign::red_ring || sign == HudSpeedSign::yellow_red_ring ||
      sign == HudSpeedSign::australia) {
    if (sign == HudSpeedSign::australia) {
      cairo_set_source_rgb(c, 1, 1, 1);
      cairo_rectangle(c, 392, 92, 110, 145);
      cairo_fill(c);
    }
    cairo_set_source_rgb(c, 1, sign == HudSpeedSign::yellow_red_ring ? .83 : 1,
                         sign == HudSpeedSign::yellow_red_ring ? 0 : 1);
    cairo_arc(c, 447, 165, 49, 0, 2 * pi);
    cairo_fill_preserve(c);
    cairo_set_source_rgb(c, .94, .16, .13);
    cairo_set_line_width(c, 10);
    cairo_stroke(c);
    label(c, 447, 184, 45, value, 80, true, 0);
  } else if (sign == HudSpeedSign::us || sign == HudSpeedSign::canada) {
    cairo_set_source_rgb(c, 1, 1, 1);
    cairo_rectangle(c, 394, 96, 106, 133);
    cairo_fill(c);
    cairo_set_source_rgb(c, 0, 0, 0);
    cairo_set_line_width(c, 2);
    cairo_rectangle(c, 399, 101, 96, 123);
    cairo_stroke(c);
    if (sign == HudSpeedSign::us) {
      label(c, 447, 126, 19, "SPEED", 86, true, 0);
      label(c, 447, 147, 19, "LIMIT", 86, true, 0);
    } else
      label(c, 447, 134, 16, "MAXIMUM", 86, true, 0);
    label(c, 447, 206, 55, value, 86, true, 0);
  } else {
    // No verified country profile: retain the observed value with explicit unit.
    label(c, 447, 124, 17, "LIMIT", 110, false, .72);
    label(c, 447, 191, 52, value, 110);
  }
  label(c, 447, 252, 17, model.locale.unit == HudSpeedUnit::miles_per_hour ? "mph" : "km/h", 100,
        false, .72);
}
void paint(const NativeHudModel& model, std::uint32_t* pixels, unsigned width, unsigned height) {
  using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
  using Context = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
  Surface surface(cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(pixels),
                                                      CAIRO_FORMAT_ARGB32, int(width), int(height),
                                                      int(width * 4)),
                  cairo_surface_destroy);
  Context context(cairo_create(surface.get()), cairo_destroy);
  auto* c = context.get();
  cairo_set_source_rgb(c, 0, 0, 0);
  cairo_paint(c);
  const double scale = std::min(width / 960., height / 360.);
  cairo_translate(c, (width - 960 * scale) / 2, (height - 360 * scale) / 2);
  cairo_scale(c, scale, scale);
  label(c, 200, 230, 194,
        model.speed_kmh ? reading(hud_speed_value(*model.speed_kmh, model.locale.unit)) : "--",
        355);
  label(c, 200, 285, 28, model.locale.unit == HudSpeedUnit::miles_per_hour ? "mph" : "km/h", 220,
        false, .72);
  if (model.speed_limit) speed_sign(c, model);
  const auto& nav = model.navigation;
  if (nav.available) {
    const bool active = nav.route_state == 1 || nav.route_state == 6;
    if (active && nav.maneuver && nav.maneuver->type) {
      cairo_save(c);
      cairo_translate(c, 730, 126);
      direction(c, *nav.maneuver->type);
      cairo_restore(c);
    }
    if (active && nav.distance_m)
      label(c, 730, 242, 47, hud_distance_text(*nav.distance_m, model.locale.unit), 355);
    auto prompt = nav;
    prompt.distance_m.reset();
    label(c, 730, 293, 27, navigation_prompt(prompt), 370, false);
  }
  if (model.simulated) label(c, 480, 340, 14, "SIMULATION", 400, false, .6);
  if (cairo_status(c) != CAIRO_STATUS_SUCCESS ||
      cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS)
    throw std::runtime_error("native HUD drawing failed");
  cairo_surface_flush(surface.get());
}
std::uint32_t sample(const std::vector<std::uint32_t>& pixels, unsigned w, unsigned h, double u,
                     double v, bool exact) {
  if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || v < 0 || u >= 1 || v >= 1)
    return 0xff000000;
  if (exact)
    return pixels[std::size_t(std::min(unsigned(v * h), h - 1)) * w +
                  std::min(unsigned(u * w), w - 1)];
  const double x = std::clamp(u * w - .5, 0., double(w - 1)),
               y = std::clamp(v * h - .5, 0., double(h - 1));
  const unsigned x0 = unsigned(x), y0 = unsigned(y), x1 = std::min(x0 + 1, w - 1),
                 y1 = std::min(y0 + 1, h - 1);
  const double a = x - x0, b = y - y0;
  std::uint32_t result = 0xff000000;
  for (unsigned shift : {0u, 8u, 16u}) {
    const auto channel = [&](unsigned px, unsigned py) {
      return double((pixels[std::size_t(py) * w + px] >> shift) & 255);
    };
    const double top = channel(x0, y0) * (1 - a) + channel(x1, y0) * a;
    const double bottom = channel(x0, y1) * (1 - a) + channel(x1, y1) * a;
    result |= std::uint32_t(std::lround(top * (1 - b) + bottom * b)) << shift;
  }
  return result;
}
void transform(const std::vector<std::uint32_t>& upright, unsigned sw, unsigned sh,
               std::uint32_t* pixels, unsigned width, unsigned height, std::size_t stride,
               const HudCalibration& calibration) {
  Matrix inverse{1, 0, 0, 0, 1, 0, 0, 0, 1};
  if (calibration.corners && !inverse_quad(*calibration.corners, inverse))
    throw std::invalid_argument("singular HUD calibration");
  const bool exact = !calibration.corners && calibration.scale == 1 && calibration.offset_x == 0 &&
                     calibration.offset_y == 0;
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      const double px = (x + .5) / width, py = (y + .5) / height;
      const double denominator = inverse[6] * px + inverse[7] * py + 1;
      double u =
          denominator == 0 ? -1 : (inverse[0] * px + inverse[1] * py + inverse[2]) / denominator;
      double v =
          denominator == 0 ? -1 : (inverse[3] * px + inverse[4] * py + inverse[5]) / denominator;
      if (u < 0 || v < 0 || u >= 1 || v >= 1) {
        pixels[std::size_t(y) * (stride / 4) + x] = 0xff000000;
        continue;
      }
      u = (u - .5 - calibration.offset_x) / calibration.scale + .5;
      v = (v - .5 - calibration.offset_y) / calibration.scale + .5;
      if (calibration.reflect_horizontal) u = 1 - u;
      if (calibration.reflect_vertical) v = 1 - v;
      const double a = u, b = v;
      if (calibration.quarter_turns == 1) {
        u = b;
        v = 1 - a;
      } else if (calibration.quarter_turns == 2) {
        u = 1 - a;
        v = 1 - b;
      } else if (calibration.quarter_turns == 3) {
        u = 1 - b;
        v = a;
      }
      pixels[std::size_t(y) * (stride / 4) + x] = sample(upright, sw, sh, u, v, exact);
    }
}
Image rgb_image(const std::vector<std::uint32_t>& frame, unsigned width, unsigned height) {
  Image image{width, height, std::vector<Rgb>(frame.size())};
  for (std::size_t i = 0; i < frame.size(); ++i)
    image.pixels[i] = {std::uint8_t(frame[i] >> 16), std::uint8_t(frame[i] >> 8),
                       std::uint8_t(frame[i])};
  return image;
}
}  // namespace
std::string hud_distance_text(std::uint32_t metres, HudSpeedUnit unit) {
  char text[32];
  if (unit == HudSpeedUnit::miles_per_hour) {
    // One international mile is exactly 1609.344 metres; feet and miles
    // follow the same display locale as speed without changing source data.
    if (metres < 161)
      std::snprintf(text, sizeof(text), "%.0f ft", metres / .3048);
    else
      std::snprintf(text, sizeof(text), "%.1f mi", metres / 1609.344);
  } else if (metres < 1000)
    std::snprintf(text, sizeof(text), "%u m", metres);
  else
    std::snprintf(text, sizeof(text), "%.1f km", metres / 1000.);
  return text;
}
bool valid_hud_calibration(const HudCalibration& calibration) noexcept {
  const auto& c = calibration;
  if (c.quarter_turns > 3 || !std::isfinite(c.scale) || c.scale < .25 || c.scale > 1 ||
      !std::isfinite(c.offset_x) || !std::isfinite(c.offset_y) || std::abs(c.offset_x) > .5 ||
      std::abs(c.offset_y) > .5)
    return false;
  if (!c.corners) return true;
  for (const auto& p : *c.corners)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < 0 || p.x > 1 || p.y < 0 || p.y > 1)
      return false;
  for (unsigned i = 0; i < 4; ++i) {
    const auto a = (*c.corners)[i], b = (*c.corners)[(i + 1) % 4], d = (*c.corners)[(i + 2) % 4];
    if ((b.x - a.x) * (d.y - b.y) - (b.y - a.y) * (d.x - b.x) < 1e-6) return false;
  }
  Matrix matrix{};
  return inverse_quad(*c.corners, matrix);
}
void validate_hud_calibration(const HudCalibration& c) {
  if (!valid_hud_calibration(c)) throw std::invalid_argument("invalid HUD calibration");
}
bool valid_hud_country(std::string_view country) noexcept {
  return country.empty() || (country.size() == 2 && country[0] >= 'A' && country[0] <= 'Z' &&
                             country[1] >= 'A' && country[1] <= 'Z');
}
HudRoadLocale hud_road_locale(std::string_view country) noexcept {
  if (country == "US") return {HudSpeedUnit::miles_per_hour, HudSpeedSign::us};
  if (country == "CA") return {HudSpeedUnit::kilometres_per_hour, HudSpeedSign::canada};
  if (country == "GB") return {HudSpeedUnit::miles_per_hour, HudSpeedSign::red_ring};
  if (country == "AU") return {HudSpeedUnit::kilometres_per_hour, HudSpeedSign::australia};
  if (country == "SE" || country == "FI")
    return {HudSpeedUnit::kilometres_per_hour, HudSpeedSign::yellow_red_ring};
  constexpr std::array countries{"FR", "DE", "AT", "BE", "CH", "CZ", "DK", "EE", "ES",
                                 "GR", "HR", "HU", "IE", "IS", "IT", "LT", "LU", "LV",
                                 "MT", "NL", "NO", "PL", "PT", "RO", "SI", "SK", "NZ"};
  if (std::find(countries.begin(), countries.end(), country) != countries.end())
    return {HudSpeedUnit::kilometres_per_hour, HudSpeedSign::red_ring};
  return {};
}
double hud_speed_value(double kmh, HudSpeedUnit unit) noexcept {
  return unit == HudSpeedUnit::miles_per_hour ? kmh / 1.609344 : kmh;
}
NativeHudModel native_hud_model(const NativeHudContent& content, Clock::time_point now) {
  NativeHudModel model;
  model.locale = hud_road_locale(content.country);
  const auto current = current_vehicle_sample(content.vehicle, now);
  if (current.speed_kmh && std::isfinite(*current.speed_kmh) && *current.speed_kmh >= 0 &&
      *current.speed_kmh <= 300) {
    model.speed_kmh = current.speed_kmh;
    model.speed_sampled_at = content.vehicle.sampled_at;
    if (current.observation_timing) model.speed_timing = current.observation_timing->readings[0];
    model.simulated =
        current.simulated ||
        (model.speed_timing && model.speed_timing->origin == VehicleObservationOrigin::simulation);
  }
  model.navigation = navigation_model(content.navigation, now);
  if (content.speed_limit && fresh(*content.speed_limit, now)) {
    model.speed_limit = content.speed_limit;
    const auto& country =
        content.speed_limit->country.empty() ? content.country : content.speed_limit->country;
    model.locale = hud_road_locale(country);
    if (model.locale.sign == HudSpeedSign::neutral) model.locale.unit = content.speed_limit->unit;
    model.simulated = model.simulated ||
                      content.speed_limit->timing.origin == VehicleObservationOrigin::simulation;
  }
  return model;
}
void draw_native_hud(const NativeHudContent& content, Clock::time_point now, std::uint32_t* pixels,
                     unsigned width, unsigned height, std::size_t stride,
                     const HudCalibration& calibration) {
  dimensions(width, height);
  validate_hud_calibration(calibration);
  if (!pixels || stride < std::size_t(width) * 4 || stride % 4 || stride > std::size_t(width) * 16)
    throw std::invalid_argument("invalid native HUD frame");
  const unsigned sw = calibration.quarter_turns % 2 ? height : width,
                 sh = calibration.quarter_turns % 2 ? width : height;
  std::vector<std::uint32_t> upright(std::size_t(sw) * sh);
  paint(native_hud_model(content, now), upright.data(), sw, sh);
  transform(upright, sw, sh, pixels, width, height, stride, calibration);
}
Image render_native_hud(const NativeHudContent& content, Clock::time_point now, unsigned width,
                        unsigned height, const HudCalibration& calibration) {
  dimensions(width, height);
  validate_hud_calibration(calibration);
  std::vector<std::uint32_t> frame(std::size_t(width) * height);
  draw_native_hud(content, now, frame.data(), width, height, std::size_t(width) * 4, calibration);
  return rgb_image(frame, width, height);
}
Image render_hud_calibration_grid(unsigned width, unsigned height,
                                  const HudCalibration& calibration) {
  dimensions(width, height);
  validate_hud_calibration(calibration);
  const unsigned sw = calibration.quarter_turns % 2 ? height : width,
                 sh = calibration.quarter_turns % 2 ? width : height;
  std::vector<std::uint32_t> source(std::size_t(sw) * sh), frame(std::size_t(width) * height);
  const std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
      cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(source.data()),
                                          CAIRO_FORMAT_ARGB32, int(sw), int(sh), int(sw * 4)),
      cairo_surface_destroy);
  const std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()),
                                                                   cairo_destroy);
  auto* c = context.get();
  cairo_set_source_rgb(c, 0, 0, 0);
  cairo_paint(c);
  cairo_scale(c, sw / 960., sh / 360.);
  cairo_set_source_rgb(c, .4, .4, .4);
  cairo_set_line_width(c, 1);
  for (unsigned x = 20; x <= 940; x += 92) {
    cairo_move_to(c, x, 20);
    cairo_line_to(c, x, 340);
  }
  for (unsigned y = 20; y <= 340; y += 40) {
    cairo_move_to(c, 20, y);
    cairo_line_to(c, 940, y);
  }
  cairo_stroke(c);
  for (auto [p, text] : std::array<std::pair<HudPoint, const char*>, 4>{
           {{{85, 60}, "TL"}, {{875, 60}, "TR"}, {{875, 325}, "BR"}, {{85, 325}, "BL"}}})
    label(c, p.x, p.y, 28, text, 100);
  cairo_save(c);
  cairo_translate(c, 480, 157);
  direction(c, 0);
  cairo_restore(c);
  label(c, 480, 272, 24, "CALIBRATION  ·  UP", 650, false);
  if (cairo_status(c) != CAIRO_STATUS_SUCCESS)
    throw std::runtime_error("HUD calibration grid failed");
  cairo_surface_flush(surface.get());
  transform(source, sw, sh, frame.data(), width, height, std::size_t(width) * 4, calibration);
  return rgb_image(frame, width, height);
}
}  // namespace headunit
