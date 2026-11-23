// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <cairo.h>

namespace headunit {
struct IndicatorFrame {
  int x, y, width, height;
  constexpr bool contains(unsigned px, unsigned py) const {
    return int(px) >= x && int(px) < x + width && int(py) >= y && int(py) < y + height;
  }
};
// Shared placement for native instruments and the Ultra overlay. Warning
// lamps retain their bottom slots; the two direction arrows sit at the top.
namespace indicator_layout {
inline constexpr int width = 1920, height = 720;
// The centre stays clear for the signed cluster's odometer, including an
// all-lamps test. Warning slots retain the original bottom-row spacing.
inline constexpr IndicatorFrame row{48, 648, 1824, 56};
inline constexpr IndicatorFrame frame(Indicator signal) {
  const auto id = static_cast<int>(signal);
  if (id < 0 || id >= static_cast<int>(Indicator::count)) return {};
  if (signal == Indicator::left_turn) return {816, 16, 64, 56};
  if (signal == Indicator::right_turn) return {1040, 16, 64, 56};
  const int slot = id - 1;
  return {row.x + slot * 68 + (slot >= 12 ? 196 : 0), row.y, 64, row.height};
}
}  // namespace indicator_layout

// Display policy only: fresh active hazards pair the direction arrows, with
// observed lamp phases taking priority over the shared flashing fallback. No
// separate hazard lamp. Sensor readings and their dates are unchanged.
struct ClusterIndicatorSnapshot : VehicleIndicatorSnapshot {
  // Effective display availability, separate from the individual sensor known
  // bits: a fresh active hazard supplies both arrows without inventing readings.
  std::bitset<2> unavailable_turns;
  std::array<std::optional<IndicatorState>, 2> turn_states;
};
ClusterIndicatorSnapshot cluster_indicator_snapshot(
    const VehicleSample&, Clock::time_point,
    Clock::duration maximum_age = std::chrono::milliseconds(500));

// Pure display-owner animation. Does not write observations or renew dates.
struct TurnSignalDisplay {
  std::array<double, 2> intensity{};
  std::bitset<2> unavailable;
  bool animating = false;
};
class TurnSignalAnimation {
 public:
  TurnSignalDisplay update(const VehicleSample&, Clock::time_point,
                           Clock::duration maximum_age = std::chrono::milliseconds(500));
  void reset() noexcept;

 private:
  struct Lamp {
    bool known = false;
    bool target = false;
    double from = 0;
    Clock::time_point changed_at{};
    bool flashing = false;
  };
  std::array<Lamp, 2> lamps_{};
  Clock::time_point last_update_{};
  std::uint64_t generation_ = 0;
};
// Dashed neutral outline uses the same arrow path, never a filled lamp.
void draw_turn_signal_unavailable(cairo_t*, Indicator, double x, double y, double width,
                                  double height);
// Full-symbol lamp easing, with a low-alpha bloom. No sequential segments.
void draw_turn_signal_lamp(cairo_t*, Indicator, double x, double y, double width, double height,
                           double intensity = 1);

// Original dashboard symbol geometry shared by native and signed-scene overlays.
// Fits a 64 x 40 logical glyph into the supplied frame, preserving aspect ratio,
// current color and caller state. No descriptive label or background is drawn;
// P and ABS are part of the conventional brake symbols. Input state/freshness
// remains owned by vehicle_indicator_snapshot, not by this drawing function.
void draw_indicator_symbol(cairo_t*, Indicator, double x, double y, double width, double height);
// Shared colour and inset as well as artwork. Preserves the caller's state.
void draw_indicator_telltale(cairo_t*, const IndicatorDefinition&, double x, double y, double width,
                             double height);
}  // namespace headunit
