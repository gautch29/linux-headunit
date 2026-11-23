// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_observation_receiver.hpp>
#include <stdexcept>
#include <type_traits>

namespace headunit {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
bool withdrawn(VehicleAvailability availability) {
  return availability == VehicleAvailability::stale ||
         availability == VehicleAvailability::disconnected;
}
}  // namespace
VehicleObservationReceiver::VehicleObservationReceiver() : owner_(std::this_thread::get_id()) {
  static_assert(reading_definitions.size() + indicator_definitions.size() ==
                legacy_vehicle_observation_capacity);
  static_assert(vehicle_observation_capacity == 40);
}
bool VehicleObservationReceiver::initialized() const noexcept {
  return sample_.observation_timing.has_value();
}
void VehicleObservationReceiver::check_owner_and_time(Clock::time_point now) const {
  check(owner_ == std::this_thread::get_id(),
        "vehicle observation receiver used by another owner thread");
  check(now > Clock::time_point{} && (!last_host_time_ || now >= *last_host_time_),
        "vehicle host receipt or poll time reversed or is not positive");
}
void VehicleObservationReceiver::accept(const VehicleObservationPacket& packet,
                                        Clock::time_point now) {
  check_owner_and_time(now);
  check(valid_vehicle_observation_packet(packet, now),
        "invalid complete vehicle observation snapshot");
  check(!version_ || *version_ == packet.version, "vehicle observation wire version changed");
  const auto& incoming = *packet.sample.observation_timing;
  bool new_generation = !initialized();
  if (initialized()) {
    check(packet.sample.drive_mode_catalog == sample_.drive_mode_catalog,
          "vehicle drive-mode catalog changed");
    const auto& previous = *sample_.observation_timing;
    check(incoming.generation >= previous.generation, "vehicle source generation reversed");
    new_generation = incoming.generation > previous.generation;
    if (new_generation) {
      // A delayed restart is ordered against the accepted source publication,
      // never against a later local receipt or sample() call.
      check(incoming.started_at >= sample_.sampled_at,
            "vehicle source restart precedes accepted publication");
    } else {
      check(incoming.started_at == previous.started_at, "vehicle source generation start changed");
      check(packet.sequence > sequence_, "vehicle observation sequence replay");
      check(packet.sample.sampled_at >= sample_.sampled_at, "vehicle source publication reversed");
    }
    for (std::size_t i = 0; i < vehicle_observation_capacity; ++i)
      check(vehicle_signal_timing(incoming, i).maximum_age ==
                vehicle_signal_timing(previous, i).maximum_age,
            "vehicle signal age policy changed");
  }
  auto trip_progress = trip_progress_;
  if (new_generation || !packet.sample.trip)
    check(withdraw_trip_progress(trip_progress), "invalid lifetime current-trip history");
  if (packet.sample.trip)
    check(update_trip_progress(trip_progress, *packet.sample.trip, *incoming.trip.acquired_at),
          "vehicle current-trip identity/reset/counter history reversed");
  if (!current_vehicle_sample(packet.sample, now).trip)
    check(withdraw_trip_progress(trip_progress), "invalid lifetime current-trip history");
  auto watermarks = new_generation ? decltype(watermarks_){} : watermarks_;
  for (std::size_t i = 0; i < watermarks.size(); ++i) {
    auto& previous = watermarks[i];
    const auto& next = vehicle_signal_timing(incoming, i);
    const auto next_value = vehicle_observation_value(packet.sample, i);
    if (previous.acquired_at) {
      check(next.acquired_at && *next.acquired_at >= *previous.acquired_at,
            "vehicle acquisition watermark reversed or dropped");
      if (next.acquired_at == previous.acquired_at) {
        check(next.origin == previous.origin, "conflicting equal-time vehicle observation origin");
        switch (next.availability) {
          case VehicleAvailability::available:
            check(previous.observation_state == VehicleAvailability::available &&
                      !previous.withdrawn && next_value == previous.value,
                  "conflicting or revived equal-time vehicle observation");
            break;
          case VehicleAvailability::unavailable:
            check(previous.observation_state == VehicleAvailability::unavailable ||
                      previous.observation_state == VehicleAvailability::disconnected,
                  "conflicting equal-time explicit vehicle availability");
            previous.observation_state = VehicleAvailability::unavailable;
            break;
          case VehicleAvailability::stale:
            check(previous.observation_state != VehicleAvailability::unavailable,
                  "unavailable observation became stale without acquisition");
            if (previous.observation_state == VehicleAvailability::disconnected)
              previous.observation_state = VehicleAvailability::stale;
            previous.withdrawn = true;
            break;
          case VehicleAvailability::disconnected:
            previous.withdrawn = true;
            break;
        }
        continue;
      }
    }
    previous = {next_value, next.acquired_at, next.origin, next.availability,
                withdrawn(next.availability)};
  }
  // The complete model and fixed watermarks cannot allocate during commit.
  static_assert(std::is_nothrow_copy_assignable_v<VehicleSample>);
  static_assert(std::is_nothrow_move_assignable_v<decltype(watermarks_)>);
  static_assert(std::is_nothrow_copy_assignable_v<TripProgress>);
  sample_ = packet.sample;
  watermarks_ = std::move(watermarks);
  sequence_ = packet.sequence;
  last_host_time_ = now;
  version_ = packet.version;
  trip_progress_ = trip_progress;
}
VehicleSample VehicleObservationReceiver::sample(Clock::time_point now) {
  check_owner_and_time(now);
  auto current = initialized() ? current_vehicle_sample(sample_, now) : VehicleSample{};
  auto trip_progress = trip_progress_;
  if (!current.trip)
    check(withdraw_trip_progress(trip_progress), "invalid lifetime current-trip history");
  trip_progress_ = trip_progress;
  last_host_time_ = now;
  return current;
}
}  // namespace headunit
