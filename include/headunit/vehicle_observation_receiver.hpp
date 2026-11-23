// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle_observation.hpp>
#include <headunit/vehicle_observation_wire.hpp>

namespace headunit {
// A complete-snapshot receiver in one host steady-clock domain. accept() and
// sample() belong to the constructing thread; invalid packets do not advance
// any clock, sequence or acquisition watermark. First accepted wire version is
// fixed for this receiver lifetime, including across source restarts. The mode
// catalog is also immutable, including absence and identifier order. Current-trip
// identity/reset/counter history survives missing observations and source restarts.
class VehicleObservationReceiver final : public VehicleSource {
 public:
  VehicleObservationReceiver();
  VehicleObservationReceiver(const VehicleObservationReceiver&) = delete;
  VehicleObservationReceiver& operator=(const VehicleObservationReceiver&) = delete;
  void accept(const VehicleObservationPacket&, Clock::time_point now);
  // Filters each field at its original acquisition date. The returned source
  // publication date is unchanged; before initialization this is legacy empty.
  VehicleSample sample(Clock::time_point now) override;
  bool initialized() const noexcept;

 private:
  struct Watermark {
    VehicleObservationValue value;
    std::optional<Clock::time_point> acquired_at;
    VehicleObservationOrigin origin = VehicleObservationOrigin::measured;
    // Preserve explicit available/unavailable history across withdrawal. An
    // initially stale observation implies available history but has no value;
    // initially disconnected history remains unknown until an explicit state.
    VehicleAvailability observation_state = VehicleAvailability::unavailable;
    bool withdrawn = false;
  };
  void check_owner_and_time(Clock::time_point) const;
  const std::thread::id owner_;
  VehicleSample sample_;
  std::array<Watermark, vehicle_observation_capacity> watermarks_{};
  std::optional<Clock::time_point> last_host_time_;
  std::uint64_t sequence_ = 0;
  std::optional<VehicleObservationWireVersion> version_;
  TripProgress trip_progress_;
};
}  // namespace headunit
