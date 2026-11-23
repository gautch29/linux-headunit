// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_indicator_publication.hpp>
#include <headunit/vehicle_observation.hpp>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, std::source_location at = std::source_location::current()) {
  if (!value)
    throw std::runtime_error("indicator publication assertion failed at " +
                             std::to_string(at.line()));
}
struct Source final : headunit::VehicleSource {
  unsigned calls = 0;
  headunit::VehicleSample value;
  headunit::VehicleSample sample(headunit::Clock::time_point) override {
    ++calls;
    return value;
  }
};
}  // namespace
int main(int argc, char** argv) try {
  using namespace headunit;
  // Explicit supplied portable fixture dates. They do not qualify a Mac IPC
  // clock mapping or represent physical measured observations.
  const auto now = Clock::time_point(std::chrono::seconds(1000));
  VehicleAgePolicy ages;
  ages.indicators.fill(std::chrono::seconds(10));
  ages.indicators[0] = std::chrono::milliseconds(500);
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, now, now);
  const std::array batch{
      VehicleObservation{"left_turn", IndicatorState::flashing, now,
                         VehicleObservationOrigin::simulation},
      VehicleObservation{"right_turn", IndicatorState::off, now,
                         VehicleObservationOrigin::simulation},
      VehicleObservation{"high_beam", IndicatorState::on, now,
                         VehicleObservationOrigin::simulation},
      VehicleObservation{"low_beam", IndicatorState::off, now,
                         VehicleObservationOrigin::simulation},
      VehicleObservation{"front_fog", std::monostate{}, now, VehicleObservationOrigin::simulation,
                         VehicleAvailability::unavailable}};
  cache.accept(7, 1, batch, now);
  const auto original = cache.sample(now);
  PrephoneNonce nonce{};
  nonce.fill(std::byte{'n'});
  std::vector<PrephoneIndicatorPacket> packets;
  const auto encode = [&](const VehicleSample& sample, Clock::time_point copied,
                          bool retired = false,
                          PrephoneIndicatorClockDomain domain =
                              PrephoneIndicatorClockDomain::host_steady) {
    const auto generation = sample.observation_timing ? sample.observation_timing->generation : 0;
    const auto model = prephone_indicator_summary(sample, {{generation, retired}, domain}, copied);
    packets.push_back(prephone_indicator_packet(model, nonce, packets.size() + 1, copied));
    return model;
  };
  const auto first = encode(original, now);
  require(first.rows[0].value == IndicatorState::flashing &&
          first.rows[1].value == IndicatorState::off);
  require(!first.rows[5].value &&
          first.rows[5].availability == VehicleSummaryAvailability::missing);
  const auto second = encode(original, now + std::chrono::milliseconds(200));
  require(second.rows[0].value == IndicatorState::flashing);
  require(second.rows[0].acquired_at == first.rows[0].acquired_at);
  require(second.source_sampled_at == first.source_sampled_at);
  const auto expired = encode(original, now + std::chrono::milliseconds(500));
  require(!expired.rows[0].value && expired.rows[1].value == IndicatorState::off);
  cache.withdraw(now + std::chrono::milliseconds(600));
  const auto withdrawn = cache.sample(now + std::chrono::milliseconds(600));
  encode(withdrawn, withdrawn.sampled_at);
  const auto successor = now + std::chrono::milliseconds(700);
  cache.begin_generation(8, successor, successor);
  encode(cache.sample(successor), successor);
  SimulatedVehicle legacy(now);
  const auto undated = encode(legacy.sample(successor), successor);
  // The accepted pure gate requires explicit source admission. With no dated
  // generation it stays awaiting; the native carrier labels legacy diagnostics.
  require(undated.state == VehicleSummaryState::awaiting_source);
  encode({}, successor, false, PrephoneIndicatorClockDomain::unqualified);
  // A terminal projection preserves source identity and dates, nulls all lamps.
  const auto retired = encode(original, successor, true);
  require(retired.state == VehicleSummaryState::disconnected && !retired.rows[1].value);
  require(packets[0].size() == 872 && packets[0][88] == std::byte{1});
  require(packets[0][90] == std::byte{24} && packets[0][104 + 2] == std::byte{2});
  require(packets[0][136 + 2] == std::byte{0} && packets[0][96] == std::byte{1});
  auto source = std::make_unique<Source>();
  auto* owner = source.get();
  owner->value = original;
  PublishingVehicle forwarding(std::move(source), "/not-present/prephone.sock", true, true);
  const auto forwarded = forwarding.sample(now);
  require(owner->calls == 1 && forwarded.sampled_at == original.sampled_at);
  require(forwarded.observation_timing->indicators[0].acquired_at == now);
  // Exact legacy byte compatibility: every byte of the known eight-row packet
  // is independently specified, including padding and absent versus zero value.
  const auto old = prephone_packet(prephone_vehicle_summary({}, {0, false}, now), nonce, 9, now);
  PrephonePacket expected{};
  const std::string magic = "HUPN001";
  for (std::size_t i = 0; i < magic.size(); ++i)
    expected[i] = std::byte(magic[i]);
  for (std::size_t i = 0; i < nonce.size(); ++i)
    expected[8 + i] = nonce[i];
  expected[40] = std::byte{9};
  const auto copy_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  for (unsigned i = 0; i < 8; ++i)
    expected[48 + i] = std::byte(copy_ns >> (i * 8));
  expected[56] = std::byte{1};
  for (std::size_t i = 0; i < 8; ++i) {
    expected[88 + i * 32] = std::byte{1};
    expected[89 + i * 32] = std::byte{255};
  }
  require(old == expected);
  if (argc == 2 && std::string_view(argv[1]) == "--catalogue") {
    std::cout << prephone_indicator_summary_json(
        original, {{7, false}, PrephoneIndicatorClockDomain::host_steady}, now);
  } else if (argc == 2) {
    for (const auto& packet : packets)
      std::cout.write(reinterpret_cast<const char*>(packet.data()), packet.size());
  } else {
    std::cout
        << "Fixed indicator wire; original TTL/canonical state/legacy bytes; one source sample\n";
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
