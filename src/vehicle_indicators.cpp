// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle.hpp>

namespace headunit {
bool indicator_flash_visible(Clock::time_point now) noexcept {
  const auto ticks =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  // Three cycles in two seconds: 1.5 Hz, 50% duty, without millisecond drift.
  return ticks >= 0 && ((ticks % 2000000000LL) * 3) % 2000000000LL < 1000000000LL;
}
VehicleIndicatorSnapshot vehicle_indicator_snapshot(const VehicleSample& sample,
                                                    Clock::time_point now,
                                                    Clock::duration maximum_age) {
  VehicleIndicatorSnapshot result;
  result.simulated = sample.simulated;
  result.fresh_data = fresh(sample, now, maximum_age) && valid_vehicle_sample(sample);
  if (!result.fresh_data) return result;
  const auto current = current_vehicle_sample(sample, now, maximum_age);
  const bool flash_phase = indicator_flash_visible(now);
  for (std::size_t i = 0; i < sample.indicators.size(); ++i) {
    const auto state = current.indicators[i];
    if (!state) continue;
    result.known.set(i);
    if (*state == IndicatorState::on || (*state == IndicatorState::flashing && flash_phase))
      result.visible.set(i);
  }
  return result;
}
}  // namespace headunit
