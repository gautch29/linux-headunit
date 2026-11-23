// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/warning_overlay.hpp>
#include "vehicle_json.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace headunit::theme::graphics {
namespace {
using V = opack::Value;
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
const opack::Dictionary& object(const V& v, std::initializer_list<std::string_view> keys,
                                std::initializer_list<std::string_view> optional = {}) {
  const auto* o = std::get_if<opack::Dictionary>(&v.data);
  check(o && o->size() >= keys.size() && o->size() <= keys.size() + optional.size(),
        "invalid warning overlay object fields");
  for (const auto& [key, value] : *o) {
    (void)value;
    const auto* s = std::get_if<std::string>(&key.data);
    check(s && (std::find(keys.begin(), keys.end(), *s) != keys.end() ||
                std::find(optional.begin(), optional.end(), *s) != optional.end()),
          "unknown warning overlay field");
  }
  return *o;
}
const V& field(const opack::Dictionary& o, std::string_view key) {
  for (const auto& [k, v] : o)
    if (std::get<std::string>(k.data) == key) return v;
  throw std::invalid_argument("missing warning overlay field");
}
double number(const V& v) {
  if (const auto* n = std::get_if<double>(&v.data)) return *n;
  if (const auto* n = std::get_if<std::int64_t>(&v.data)) return double(*n);
  throw std::invalid_argument("warning overlay geometry must be numeric");
}
std::int64_t integer(const V& v) {
  const auto* n = std::get_if<std::int64_t>(&v.data);
  check(n, "warning overlay value must be an integer");
  return *n;
}
Rect rectangle(const V& v) {
  const auto& o = object(v, {"x", "y", "width", "height"});
  return {{number(field(o, "x")), number(field(o, "y"))},
          {number(field(o, "width")), number(field(o, "height"))}};
}
bool intersects(Rect a, Rect b) {
  return a.origin.x < b.origin.x + b.size.width && b.origin.x < a.origin.x + a.size.width &&
         a.origin.y < b.origin.y + b.size.height && b.origin.y < a.origin.y + a.size.height;
}
}  // namespace
void validate_warning_overlay_configuration(const WarningOverlayConfiguration& c) {
  check(std::isfinite(c.canvas.width) && std::isfinite(c.canvas.height) && c.canvas.width >= 128 &&
            c.canvas.height >= 96 && c.canvas.width <= 4096 && c.canvas.height <= 4096,
        "invalid warning overlay canvas");
  check(c.maximum_age.count() >= 50 && c.maximum_age.count() <= 2000,
        "invalid warning overlay freshness limit");
  check(c.indicators.size() <= indicator_definitions.size(), "too many warning overlay indicators");
  const auto geometry = [&](Rect r, double min_width, double min_height) {
    check(std::isfinite(r.origin.x) && std::isfinite(r.origin.y) && std::isfinite(r.size.width) &&
              std::isfinite(r.size.height) && r.origin.x >= 0 && r.origin.y >= 0 &&
              r.size.width >= min_width && r.size.height >= min_height && r.size.width <= 180 &&
              r.size.height <= 96 && r.origin.x + r.size.width <= c.canvas.width &&
              r.origin.y + r.size.height <= c.canvas.height,
          "warning overlay placement exceeds canvas or tile limits");
  };
  geometry(c.simulation_badge, 64, 20);
  std::bitset<24> used;
  for (std::size_t i = 0; i < c.indicators.size(); ++i) {
    const auto& p = c.indicators[i];
    const auto id = static_cast<std::size_t>(p.signal);
    check(id < used.size() && !used[id], "unknown or duplicate warning overlay signal");
    used.set(id);
    geometry(p.frame, 64, 48);
    check(!intersects(p.frame, c.simulation_badge), "warning overlay overlaps simulation badge");
    for (std::size_t j = 0; j < i; ++j)
      check(!intersects(p.frame, c.indicators[j].frame), "warning overlay slots overlap");
  }
}
WarningOverlayConfiguration decode_warning_overlay_configuration(std::string_view text) {
  const auto json = vehicle::detail::Json(text).parse();
  const auto& o = object(
      json, {"schemaVersion", "enabled", "canvas", "maximumAgeMs", "simulationBadge", "indicators"},
      {"hideDuringStartup"});
  check(integer(field(o, "schemaVersion")) == 1, "unsupported warning overlay schema");
  const auto* enabled = std::get_if<bool>(&field(o, "enabled").data);
  check(enabled, "invalid warning overlay enabled flag");
  const auto& canvas = object(field(o, "canvas"), {"width", "height"});
  WarningOverlayConfiguration result;
  result.enabled = *enabled;
  for (const auto& [key, value] : o)
    if (std::get<std::string>(key.data) == "hideDuringStartup") {
      const auto* hide = std::get_if<bool>(&value.data);
      check(hide, "invalid warning overlay startup flag");
      result.hide_during_startup = *hide;
    }
  result.canvas = {number(field(canvas, "width")), number(field(canvas, "height"))};
  result.maximum_age = std::chrono::milliseconds(integer(field(o, "maximumAgeMs")));
  result.simulation_badge = rectangle(field(o, "simulationBadge"));
  const auto* items = std::get_if<opack::Array>(&field(o, "indicators").data);
  check(items && items->size() <= 24, "invalid warning overlay indicator list");
  for (const auto& item : *items) {
    const auto& entry = object(item, {"signal", "frame"});
    const auto* name = std::get_if<std::string>(&field(entry, "signal").data);
    const auto* definition = name ? find_vehicle_indicator(*name) : nullptr;
    check(definition, "unknown warning overlay signal");
    result.indicators.push_back({definition->id, rectangle(field(entry, "frame"))});
  }
  validate_warning_overlay_configuration(result);
  return result;
}
}  // namespace headunit::theme::graphics
