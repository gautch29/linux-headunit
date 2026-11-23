// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_publication.hpp>
#include <headunit/vehicle_observation.hpp>
#include <stdexcept>
#include <iostream>

namespace {
void require(bool value) {
  if (!value) throw std::runtime_error("publication assertion failed");
}
struct Source final : headunit::VehicleSource {
  int calls = 0;
  bool fail = false;
  headunit::VehicleSample value;
  headunit::VehicleSample sample(headunit::Clock::time_point) override {
    ++calls;
    if (fail) throw std::runtime_error("source failure");
    return value;
  }
};
}  // namespace
int main(int argc, char**) {
  using namespace headunit;
  const auto now = Clock::now();
  VehicleObservationCache cache;
  cache.begin_generation(17, now, now);
  const std::array batch{
      VehicleObservation{"fuel_percent", 0.0, now, VehicleObservationOrigin::simulation},
      VehicleObservation{"ambient_c", -7.0, now, VehicleObservationOrigin::simulation}};
  cache.accept(17, 1, batch, now);
  auto source = std::make_unique<Source>();
  auto* owner = source.get();
  owner->value = cache.sample(now);
  const auto original = owner->value;
  PublishingVehicle forwarded(std::move(source), "/not-present/prephone.sock");
  const auto copy = forwarded.sample(now);
  require(owner->calls == 1 && copy.fuel_percent == original.fuel_percent &&
          copy.ambient_c == original.ambient_c);
  require(copy.observation_timing->generation == 17 && copy.sampled_at == original.sampled_at);
  require(copy.observation_timing->readings[2].acquired_at == now);
  PrephoneNonce nonce{};
  nonce[0] = std::byte{42};
  const auto packet =
      prephone_packet(prephone_vehicle_summary(copy, {17, false}, now), nonce, 9, now);
  require(packet.size() == 344 && packet[8] == std::byte{42} && packet[40] == std::byte{9});
  require(packet[64] == std::byte{17} && packet[88] == std::byte{0} && packet[89] == std::byte{2});
  if (argc == 2) {
    std::cout.write(reinterpret_cast<const char*>(packet.data()), packet.size());
    return 0;
  }
  owner->fail = true;
  bool caught = false;
  try {
    forwarded.sample(now);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  require(caught && owner->calls == 2);
  std::cout << "Same source sampled once; original dates/value/generation unchanged; optional "
               "failure and source retirement preserved\n";
}
