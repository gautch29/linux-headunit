// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Test-only production cache/codec/receiver/JSON path. All dates and readings
// are explicit simulation, not a publication endpoint or physical evidence.
#include <headunit/prephone_vehicle_summary.hpp>
#include <headunit/vehicle_observation_receiver.hpp>
#include <iostream>
#include <stdexcept>

using namespace headunit;
using namespace std::chrono_literals;
namespace {
Clock::time_point at(std::int64_t ms) {
  return Clock::time_point{std::chrono::milliseconds(ms)};
}
VehicleSample transfer(VehicleObservationCache& source, VehicleObservationReceiver& receiver,
                       std::uint64_t sequence, Clock::time_point published,
                       Clock::time_point received) {
  const auto bytes = encode_vehicle_observation_datagram(
      {source.sample(published), sequence, VehicleObservationWireVersion::hvo4});
  const auto packet = parse_vehicle_observation_datagram(bytes, received);
  if (!packet) throw std::runtime_error("Production codec rejected fixture snapshot");
  receiver.accept(*packet, received);
  return receiver.sample(received);
}
void emit(const VehicleSample& sample, VehicleSummaryAdmission admission, Clock::time_point now) {
  std::cout << prephone_vehicle_summary_json(sample, admission, now) << '\n';
}
}  // namespace
int main() {
  try {
    VehicleAgePolicy ages;
    ages.readings[2] = 100ms;
    ages.readings[7] = 2s;
    VehicleObservationCache source(ages);
    VehicleObservationReceiver receiver;
    source.begin_generation(7, at(1000), at(1000));
    emit(transfer(source, receiver, 1, at(1000), at(1000)), {7}, at(1000));
    const std::array readings{
        VehicleObservation{"fuel_percent", 0.0, at(1010), VehicleObservationOrigin::simulation},
        VehicleObservation{"ambient_c", -12.5, at(1015), VehicleObservationOrigin::simulation},
        VehicleObservation{"range_km", 123.25, at(1017), VehicleObservationOrigin::calculated},
        VehicleObservation{"oil_c", std::monostate{}, at(1018), VehicleObservationOrigin::measured,
                           VehicleAvailability::unavailable}};
    source.accept(7, 1, readings, at(1020));
    const auto captured = transfer(source, receiver, 2, at(1020), at(1020));
    emit(captured, {7}, at(1020));
    emit(receiver.sample(at(1110)), {7}, at(1110));
    emit(receiver.sample(at(1111)), {7, true}, at(1111));
    source.begin_generation(8, at(1112), at(1112));
    emit(transfer(source, receiver, 1, at(1112), at(1112)), {8}, at(1112));
    const std::array recovery{
        VehicleObservation{"fuel_percent", 5.0, at(1113), VehicleObservationOrigin::simulation}};
    source.accept(8, 1, recovery, at(1113));
    emit(transfer(source, receiver, 2, at(1113), at(1113)), {8}, at(1113));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
