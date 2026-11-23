// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/indicator_symbols.hpp>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <stdexcept>
#include <string>

namespace headunit {
ClusterIndicatorSnapshot cluster_indicator_snapshot(const VehicleSample& sample,
                                                    Clock::time_point now,
                                                    Clock::duration maximum_age) {
  ClusterIndicatorSnapshot result;
  static_cast<VehicleIndicatorSnapshot&>(result) =
      vehicle_indicator_snapshot(sample, now, maximum_age);
  result.visible.reset(static_cast<std::size_t>(Indicator::hazard));
  const auto current = current_vehicle_sample(sample, now, maximum_age);
  result.turn_states = {current.indicator(Indicator::left_turn),
                        current.indicator(Indicator::right_turn)};
  const auto hazard = current.indicator(Indicator::hazard);
  if (result.fresh_data && hazard && *hazard != IndicatorState::off) {
    // An observed lamp phase outranks a hazard-enable/flashing summary. A
    // single observed phase can supply the paired lamp without inventing a
    // sensor reading. Conflicting explicit observations remain authoritative.
    for (std::size_t i = 0; i < 2; ++i) {
      if (result.turn_states[i] && *result.turn_states[i] != IndicatorState::flashing) continue;
      const auto other = result.turn_states[1 - i];
      result.turn_states[i] = other && *other != IndicatorState::flashing
                                  ? other
                                  : std::optional{IndicatorState::flashing};
    }
  }
  for (std::size_t i = 0; i < 2; ++i) {
    if (!result.fresh_data) result.turn_states[i].reset();
    const auto state = result.turn_states[i];
    result.unavailable_turns.set(i, !state);
    const bool lit =
        state && (*state == IndicatorState::on ||
                  (*state == IndicatorState::flashing && indicator_flash_visible(now)));
    result.visible.set(std::size_t(i ? Indicator::right_turn : Indicator::left_turn), lit);
  }
  return result;
}
namespace {
double ease(double fraction) {
  const double t = std::clamp(fraction, 0., 1.);
  return t * t * (3 - 2 * t);
}
double lamp_intensity(bool target, double from, Clock::time_point changed, Clock::time_point now) {
  const double duration = target ? 40. : 120.;
  const double elapsed = std::chrono::duration<double, std::milli>(now - changed).count();
  if (elapsed >= duration) return target ? 1. : 0.;
  return from + ((target ? 1. : 0.) - from) * ease(elapsed / duration);
}
}  // namespace
void TurnSignalAnimation::reset() noexcept {
  lamps_ = {};
  last_update_ = {};
  generation_ = 0;
}
TurnSignalDisplay TurnSignalAnimation::update(const VehicleSample& sample, Clock::time_point now,
                                              Clock::duration maximum_age) {
  const auto generation = sample.observation_timing ? sample.observation_timing->generation : 0;
  if (now < last_update_ || generation != generation_) reset();
  generation_ = generation;
  const auto previous_update = last_update_;
  last_update_ = now;
  const auto snapshot = cluster_indicator_snapshot(sample, now, maximum_age);
  TurnSignalDisplay result;
  result.unavailable = snapshot.unavailable_turns;
  for (std::size_t i = 0; i < 2; ++i) {
    auto& lamp = lamps_[i];
    const auto state = snapshot.turn_states[i];
    if (!state) {
      lamp = {};
      continue;
    }
    const bool target = *state == IndicatorState::on ||
                        (*state == IndicatorState::flashing && indicator_flash_visible(now));
    const double previous =
        lamp.known ? lamp_intensity(lamp.target, lamp.from, lamp.changed_at, now) : 0.;
    if (*state == IndicatorState::flashing) {
      // After initial availability easing, fallback edges share the clock in
      // every renderer. A first dark sample cannot invent prior lamp energy.
      const auto ticks =
          std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
      const auto phase = ((ticks % 2000000000LL) * 3) % 2000000000LL;
      const auto elapsed = std::chrono::nanoseconds((phase % 1000000000LL) / 3);
      if (!lamp.known || !lamp.flashing)
        lamp = {true, target, previous, now, true};
      else if (lamp.target != target || now - previous_update >= std::chrono::microseconds(333334))
        lamp = {true, target, target ? 0. : 1., now - elapsed, true};
    } else if (!lamp.known || lamp.target != target || lamp.flashing) {
      lamp = {true, target, previous, now, false};
    }
    result.intensity[i] = lamp_intensity(lamp.target, lamp.from, lamp.changed_at, now);
    result.animating |= result.intensity[i] != (target ? 1. : 0.);
  }
  return result;
}

namespace {
struct Point {
  double x, y;
};
constexpr double pi = std::numbers::pi;
void arrow_path(cairo_t* c, Indicator signal) {
  if (signal == Indicator::right_turn) {
    cairo_translate(c, 64, 0);
    cairo_scale(c, -1, 1);
  }
  cairo_move_to(c, 8, 20);
  for (const Point p :
       std::initializer_list<Point>{{27, 5}, {27, 14}, {55, 14}, {55, 26}, {27, 26}, {27, 35}})
    cairo_line_to(c, p.x, p.y);
  cairo_close_path(c);
}

void stroke(cairo_t* c, std::initializer_list<Point> points, bool close = false) {
  bool first = true;
  for (const auto p : points) {
    if (first)
      cairo_move_to(c, p.x, p.y);
    else
      cairo_line_to(c, p.x, p.y);
    first = false;
  }
  if (close) cairo_close_path(c);
  cairo_stroke(c);
}
void circle(cairo_t* c, double x, double y, double radius, bool fill = false) {
  cairo_new_path(c);
  cairo_arc(c, x, y, radius, 0, 2 * pi);
  if (fill)
    cairo_fill(c);
  else
    cairo_stroke(c);
}
void text(cairo_t* c, std::string_view label, double x, double y, double size) {
  cairo_select_font_face(c, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(c, size);
  const std::string s(label);
  cairo_text_extents_t e{};
  cairo_text_extents(c, s.c_str(), &e);
  cairo_move_to(c, x - e.width / 2 - e.x_bearing, y);
  cairo_show_text(c, s.c_str());
}
void exclamation(cairo_t* c, double x) {
  stroke(c, {{x, 10}, {x, 24}});
  circle(c, x, 31, 1.4, true);
}
void waves(cairo_t* c, double y) {
  for (int row = 0; row < 2; ++row) {
    cairo_move_to(c, 8, y + row * 6);
    for (int x = 8; x < 56; x += 16)
      cairo_curve_to(c, x + 4, y + row * 6 - 4, x + 12, y + row * 6 + 4, x + 16, y + row * 6);
    cairo_stroke(c);
  }
}
void car(cairo_t* c) {
  stroke(c, {{23, 7}, {41, 7}, {47, 18}, {47, 34}, {17, 34}, {17, 18}}, true);
  stroke(c, {{23, 10}, {21, 19}, {43, 19}, {41, 10}}, true);
}
void symbol(cairo_t* c, Indicator signal) {
  cairo_set_line_width(c, 2.7);
  cairo_set_line_join(c, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
  switch (signal) {
    case Indicator::left_turn:
    case Indicator::right_turn: {
      arrow_path(c, signal);
      cairo_fill(c);
      break;
    }
    case Indicator::hazard:
      stroke(c, {{32, 3}, {59, 37}, {5, 37}}, true);
      stroke(c, {{32, 14}, {48, 33}, {16, 33}}, true);
      break;
    case Indicator::high_beam:
    case Indicator::low_beam:
    case Indicator::front_fog:
    case Indicator::rear_fog: {
      if (signal == Indicator::rear_fog) {
        cairo_translate(c, 64, 0);
        cairo_scale(c, -1, 1);
      }
      cairo_move_to(c, 34, 6);
      cairo_curve_to(c, 58, 5, 58, 35, 34, 34);
      cairo_close_path(c);
      cairo_stroke(c);
      for (int y = 8; y <= 32; y += 8)
        stroke(
            c,
            {{9, double(y) +
                     ((signal == Indicator::low_beam || signal == Indicator::front_fog) ? 6 : 0)},
             {28, double(y)}});
      if (signal == Indicator::front_fog || signal == Indicator::rear_fog) {
        cairo_move_to(c, 19, 4);
        cairo_curve_to(c, 11, 14, 27, 25, 19, 37);
        cairo_stroke(c);
      }
      break;
    }
    case Indicator::position_lights:
      cairo_move_to(c, 26, 8);
      cairo_curve_to(c, 12, 8, 12, 32, 26, 32);
      cairo_close_path(c);
      cairo_stroke(c);
      cairo_move_to(c, 38, 8);
      cairo_curve_to(c, 52, 8, 52, 32, 38, 32);
      cairo_close_path(c);
      cairo_stroke(c);
      for (int y = 10; y <= 30; y += 10) {
        stroke(c, {{2, double(y)}, {10, double(y)}});
        stroke(c, {{54, double(y)}, {62, double(y)}});
      }
      break;
    case Indicator::check_engine:
      stroke(c,
             {{10, 12},
              {23, 12},
              {23, 6},
              {39, 6},
              {39, 12},
              {48, 12},
              {48, 17},
              {55, 17},
              {55, 31},
              {45, 31},
              {40, 36},
              {16, 36},
              {10, 29}},
             true);
      stroke(c, {{4, 13}, {4, 27}});
      stroke(c, {{28, 2}, {36, 2}});
      break;
    case Indicator::oil_pressure:
      stroke(c, {{10, 14}, {35, 14}, {44, 7}, {53, 13}, {37, 26}, {13, 26}}, true);
      stroke(c, {{10, 15}, {4, 15}, {4, 24}, {12, 24}});
      stroke(c, {{20, 8}, {29, 8}, {29, 14}});
      cairo_move_to(c, 53, 23);
      cairo_curve_to(c, 45, 33, 49, 37, 53, 37);
      cairo_curve_to(c, 57, 37, 61, 33, 53, 23);
      cairo_fill(c);
      break;
    case Indicator::battery_charge:
      cairo_rectangle(c, 8, 9, 48, 27);
      cairo_stroke(c);
      stroke(c, {{17, 6}, {23, 6}});
      stroke(c, {{41, 6}, {47, 6}});
      stroke(c, {{17, 22}, {27, 22}});
      stroke(c, {{22, 17}, {22, 27}});
      stroke(c, {{38, 22}, {48, 22}});
      break;
    case Indicator::brake_fault:
    case Indicator::parking_brake:
    case Indicator::abs:
      circle(c, 32, 20, 16);
      text(c,
           signal == Indicator::parking_brake ? "P"
           : signal == Indicator::abs         ? "ABS"
                                              : "!",
           32, 27, signal == Indicator::abs ? 13 : 22);
      cairo_new_path(c);
      cairo_arc(c, 32, 20, 23, -.8, .8);
      cairo_stroke(c);
      cairo_arc(c, 32, 20, 23, pi - .8, pi + .8);
      cairo_stroke(c);
      break;
    case Indicator::stability_control:
    case Indicator::traction_control:
      cairo_save(c);
      cairo_translate(c, 0, -3);
      cairo_scale(c, 1, .7);
      car(c);
      cairo_restore(c);
      for (const double x : {18., 39.}) {
        cairo_move_to(c, x, 27);
        cairo_curve_to(c, x + 10, 30, x - 5, 34, x + 5, 38);
        cairo_stroke(c);
      }
      break;
    case Indicator::airbag:
      circle(c, 20, 7, 4, true);
      stroke(c, {{20, 15}, {17, 26}, {29, 26}, {34, 36}});
      circle(c, 42, 18, 9);
      break;
    case Indicator::seat_belt:
      circle(c, 32, 6, 4, true);
      stroke(c, {{26, 15}, {23, 28}, {19, 36}});
      stroke(c, {{38, 15}, {41, 28}, {45, 36}});
      stroke(c, {{20, 31}, {43, 12}});
      stroke(c, {{25, 31}, {40, 31}});
      break;
    case Indicator::door_open:
      car(c);
      stroke(c, {{17, 19}, {7, 24}, {7, 32}, {17, 28}});
      stroke(c, {{47, 19}, {57, 24}, {57, 32}, {47, 28}});
      break;
    case Indicator::low_fuel:
      stroke(c, {{12, 36}, {12, 6}, {34, 6}, {34, 36}}, true);
      stroke(c, {{17, 11}, {29, 11}, {29, 21}, {17, 21}}, true);
      stroke(c, {{34, 21}, {42, 21}, {42, 32}, {50, 32}, {50, 13}, {44, 7}});
      stroke(c, {{10, 37}, {37, 37}});
      break;
    case Indicator::coolant_warning:
      circle(c, 32, 22, 5);
      stroke(c, {{29, 17}, {29, 4}, {35, 4}, {35, 17}});
      for (int y = 6; y <= 14; y += 4)
        stroke(c, {{35, double(y)}, {42, double(y)}});
      waves(c, 30);
      break;
    case Indicator::washer_low:
      cairo_move_to(c, 8, 18);
      cairo_curve_to(c, 19, 8, 45, 8, 56, 18);
      cairo_line_to(c, 49, 36);
      cairo_line_to(c, 15, 36);
      cairo_close_path(c);
      cairo_stroke(c);
      stroke(c, {{32, 28}, {32, 9}});
      stroke(c, {{25, 10}, {18, 3}});
      stroke(c, {{39, 10}, {46, 3}});
      break;
    case Indicator::tyre_pressure:
      cairo_move_to(c, 14, 5);
      cairo_curve_to(c, 1, 33, 12, 37, 32, 37);
      cairo_curve_to(c, 52, 37, 63, 33, 50, 5);
      cairo_stroke(c);
      exclamation(c, 32);
      break;
    case Indicator::glow_plug:
      cairo_move_to(c, 5, 23);
      cairo_line_to(c, 14, 23);
      for (int x = 14; x < 50; x += 12)
        cairo_curve_to(c, x + 16, 0, x - 4, 0, x + 12, 23);
      cairo_line_to(c, 59, 23);
      cairo_stroke(c);
      break;
    case Indicator::count:
      throw std::invalid_argument("invalid warning symbol");
  }
}
}  // namespace
void draw_indicator_symbol(cairo_t* c, Indicator signal, double x, double y, double width,
                           double height) {
  if (!c || static_cast<unsigned>(signal) >= static_cast<unsigned>(Indicator::count) ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
      width <= 0 || height <= 0 || width > 4096 || height > 4096)
    throw std::invalid_argument("invalid indicator symbol frame");
  const double scale = std::min(width / 64, height / 40);
  cairo_save(c);
  cairo_new_path(c);
  cairo_rectangle(c, x, y, width, height);
  cairo_clip(c);
  cairo_translate(c, x + (width - 64 * scale) / 2, y + (height - 40 * scale) / 2);
  cairo_scale(c, scale, scale);
  symbol(c, signal);
  cairo_restore(c);
}
namespace {
void turn_frame(cairo_t* c, Indicator signal, double x, double y, double width, double height) {
  if (!c || (signal != Indicator::left_turn && signal != Indicator::right_turn) ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
      width <= 16 || height <= 16 || width > 4096 || height > 4096)
    throw std::invalid_argument("invalid direction frame");
  cairo_rectangle(c, x, y, width, height);
  cairo_clip(c);
  // 60 rather than 48 logical pixels in the standard slot: 25% larger.
  const double scale = std::min((width - 4) / 64, (height - 4) / 40);
  cairo_translate(c, x + (width - 64 * scale) / 2, y + (height - 40 * scale) / 2);
  cairo_scale(c, scale, scale);
  cairo_set_antialias(c, CAIRO_ANTIALIAS_GRAY);
}
}  // namespace
void draw_turn_signal_unavailable(cairo_t* c, Indicator signal, double x, double y, double width,
                                  double height) {
  if (!c) throw std::invalid_argument("missing direction context");
  cairo_save(c);
  try {
    turn_frame(c, signal, x, y, width, height);
  } catch (...) {
    cairo_restore(c);
    throw;
  }
  cairo_set_source_rgba(c, .64, .66, .69, .32);
  cairo_set_line_width(c, 1.3);
  cairo_set_line_join(c, CAIRO_LINE_JOIN_MITER);
  const double dash[]{3., 3.};
  cairo_set_dash(c, dash, 2, 0);
  arrow_path(c, signal);
  cairo_stroke(c);
  cairo_restore(c);
}
void draw_turn_signal_lamp(cairo_t* c, Indicator signal, double x, double y, double width,
                           double height, double intensity) {
  if (!std::isfinite(intensity) || intensity < 0 || intensity > 1)
    throw std::invalid_argument("invalid direction intensity");
  if (!c) throw std::invalid_argument("missing direction context");
  cairo_save(c);
  try {
    turn_frame(c, signal, x, y, width, height);
  } catch (...) {
    cairo_restore(c);
    throw;
  }
  // Bounded concentric bloom, no gradient/filter allocation per animation tick.
  // Outer radius ~1.5 times the solid symbol's half-width, alpha stays subtle.
  for (int ring = 0; ring < 8; ++ring) {
    const double radius = 35. - ring * 2.;
    cairo_save(c);
    cairo_translate(c, 31.5, 20);
    cairo_scale(c, 1, .72);
    cairo_arc(c, 0, 0, radius, 0, 2 * pi);
    cairo_set_source_rgba(c, 43. / 255, 212. / 255, 91. / 255, .012 * intensity);
    cairo_fill(c);
    cairo_restore(c);
  }
  cairo_set_source_rgba(c, 43. / 255, 212. / 255, 91. / 255, intensity);
  arrow_path(c, signal);
  cairo_fill(c);
  cairo_restore(c);
}
void draw_indicator_telltale(cairo_t* c, const IndicatorDefinition& definition, double x, double y,
                             double width, double height) {
  if (!c || static_cast<unsigned>(definition.id) >= static_cast<unsigned>(Indicator::count) ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
      width <= 16 || height <= 16 || width > 4096 || height > 4096)
    throw std::invalid_argument("invalid indicator telltale frame");
  if (definition.id == Indicator::left_turn || definition.id == Indicator::right_turn) {
    draw_turn_signal_lamp(c, definition.id, x, y, width, height);
    return;
  }
  double r = 0, g = 0, b = 0;
  switch (definition.color) {
    case IndicatorColor::green:
      r = .23;
      g = .94;
      b = .47;
      break;
    case IndicatorColor::blue:
      r = .23;
      g = .63;
      b = 1;
      break;
    case IndicatorColor::amber:
      r = 1;
      g = .72;
      b = .16;
      break;
    case IndicatorColor::red:
      r = 1;
      g = .28;
      b = .29;
      break;
    default:
      throw std::invalid_argument("invalid indicator telltale color");
  }
  cairo_save(c);
  cairo_set_source_rgb(c, r, g, b);
  // Native draws at output scale while Ultra caches a 2x tile. Device-grid
  // hinting would give P/ABS different shapes; keep their outlines scalable.
  auto* options = cairo_font_options_create();
  cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
  cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
  cairo_set_font_options(c, options);
  cairo_font_options_destroy(options);
  draw_indicator_symbol(c, definition.id, x + 8, y + 8, width - 16, height - 16);
  cairo_restore(c);
}
}  // namespace headunit
