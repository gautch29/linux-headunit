// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/camera.hpp>
#include <array>
#include <variant>

namespace headunit::theme::vehicle {
enum class ViewProvenance { physical, simulated };
struct LaneView {
  // Metres relative to vehicle centre. Missing boundaries/engagement stay unknown.
  std::optional<double> left_boundary_m, right_boundary_m;
  std::optional<bool> assistance_engaged;
};
struct SeatView {
  std::optional<bool> occupied, belt_fastened;
};
struct CabinView {
  // Front left/right, rear left/centre/right; doors front left/right, rear left/right.
  std::array<SeatView, 5> seats;
  std::array<std::optional<bool>, 4> doors_open;
};
struct ViewObservation {
  std::uint64_t generation = 0, sequence = 0, acquired_ns = 0;
  ViewProvenance provenance = ViewProvenance::physical;
  std::variant<LaneView, CabinView> content;
};
// Original local artwork. Does not copy a vehicle brand's assets or imply ADAS
// sensing. Simulated observations always carry an in-pixel BENCH badge.
graphics::Image render_vehicle_view(const ViewObservation&);
struct ViewStatistics {
  std::uint64_t accepted = 0, superseded = 0, rendered = 0, published = 0, stale = 0,
                backpressure = 0;
};
// One pending dated observation; one CPU render worker; existing bounded queue.
// Queue must outlive this owner. submit does no rendering/driver/VM/GPU work.
// Redraw never renews acquisition age. Stop immediately closes this queue
// generation; acknowledgement follows worker cleanup.
class VehicleViewProvider {
 public:
  VehicleViewProvider(camera::Queue&, bool cabin, unsigned maximum_age_ms = 150);
  ~VehicleViewProvider();
  VehicleViewProvider(const VehicleViewProvider&) = delete;
  VehicleViewProvider& operator=(const VehicleViewProvider&) = delete;
  bool submit(ViewObservation, std::uint64_t observed_ns);
  void request_stop() noexcept;
  bool stopped() const noexcept;
  ViewStatistics statistics() const;
  std::optional<std::string> failure() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::vehicle
