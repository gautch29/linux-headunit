// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/warning_overlay.hpp>
#include <headunit/indicator_symbols.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <cairo.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace headunit::theme::graphics {
namespace {
constexpr double pi = std::numbers::pi;
void text(cairo_t* c, std::string_view label, double x, double y, double size) {
  cairo_select_font_face(c, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(c, size);
  const std::string s(label);
  cairo_text_extents_t e{};
  cairo_text_extents(c, s.c_str(), &e);
  cairo_move_to(c, x - e.width / 2 - e.x_bearing, y);
  cairo_show_text(c, s.c_str());
}
void rounded(cairo_t* c, double x, double y, double width, double height, double radius) {
  cairo_new_sub_path(c);
  cairo_arc(c, x + width - radius, y + radius, radius, -pi / 2, 0);
  cairo_arc(c, x + width - radius, y + height - radius, radius, 0, pi / 2);
  cairo_arc(c, x + radius, y + height - radius, radius, pi / 2, pi);
  cairo_arc(c, x + radius, y + radius, radius, pi, 3 * pi / 2);
  cairo_close_path(c);
}
std::shared_ptr<Layer> tile(Rect frame, const IndicatorDefinition* definition,
                            bool unavailable = false, double intensity = 1) {
  auto image = std::make_shared<Image>();
  image->width = unsigned(std::ceil(frame.size.width * 2));
  image->height = unsigned(std::ceil(frame.size.height * 2));
  image->pixels.assign(image->width * image->height, 0);
  auto* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(image->pixels.data()), CAIRO_FORMAT_ARGB32,
      int(image->width), int(image->height), int(image->width * 4));
  auto* c = cairo_create(surface);
  cairo_scale(c, image->width / frame.size.width, image->height / frame.size.height);
  if (unavailable) {
    draw_turn_signal_unavailable(c, definition->id, 0, 0, frame.size.width, frame.size.height);
  } else if (definition &&
             (definition->id == Indicator::left_turn || definition->id == Indicator::right_turn)) {
    draw_turn_signal_lamp(c, definition->id, 0, 0, frame.size.width, frame.size.height, intensity);
  } else if (definition) {
    draw_indicator_telltale(c, *definition, 0, 0, frame.size.width, frame.size.height);
  } else {
    rounded(c, .5, .5, frame.size.width - 1, frame.size.height - 1, 8);
    cairo_set_source_rgba(c, .015, .02, .03, .9);
    cairo_fill(c);
    cairo_set_source_rgba(c, 1, .72, .16, 1);
    text(c, "SIMULATED", frame.size.width / 2, frame.size.height / 2 + 4, 10);
  }
  const auto status = cairo_status(c);
  cairo_destroy(c);
  cairo_surface_flush(surface);
  const auto surface_status = cairo_surface_status(surface);
  cairo_surface_destroy(surface);
  if (status != CAIRO_STATUS_SUCCESS || surface_status != CAIRO_STATUS_SUCCESS)
    throw std::runtime_error("warning symbol rendering failed");
  auto result = std::make_shared<Layer>(Kind::image);
  result->image = std::move(image);
  result->set_frame(frame);
  return result;
}
}  // namespace
struct WarningOverlay::Impl {
  WarningOverlayConfiguration config;
  // 32 brightness intervals + off + unavailable. Every animation state has
  // a retained immutable tree; animation ticks only select an existing pointer.
  static constexpr std::size_t levels = 32;
  static constexpr std::size_t states = levels + 2;
  std::vector<std::shared_ptr<Layer>> tiles;
  std::array<std::array<std::shared_ptr<Layer>, states>, 2> turns;
  std::array<std::shared_ptr<const Layer>, states * states> scenes;
  std::shared_ptr<Layer> badge;
  TurnSignalAnimation animation;
  std::bitset<24> last_visible;
  bool last_simulated = false;
  bool prepared = false;
  Size last_output{};
  explicit Impl(WarningOverlayConfiguration value) : config(std::move(value)) {
    validate_warning_overlay_configuration(config);
    if (!config.enabled) return;
    for (const auto& p : config.indicators) {
      const auto& definition = indicator_definitions.at(static_cast<std::size_t>(p.signal));
      const bool direction = p.signal == Indicator::left_turn || p.signal == Indicator::right_turn;
      tiles.push_back(direction ? nullptr : tile(p.frame, &definition));
      if (!direction) continue;
      auto& frames = turns[p.signal == Indicator::right_turn ? 1 : 0];
      for (std::size_t level = 1; level <= levels; ++level)
        frames[level] = tile(p.frame, &definition, false, double(level) / levels);
      frames.back() = tile(p.frame, &definition, true);
    }
    badge = tile(config.simulation_badge, nullptr);
  }
  void prepare(std::bitset<24> visible, bool simulated, Size output) {
    // Only non-turn topology/output changes allocate. On/off, fades, unknown
    // and shared clock phase all reuse this bounded table, including held trees.
    for (std::size_t left = 0; left < states; ++left) {
      for (std::size_t right = 0; right < states; ++right) {
        auto root = std::make_shared<Layer>();
        root->anchor = {0, 0};
        root->group_blending = false;
        root->children.reserve(config.indicators.size() + 1);
        for (std::size_t i = 0; i < config.indicators.size(); ++i) {
          const auto signal = config.indicators[i].signal;
          if (signal == Indicator::left_turn || signal == Indicator::right_turn) {
            const bool is_right = signal == Indicator::right_turn;
            const auto& node = turns[is_right ? 1 : 0][is_right ? right : left];
            if (node) root->children.push_back(node);
          } else if (visible[static_cast<std::size_t>(signal)]) {
            root->children.push_back(tiles[i]);
          }
        }
        if (simulated) root->children.push_back(badge);
        scenes[left * states + right] =
            root->children.empty() ? nullptr : map_scene_canvas(root, config.canvas, output);
      }
    }
    last_visible = visible;
    last_simulated = simulated;
    last_output = output;
    prepared = true;
  }
};
WarningOverlay::WarningOverlay(WarningOverlayConfiguration config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
WarningOverlay::~WarningOverlay() = default;
void WarningOverlay::set_hide_during_startup(bool hide) {
  impl_->config.hide_during_startup = hide;
}
std::shared_ptr<const Layer> WarningOverlay::scene(const VehicleSample& sample,
                                                   Clock::time_point now, Size output,
                                                   bool startup) {
  auto& p = *impl_;
  // Validate output even when all lamps are off; do not hide configuration errors.
  (void)map_scene_canvas({}, p.config.canvas, output);
  if (!p.config.enabled) return {};
  if (startup && p.config.hide_during_startup) {
    p.animation.reset();
    return {};
  }
  const auto snapshot = cluster_indicator_snapshot(sample, now, p.config.maximum_age);
  std::bitset<24> visible;
  for (const auto& entry : p.config.indicators) {
    if (entry.signal == Indicator::left_turn || entry.signal == Indicator::right_turn) continue;
    const auto index = std::size_t(entry.signal);
    visible[index] = snapshot.visible[index];
  }
  const bool simulated = snapshot.fresh_data && snapshot.simulated;
  if (!p.prepared || visible != p.last_visible || simulated != p.last_simulated ||
      p.last_output.width != output.width || p.last_output.height != output.height)
    p.prepare(visible, simulated, output);
  const auto display = p.animation.update(sample, now, p.config.maximum_age);
  const auto level = [&](std::size_t index) {
    return display.unavailable[index]
               ? Impl::states - 1
               : static_cast<std::size_t>(std::lround(display.intensity[index] * Impl::levels));
  };
  return p.scenes[level(0) * Impl::states + level(1)];
}
}  // namespace headunit::theme::graphics
