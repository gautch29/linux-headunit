// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <thread>
#include <variant>

namespace headunit {
inline constexpr std::size_t legacy_vehicle_observation_capacity = 34;
inline constexpr std::size_t transmission_observation_index = legacy_vehicle_observation_capacity;
inline constexpr std::size_t transmission_vehicle_observation_capacity = 35;
inline constexpr std::size_t drive_mode_observation_index =
    transmission_vehicle_observation_capacity;
inline constexpr std::size_t drive_mode_vehicle_observation_capacity = 36;
inline constexpr std::size_t trip_observation_index = drive_mode_vehicle_observation_capacity;
inline constexpr std::size_t heading_observation_index = trip_observation_index + 1;
inline constexpr std::size_t trip_heading_vehicle_observation_capacity = 38;
inline constexpr std::size_t acceleration_observation_index = 38;
inline constexpr std::size_t gps_position_observation_index = 39;
inline constexpr std::size_t vehicle_observation_capacity = 40;
inline constexpr auto maximum_vehicle_signal_age = std::chrono::hours(24);
using VehicleObservationValue =
    std::variant<std::monostate, double, IndicatorState, TransmissionState, DriveModeIdentifier,
                 TripState, HeadingState, AccelerationState, GpsPositionState>;
VehicleSignalTiming& vehicle_signal_timing(VehicleObservationTiming&, std::size_t);
const VehicleSignalTiming& vehicle_signal_timing(const VehicleObservationTiming&, std::size_t);
VehicleObservationValue vehicle_observation_value(const VehicleSample&, std::size_t);
struct VehicleObservation {
  std::string_view key;
  VehicleObservationValue value;
  Clock::time_point acquired_at;
  VehicleObservationOrigin origin = VehicleObservationOrigin::measured;
  // Producers report available/unavailable only. Cache policy derives stale
  // and withdraw() derives disconnected, preserving acquisition watermarks.
  VehicleAvailability availability = VehicleAvailability::available;
};
struct VehicleAgePolicy {
  std::array<Clock::duration, 10> readings;
  std::array<Clock::duration, static_cast<std::size_t>(Indicator::count)> indicators;
  Clock::duration transmission = std::chrono::milliseconds(500);
  Clock::duration drive_mode = std::chrono::milliseconds(500);
  Clock::duration trip = std::chrono::milliseconds(500);
  Clock::duration heading = std::chrono::milliseconds(500);
  Clock::duration acceleration = std::chrono::milliseconds(500);
  Clock::duration gps_position = std::chrono::milliseconds(500);
  VehicleAgePolicy();
};
// One owner thread, one host monotonic domain, no hardware I/O or wire codec.
// accept() is an atomic delta batch: omitted fields are unchanged, unlike HUV2.
// Ages are copied once and immutable. All calls reject clock reversal.
class VehicleObservationCache final : public VehicleSource {
 public:
  explicit VehicleObservationCache(VehicleAgePolicy = {}, std::optional<DriveModeCatalog> = {});
  VehicleObservationCache(const VehicleObservationCache&) = delete;
  VehicleObservationCache& operator=(const VehicleObservationCache&) = delete;
  void begin_generation(std::uint64_t generation, Clock::time_point started_at,
                        Clock::time_point now);
  void accept(std::uint64_t generation, std::uint64_t sequence, std::span<const VehicleObservation>,
              Clock::time_point now);
  void withdraw(Clock::time_point now);
  VehicleSample sample(Clock::time_point now) override;

 private:
  struct Watermark {
    VehicleObservationValue value;
    std::optional<Clock::time_point> acquired_at;
    VehicleObservationOrigin origin = VehicleObservationOrigin::measured;
    VehicleAvailability availability = VehicleAvailability::unavailable;
  };
  void check_owner_and_time(Clock::time_point) const;
  void expire(Clock::time_point);
  const VehicleAgePolicy ages_;
  const std::optional<DriveModeCatalog> drive_mode_catalog_;
  const std::thread::id owner_;
  VehicleSample sample_;
  std::array<Watermark, vehicle_observation_capacity> watermarks_{};
  TripProgress trip_progress_{};
  std::optional<Clock::time_point> last_host_time_;
  std::uint64_t sequence_ = 0;
};
}  // namespace headunit
