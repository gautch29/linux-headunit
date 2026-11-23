// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/local_vehicle_profile.hpp>
#include <headunit/vehicle_observation.hpp>

namespace headunit::theme::vehicle {
// Display-owner adapter. Uses local semantic bindings, never phone VDP type IDs.
// The original acquisition times are preserved; repeated reads cannot renew
// freshness. Source format, age policies and optional ordered drive-mode
// catalogue are immutable for the adapter lifetime, including across restarts.
// Trip identity/reset/counter history also survives absence and source restarts.
class GaugeInput {
 public:
  GaugeInput(const LocalProfile&, std::unique_ptr<headunit::VehicleSource>);
  void pump(headunit::Clock::time_point);
  // Last accepted source snapshot, including original acquisition metadata. Owner-thread
  // consumers share this sample without reading/draining the source twice.
  const VehicleSample& sample() const noexcept { return sample_; }

 private:
  struct ObservationWatermark {
    std::optional<headunit::Clock::time_point> acquired_at;
    VehicleObservationValue original_value{};
    std::optional<bool> original_available;
    VehicleObservationOrigin origin = VehicleObservationOrigin::measured;
    bool withdrawn = false;
  };
  void pump_observed(const VehicleSample&, const VehicleSample&, headunit::Clock::time_point);
  std::shared_ptr<ReadOnlyService> service_;
  std::unique_ptr<headunit::VehicleSource> input_;
  std::array<std::int64_t, 5> ids_;
  std::vector<VehicleInputBinding> additional_;
  std::optional<std::uint64_t> last_sample_;
  std::optional<headunit::Clock::time_point> last_now_;
  std::optional<bool> observed_format_;
  std::array<ObservationWatermark, vehicle_observation_capacity> observations_{};
  std::map<std::int64_t, std::uint64_t> delivered_ms_;
  bool mode_catalog_selected_ = false;
  std::optional<DriveModeCatalog> mode_catalog_{};
  TripProgress trip_progress_{};
  VehicleSample sample_;
};
}  // namespace headunit::theme::vehicle
