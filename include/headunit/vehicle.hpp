// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/transmission.hpp>
#include <headunit/drive_mode.hpp>
#include <headunit/trip.hpp>
#include <headunit/heading.hpp>
#include <headunit/sport_observation.hpp>

#include <chrono>
#include <array>
#include <string_view>
#include <span>
#include <cstddef>
#include <bitset>
#include <optional>
#include <cstdint>

namespace headunit {

using Clock = std::chrono::steady_clock;

enum class IndicatorState { off, on, flashing };
enum class Indicator {
  left_turn,
  right_turn,
  hazard,
  high_beam,
  low_beam,
  position_lights,
  front_fog,
  rear_fog,
  check_engine,
  oil_pressure,
  battery_charge,
  brake_fault,
  parking_brake,
  abs,
  stability_control,
  traction_control,
  airbag,
  seat_belt,
  door_open,
  low_fuel,
  coolant_warning,
  washer_low,
  tyre_pressure,
  glow_plug,
  count
};
enum class IndicatorColor { green, blue, amber, red };
enum class VehicleObservationOrigin { measured, calculated, simulation };
enum class VehicleAvailability { available, unavailable, stale, disconnected };
// Acquisition dates are in this host's qualified steady-clock domain. An
// unobserved signal has no acquisition date; zero/off are still real values.
struct VehicleSignalTiming {
  std::optional<Clock::time_point> acquired_at{};
  Clock::duration maximum_age = std::chrono::milliseconds(500);
  VehicleObservationOrigin origin = VehicleObservationOrigin::measured;
  VehicleAvailability availability = VehicleAvailability::unavailable;
};
struct VehicleObservationTiming {
  std::uint64_t generation = 0;
  Clock::time_point started_at{};
  std::array<VehicleSignalTiming, 10> readings{};
  std::array<VehicleSignalTiming, static_cast<std::size_t>(Indicator::count)> indicators{};
  VehicleSignalTiming transmission{};
  VehicleSignalTiming drive_mode{};
  VehicleSignalTiming trip{};
  VehicleSignalTiming heading{};
  VehicleSignalTiming acceleration{};
  VehicleSignalTiming gps_position{};
};
struct IndicatorDefinition {
  Indicator id;
  std::string_view key, label;
  IndicatorColor color;
};
inline constexpr std::array<IndicatorDefinition, 24> indicator_definitions{
    {IndicatorDefinition{Indicator::left_turn, "left_turn", "LEFT", IndicatorColor::green},
     {Indicator::right_turn, "right_turn", "RIGHT", IndicatorColor::green},
     {Indicator::hazard, "hazard", "HAZARD", IndicatorColor::amber},
     {Indicator::high_beam, "high_beam", "HIGH", IndicatorColor::blue},
     {Indicator::low_beam, "low_beam", "DIPPED", IndicatorColor::green},
     {Indicator::position_lights, "position_lights", "LIGHTS", IndicatorColor::green},
     {Indicator::front_fog, "front_fog", "FOG F", IndicatorColor::green},
     {Indicator::rear_fog, "rear_fog", "FOG R", IndicatorColor::amber},
     {Indicator::check_engine, "check_engine", "ENGINE", IndicatorColor::amber},
     {Indicator::oil_pressure, "oil_pressure", "OIL", IndicatorColor::red},
     {Indicator::battery_charge, "battery_charge", "CHARGE", IndicatorColor::red},
     {Indicator::brake_fault, "brake_fault", "BRAKE", IndicatorColor::red},
     {Indicator::parking_brake, "parking_brake", "PARK", IndicatorColor::red},
     {Indicator::abs, "abs", "ABS", IndicatorColor::amber},
     {Indicator::stability_control, "stability_control", "ESC", IndicatorColor::amber},
     {Indicator::traction_control, "traction_control", "TRACTION", IndicatorColor::amber},
     {Indicator::airbag, "airbag", "AIRBAG", IndicatorColor::red},
     {Indicator::seat_belt, "seat_belt", "BELT", IndicatorColor::red},
     {Indicator::door_open, "door_open", "DOOR", IndicatorColor::red},
     {Indicator::low_fuel, "low_fuel", "FUEL", IndicatorColor::amber},
     {Indicator::coolant_warning, "coolant_warning", "TEMP", IndicatorColor::red},
     {Indicator::washer_low, "washer_low", "WASHER", IndicatorColor::amber},
     {Indicator::tyre_pressure, "tyre_pressure", "TYRES", IndicatorColor::amber},
     {Indicator::glow_plug, "glow_plug", "GLOW", IndicatorColor::amber}}};

// A missing value is displayed as unavailable. In particular, zero speed must
// never be inferred from a disconnected sensor.
struct VehicleSample {
  std::optional<double> speed_kmh;
  std::optional<double> engine_rpm;
  Clock::time_point sampled_at{};
  std::optional<double> fuel_percent{};
  std::optional<double> coolant_c{};
  std::optional<double> oil_c{};
  std::optional<double> oil_pressure_kpa{};
  std::optional<double> battery_voltage{};
  std::optional<double> ambient_c{};
  std::optional<double> range_km{};
  std::optional<double> odometer_km{};
  bool simulated = false;
  std::array<std::optional<IndicatorState>, static_cast<std::size_t>(Indicator::count)>
      indicators{};
  std::optional<IndicatorState>& indicator(Indicator id) {
    return indicators.at(static_cast<std::size_t>(id));
  }
  const std::optional<IndicatorState>& indicator(Indicator id) const {
    return indicators.at(static_cast<std::size_t>(id));
  }
  // Appended to preserve existing aggregate initialization. Legacy transports
  // do not carry this metadata and must not silently discard it when encoding.
  std::optional<VehicleObservationTiming> observation_timing{};
  std::optional<TransmissionState> transmission{};
  // Static vehicle capability and independently acquired effective mode.
  std::optional<DriveModeCatalog> drive_mode_catalog{};
  std::optional<DriveModeIdentifier> drive_mode{};
  std::optional<TripState> trip{};
  std::optional<HeadingState> heading{};
  std::optional<AccelerationState> acceleration{};
  std::optional<GpsPositionState> gps_position{};
};

struct ReadingDefinition {
  std::string_view key, label, unit;
  double minimum, maximum;
  std::optional<double> VehicleSample::* member;
};
inline constexpr std::array<ReadingDefinition, 10> reading_definitions{
    {ReadingDefinition{"speed_kmh", "SPEED", "KM/H", 0, 300, &VehicleSample::speed_kmh},
     {"engine_rpm", "ENGINE", "RPM", 0, 9000, &VehicleSample::engine_rpm},
     {"fuel_percent", "FUEL", "%", 0, 100, &VehicleSample::fuel_percent},
     {"coolant_c", "COOLANT", "C", -50, 180, &VehicleSample::coolant_c},
     {"oil_c", "OIL TEMP", "C", -50, 200, &VehicleSample::oil_c},
     {"oil_pressure_kpa", "OIL PRESS", "KPA", 0, 1500, &VehicleSample::oil_pressure_kpa},
     {"battery_voltage", "BATTERY", "V", 0, 60, &VehicleSample::battery_voltage},
     {"ambient_c", "OUTSIDE", "C", -60, 80, &VehicleSample::ambient_c},
     {"range_km", "RANGE", "KM", 0, 3000, &VehicleSample::range_km},
     {"odometer_km", "ODOMETER", "KM", 0, 9999999, &VehicleSample::odometer_km}}};
// Bounds are transport plausibility limits, not warning/calibration thresholds.
bool valid_vehicle_sample(const VehicleSample&);
const ReadingDefinition* find_vehicle_reading(std::string_view);
const IndicatorDefinition* find_vehicle_indicator(std::string_view);

class VehicleSource {
 public:
  virtual ~VehicleSource() = default;
  virtual VehicleSample sample(Clock::time_point now) = 0;
};

// A test source only. It has no route to a CAN bus, GPIO, or HVAC actuator.
class SimulatedVehicle final : public VehicleSource {
 public:
  explicit SimulatedVehicle(Clock::time_point started_at);
  VehicleSample sample(Clock::time_point now) override;

 private:
  Clock::time_point started_at_;
};

// With timing metadata: true when any valid available signal remains within
// its own age policy; maximum_age applies only to legacy aggregate samples.
bool fresh(const VehicleSample& value, Clock::time_point now, Clock::duration maximum_age);
// Legacy samples keep inclusive aggregate expiry. Metadata-bearing samples
// expire each signal at age >= its immutable policy without renewing dates.
// Invalid metadata clears all values, and cannot become a valid fresh sample.
VehicleSample current_vehicle_sample(
    const VehicleSample&, Clock::time_point,
    Clock::duration legacy_maximum_age = std::chrono::milliseconds(500));

// Shared native/Ultra telltale policy. Unknown is distinct from a known off
// signal; stale or invalid snapshots clear both sets rather than latching lights.
struct VehicleIndicatorSnapshot {
  std::bitset<static_cast<std::size_t>(Indicator::count)> known, visible;
  bool fresh_data = false, simulated = false;
};
// Shared 1.5 Hz / 50% duty fallback cadence. Explicit observed on/off
// values bypass this clock; it never changes acquisition time.
bool indicator_flash_visible(Clock::time_point) noexcept;
VehicleIndicatorSnapshot vehicle_indicator_snapshot(
    const VehicleSample&, Clock::time_point,
    Clock::duration maximum_age = std::chrono::milliseconds(500));

}  // namespace headunit
