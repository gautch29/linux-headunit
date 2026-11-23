// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_observation.hpp>
#include <headunit/vehicle_observation_wire.hpp>
#include <headunit/vehicle_observation_receiver.hpp>
#include <headunit/vehicle_wire.hpp>
#include <headunit/sport_simulated_vehicle.hpp>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace {
using namespace headunit;
using namespace std::chrono_literals;
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
template <class Action>
void rejects(Action action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid sport observation admitted");
}
}  // namespace
int main() try {
  const auto base = Clock::time_point{1s};
  VehicleAgePolicy ages;
  ages.acceleration = 100ms;
  ages.gps_position = 200ms;
  VehicleObservationCache source(ages);
  source.begin_generation(1, base, base);
  const std::array initial{
      VehicleObservation{"speed_kmh", 0., base, VehicleObservationOrigin::simulation},
      VehicleObservation{"acceleration", AccelerationState{0, -9.80665}, base,
                         VehicleObservationOrigin::simulation},
      VehicleObservation{"gps_position", GpsPositionState{48, 2, 1}, base,
                         VehicleObservationOrigin::simulation}};
  source.accept(1, 1, initial, base);
  VehicleObservationPacket packet{source.sample(base), 1, VehicleObservationWireVersion::hvo6};
  check(valid_vehicle_sample(packet.sample), "initial sample invalid");
  check(valid_vehicle_observation_packet(packet, base), "initial sport packet invalid");
  const auto bytes = encode_vehicle_observation_datagram(packet);
  const auto decoded = parse_vehicle_observation_datagram(bytes, base);
  check(decoded && decoded->sample.acceleration == packet.sample.acceleration &&
            decoded->sample.gps_position == packet.sample.gps_position &&
            decoded->sample.observation_timing->acceleration.acquired_at == base &&
            decoded->sample.simulated,
        "HVO6 metadata round trip failed");
  for (const auto version :
       {VehicleObservationWireVersion::hvo1, VehicleObservationWireVersion::hvo2,
        VehicleObservationWireVersion::hvo3, VehicleObservationWireVersion::hvo4}) {
    auto older = packet;
    older.version = version;
    rejects([&] { encode_vehicle_observation_datagram(older); });
  }
  rejects([&] { encode_vehicle_datagram(packet.sample); });
  VehicleObservationReceiver receiver;
  check(valid_vehicle_observation_packet(*decoded, base),
        "decoded packet invalid after round trip");
  receiver.accept(*decoded, base);
  auto conflict = *decoded;
  conflict.sequence = 2;
  conflict.sample.acceleration->lateral_mps2 = 0;
  rejects([&] { receiver.accept(conflict, base); });
  auto policy = *decoded;
  policy.sequence = 2;
  policy.sample.observation_timing->gps_position.maximum_age = 300ms;
  rejects([&] { receiver.accept(policy, base); });
  auto newer = *decoded;
  newer.sequence = 2;
  newer.sample.sampled_at = base + 10ms;
  check(valid_vehicle_observation_packet(newer, base + 10ms), "newer sport heartbeat invalid");
  receiver.accept(newer, base + 10ms);
  check(!receiver.sample(base + 100ms).acceleration && receiver.sample(base + 100ms).gps_position,
        "independent acceleration expiry failed");
  check(!receiver.sample(base + 200ms).gps_position, "GPS exact expiry failed");
  source.withdraw(base + 200ms);
  const auto withdrawn = source.sample(base + 200ms);
  check(!withdrawn.acceleration && !withdrawn.gps_position &&
            withdrawn.observation_timing->acceleration.acquired_at == base &&
            withdrawn.observation_timing->gps_position.availability ==
                VehicleAvailability::disconnected,
        "withdrawal lost original sport timing");
  source.accept(1, 2, initial, base + 200ms);
  check(!source.sample(base + 200ms).acceleration, "equal-date sport revival admitted");
  const std::array invalid{
      VehicleObservation{"gps_position", GpsPositionState{91, 2, 1}, base + 201ms}};
  rejects([&] { source.accept(1, 3, invalid, base + 201ms); });
  const std::array recovery{
      VehicleObservation{"gps_position", GpsPositionState{0, 0, 1}, base + 201ms}};
  source.accept(1, 3, recovery, base + 201ms);
  check(source.sample(base + 201ms).gps_position->latitude_deg == 0,
        "known GPS zero became absence");
  check(!parse_acceleration_state_key("nan/0") && !parse_acceleration_state_key("0/101") &&
            !parse_gps_position_state_key("48/2/0") && !parse_gps_position_state_key("48/181/1") &&
            !parse_gps_position_state_key("48/2/1/4"),
        "invalid compound parser value admitted");
  auto malformed = bytes;
  const auto start = malformed.find("acceleration=");
  malformed.replace(start, std::string("acceleration").size(), "gps_position");
  check(!parse_vehicle_observation_datagram(malformed, base), "duplicate HVO6 signal admitted");
  const auto simulated = sport_simulation_sample(base, base + 7s);
  check(
      simulated.speed_kmh == 100 && simulated.acceleration && simulated.gps_position &&
          simulated.observation_timing->gps_position.origin == VehicleObservationOrigin::simulation,
      "sport simulation fixture failed");
  std::cout << "sport observations: typed HVO6, legacy refusal, atomic rejection, expiry and "
               "retirement pass\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
