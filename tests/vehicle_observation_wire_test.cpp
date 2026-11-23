// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_observation_wire.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/vehicle_wire.hpp>
#include <array>
#include <iostream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace headunit;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F&& action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "inconsistent observation snapshot encoded");
}
Clock::time_point at(std::int64_t ms) {
  return Clock::time_point{std::chrono::milliseconds(ms)};
}
VehicleObservationPacket fixture() {
  VehicleAgePolicy ages;
  ages.readings.fill(2s);
  ages.indicators.fill(2s);
  ages.readings[0] = 100ms;
  ages.readings[1] = 250ms;
  ages.readings[2] = 1s;
  VehicleObservationCache cache(ages);
  cache.begin_generation(9, at(1000), at(1000));
  const std::array observations{
      VehicleObservation{"speed_kmh", 0., at(1950)},
      VehicleObservation{"engine_rpm", 3400., at(1700)},
      VehicleObservation{"fuel_percent", 55., at(1000), VehicleObservationOrigin::calculated},
      VehicleObservation{"coolant_c", 90., at(1900), VehicleObservationOrigin::simulation},
      VehicleObservation{"oil_c", std::monostate{}, at(1600), VehicleObservationOrigin::measured,
                         VehicleAvailability::unavailable},
      VehicleObservation{"range_km", 429.35123456789, at(1950),
                         VehicleObservationOrigin::calculated},
      VehicleObservation{"high_beam", IndicatorState::off, at(1800)},
      VehicleObservation{"low_beam", IndicatorState::on, at(1950)},
      VehicleObservation{"left_turn", IndicatorState::flashing, at(1990)}};
  cache.accept(9, 3, observations, at(2000));
  VehicleObservationPacket packet{cache.sample(at(2000)), 3};
  packet.sample.observation_timing->readings[4].availability = VehicleAvailability::disconnected;
  return packet;
}
std::vector<std::string> tokens(std::string_view text) {
  std::vector<std::string> result;
  while (!text.empty()) {
    const auto next = text.find(' ');
    result.emplace_back(text.substr(0, next));
    if (next == text.npos) break;
    text.remove_prefix(next + 1);
  }
  return result;
}
std::string join(const std::vector<std::string>& parts) {
  std::string result;
  for (const auto& part : parts) {
    if (!result.empty()) result += ' ';
    result += part;
  }
  return result;
}
std::string field(std::string text, std::string_view key, std::string_view value) {
  const std::string prefix = " " + std::string(key) + "=";
  const auto begin = text.find(prefix);
  check(begin != text.npos, "test field missing");
  const auto end = text.find(' ', begin + 1);
  text.replace(begin + prefix.size(),
               (end == text.npos ? text.size() : end) - begin - prefix.size(), value);
  return text;
}
void roundtrip() {
  const auto packet = fixture();
  check(valid_vehicle_observation_packet(packet, at(2100)), "fixture is invalid");
  const auto encoded = encode_vehicle_observation_datagram(packet);
  check(encoded.size() <= maximum_vehicle_observation_datagram && tokens(encoded).size() == 39,
        "complete bounded shape changed");
  const auto decoded = parse_vehicle_observation_datagram(encoded, at(2100));
  check(decoded && decoded->sequence == 3 && decoded->sample.sampled_at == at(2000) &&
            decoded->sample.simulated,
        "packet sequence, publication or simulation provenance was lost");
  check(encode_vehicle_observation_datagram(*decoded) == encoded,
        "complete snapshot did not roundtrip exactly");
  check(decoded->sample.speed_kmh == 0 &&
            decoded->sample.indicator(Indicator::high_beam) == IndicatorState::off &&
            decoded->sample.indicator(Indicator::low_beam) == IndicatorState::on &&
            decoded->sample.indicator(Indicator::left_turn) == IndicatorState::flashing,
        "zero/off/on/flash changed");
  const auto& timing = *decoded->sample.observation_timing;
  check(timing.generation == 9 && timing.started_at == at(1000) &&
            timing.readings[0].acquired_at == at(1950) && timing.readings[0].maximum_age == 100ms &&
            timing.readings[3].origin == VehicleObservationOrigin::simulation &&
            timing.readings[8].origin == VehicleObservationOrigin::calculated,
        "individual dates, policy or origins changed");
  check(!decoded->sample.engine_rpm &&
            timing.readings[1].availability == VehicleAvailability::stale &&
            !decoded->sample.fuel_percent &&
            timing.readings[2].availability == VehicleAvailability::stale,
        "expired or exact-boundary stale fields were recreated");
  check(timing.readings[4].availability == VehicleAvailability::disconnected &&
            timing.readings[4].acquired_at == at(1600) && !decoded->sample.oil_c &&
            !timing.readings[9].acquired_at && !decoded->sample.odometer_km,
        "loss/unknown acquisition changed");
  check(parse_vehicle_observation_datagram(encoded, at(30000)).has_value(),
        "receipt delay silently expired/refilled original snapshot");
  check(parse_vehicle_observation_datagram(encoded, at(2100)).has_value(),
        "stateless parser inferred replay order");
  auto reordered = tokens(encoded);
  std::swap(reordered[5], reordered.back());
  check(parse_vehicle_observation_datagram(join(reordered), at(2100)).has_value(),
        "complete field order must be semantic");
  auto widest = packet;
  widest.sequence = std::numeric_limits<std::uint64_t>::max();
  widest.sample.observation_timing->generation = std::numeric_limits<std::uint64_t>::max();
  check(parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(widest), at(2100))
            .has_value(),
        "valid uint64 identity boundary rejected");
  // A syntactically valid long zero stays bounded; overflow is checked before
  // parsing, even when every remaining semantic field is otherwise valid.
  const auto extra = maximum_vehicle_observation_datagram - encoded.size();
  const auto exact_bound =
      field(encoded, "speed_kmh", std::string(extra + 1, '0') + ",1950000000,100000000,m,a");
  check(exact_bound.size() == maximum_vehicle_observation_datagram &&
            parse_vehicle_observation_datagram(exact_bound, at(2100)).has_value(),
        "valid exact datagram size boundary rejected");
  check(!parse_vehicle_observation_datagram(
            field(exact_bound, "speed_kmh",
                  std::string(extra + 2, '0') + ",1950000000,100000000,m,a"),
            at(2100)),
        "valid-looking oversized packet bypassed bound");
  // Host representability is checked without an overflowing duration_cast.
  if constexpr (std::ratio_equal_v<Clock::duration::period, std::nano>) {
    VehicleObservationPacket endpoint;
    endpoint.sequence = 1;
    endpoint.sample.observation_timing.emplace();
    endpoint.sample.sampled_at =
        Clock::time_point{Clock::duration{std::numeric_limits<Clock::duration::rep>::max()}};
    auto& maximum = *endpoint.sample.observation_timing;
    maximum.generation = 1;
    maximum.started_at = Clock::time_point{1ns};
    endpoint.sample.speed_kmh = 0;
    maximum.readings[0].acquired_at = endpoint.sample.sampled_at;
    maximum.readings[0].maximum_age = 1ns;
    maximum.readings[0].availability = VehicleAvailability::available;
    const auto maximum_wire = encode_vehicle_observation_datagram(endpoint);
    check(parse_vehicle_observation_datagram(maximum_wire, endpoint.sample.sampled_at).has_value(),
          "maximum positive host date rejected");
  }
}
void transmission_roundtrip_and_version_shape() {
  auto packet = fixture();
  packet.version = VehicleObservationWireVersion::hvo2;
  auto& signal = packet.sample.observation_timing->transmission;
  signal.maximum_age = 750ms;
  signal.acquired_at = at(1900) + 1ns;
  signal.origin = VehicleObservationOrigin::calculated;
  signal.availability = VehicleAvailability::available;
  const std::array states{
      TransmissionState{GearboxType::manual, TransmissionSelector::manual,
                        EngagedGear{EngagedGearKind::forward, 1}},
      TransmissionState{GearboxType::automatic, TransmissionSelector::drive,
                        EngagedGear{EngagedGearKind::forward, 32}},
      TransmissionState{GearboxType::automated_manual, TransmissionSelector::reverse,
                        EngagedGear{EngagedGearKind::reverse}},
      TransmissionState{GearboxType::continuously_variable, TransmissionSelector::neutral,
                        EngagedGear{EngagedGearKind::neutral}},
      TransmissionState{std::nullopt, TransmissionSelector::park, std::nullopt},
      TransmissionState{std::nullopt, TransmissionSelector::low, std::nullopt},
      TransmissionState{std::nullopt, TransmissionSelector::sport, std::nullopt},
      TransmissionState{GearboxType::automatic, std::nullopt, std::nullopt},
      TransmissionState{std::nullopt, std::nullopt, EngagedGear{EngagedGearKind::forward, 3}}};
  for (const auto& state : states) {
    packet.sample.transmission = state;
    const auto encoded = encode_vehicle_observation_datagram(packet);
    const auto decoded = parse_vehicle_observation_datagram(encoded, at(2100));
    check(encoded.starts_with("HVO2 ") && tokens(encoded).size() == 40 && decoded &&
              decoded->version == VehicleObservationWireVersion::hvo2 &&
              decoded->sample.transmission == state &&
              decoded->sample.observation_timing->transmission.acquired_at == at(1900) + 1ns &&
              decoded->sample.observation_timing->transmission.maximum_age == 750ms &&
              decoded->sample.observation_timing->transmission.origin ==
                  VehicleObservationOrigin::calculated &&
              encode_vehicle_observation_datagram(*decoded) == encoded,
          "HVO2 lost compound state, original date or version");
    auto ordered = tokens(encoded);
    std::swap(ordered[5], ordered.back());
    check(parse_vehicle_observation_datagram(join(ordered), at(2100)).has_value(),
          "transmission record required a fixed position");
    auto down = packet;
    down.version = VehicleObservationWireVersion::hvo1;
    check(!valid_vehicle_observation_packet(down, at(2100)),
          "HVO1 silently lost transmission state");
    rejects([&] { encode_vehicle_observation_datagram(down); });
    auto legacy = packet.sample;
    legacy.observation_timing.reset();
    rejects([&] { encode_vehicle_datagram(legacy); });
  }
  packet.sample.transmission.reset();
  signal.availability = VehicleAvailability::disconnected;
  const auto lost =
      parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(packet), at(2100));
  check(lost && !lost->sample.transmission &&
            lost->sample.observation_timing->transmission.acquired_at == at(1900) + 1ns &&
            lost->sample.observation_timing->transmission.availability ==
                VehicleAvailability::disconnected,
        "HVO2 unknown transmission erased its acquisition watermark");
  signal.acquired_at = at(1250);
  signal.availability = VehicleAvailability::stale;
  check(parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(packet), at(2100))
            .has_value(),
        "HVO2 exact transmission expiry rejected");
  // HVO1 keeps its original 34-record schema and cannot encode even unknown
  // transmission metadata that changes origin, availability, age or acquisition.
  const auto legacy = fixture();
  check(legacy.version == VehicleObservationWireVersion::hvo1, "legacy aggregate default changed");
  const auto original = encode_vehicle_observation_datagram(legacy);
  const auto old = parse_vehicle_observation_datagram(original, at(3000));
  check(
      old && old->version == VehicleObservationWireVersion::hvo1 && !old->sample.transmission &&
          !old->sample.observation_timing->transmission.acquired_at &&
          old->sample.observation_timing->transmission.maximum_age == 500ms &&
          old->sample.observation_timing->transmission.availability ==
              VehicleAvailability::unavailable &&
          old->sample.observation_timing->transmission.origin == VehicleObservationOrigin::measured,
      "HVO1 manufactured transmission acquisition or policy");
  const std::array<std::function<void(VehicleSignalTiming&)>, 4> losses{
      [](auto& t) { t.acquired_at = at(1900); }, [](auto& t) { t.maximum_age = 501ms; },
      [](auto& t) { t.origin = VehicleObservationOrigin::calculated; },
      [](auto& t) {
        t.availability = VehicleAvailability::disconnected;
      }};
  for (const auto& change : losses) {
    auto bad = legacy;
    change(bad.sample.observation_timing->transmission);
    check(!valid_vehicle_observation_packet(bad, at(2100)),
          "HVO1 accepted lossy transmission metadata");
    rejects([&] { encode_vehicle_observation_datagram(bad); });
  }
  packet = legacy;
  packet.version = VehicleObservationWireVersion::hvo2;
  const auto v2 = encode_vehicle_observation_datagram(packet);
  check(v2.ends_with(" transmission=-,-,500000000,m,u"),
        "HVO2 did not explicitly encode unknown transmission");
  auto incomplete = tokens(v2);
  incomplete.pop_back();
  check(!parse_vehicle_observation_datagram(join(incomplete), at(2100)),
        "HVO2 omitted transmission");
  auto v1extra = v2;
  v1extra.replace(0, 4, "HVO1");
  check(!parse_vehicle_observation_datagram(v1extra, at(2100)), "HVO1 accepted HVO2's extra field");
  auto renamed = original;
  renamed.replace(0, 4, "HVO2");
  check(!parse_vehicle_observation_datagram(renamed, at(2100)),
        "HVO2 accepted HVO1's incomplete shape");
}
void transmission_malformed() {
  auto packet = fixture();
  packet.version = VehicleObservationWireVersion::hvo2;
  packet.sample.transmission =
      TransmissionState{GearboxType::automatic, TransmissionSelector::drive,
                        EngagedGear{EngagedGearKind::forward, 3}};
  packet.sample.observation_timing->transmission = {
      at(1900), 500ms, VehicleObservationOrigin::measured, VehicleAvailability::available};
  const auto encoded = encode_vehicle_observation_datagram(packet);
  auto bad = [&](std::string text) {
    check(!parse_vehicle_observation_datagram(text, at(2100)),
          "malformed or incoherent HVO2 transmission accepted");
  };
  for (const auto state :
       {"-/-/-", "automatic/drive", "automatic/drive/3/extra", "automatic//3", "bogus/drive/3",
        "automatic/bogus/3", "automatic/drive/bogus", "automatic/drive/0", "automatic/drive/33",
        "automatic/drive/+3", "automatic/drive/03", "automatic/drive/3.0", "automatic/drive/256",
        "automatic/park/3", "automatic/neutral/reverse", "automatic/reverse/3",
        "automatic/sport/reverse", "automatic/low/reverse"})
    bad(field(encoded, "transmission", std::string(state) + ",1900000000,500000000,m,a"));
  for (const auto value :
       {"-,1900000000,500000000,m,a", "automatic/drive/3,-,500000000,m,a",
        "automatic/drive/3,2000000001,500000000,m,a", "automatic/drive/3,999999999,500000000,m,a",
        "automatic/drive/3,1900000000,500000000,m,u", "automatic/drive/3,1900000000,500000000,m,d",
        "automatic/drive/3,1500000000,500000000,m,a", "-,1500000001,500000000,m,s",
        "-,-,500000000,m,s", "-,-,0,m,u", "-,-,86400000000001,m,u"})
    bad(field(encoded, "transmission", value));
  auto parts = tokens(encoded);
  parts.back() = parts[5];
  bad(join(parts));
  parts = tokens(encoded);
  parts[5] = parts.back();
  bad(join(parts));
  bad(encoded + " transmission=-,-,500000000,m,u");
  auto invalid = packet;
  invalid.version = static_cast<VehicleObservationWireVersion>(99);
  check(!valid_vehicle_observation_packet(invalid, at(2100)), "invalid wire version accepted");
  rejects([&] { encode_vehicle_observation_datagram(invalid); });
  invalid = packet;
  invalid.sample.transmission->selector = TransmissionSelector::park;
  check(!valid_vehicle_observation_packet(invalid, at(2100)),
        "incoherent decoded transmission accepted");
  rejects([&] { encode_vehicle_observation_datagram(invalid); });
  // V2 retains the existing exact datagram boundary and overflow guard.
  packet.sample.speed_kmh = 0;
  const auto base = encode_vehicle_observation_datagram(packet);
  const auto extra = maximum_vehicle_observation_datagram - base.size();
  const auto boundary =
      field(base, "speed_kmh", std::string(extra + 1, '0') + ",1950000000,100000000,m,a");
  check(boundary.size() == maximum_vehicle_observation_datagram &&
            parse_vehicle_observation_datagram(boundary, at(2100)),
        "HVO2 exact packet boundary rejected");
  bad(boundary + 'x');
}
DriveModeIdentifier mode(std::string_view text) {
  const auto value = parse_drive_mode_identifier(text);
  check(value.has_value(), "invalid test mode identifier");
  return *value;
}
DriveModeCatalog catalog(std::string_view text) {
  const auto value = parse_drive_mode_catalog_key(text);
  check(value.has_value(), "invalid test mode catalog");
  return *value;
}
VehicleObservationPacket mode_fixture() {
  auto packet = fixture();
  packet.version = VehicleObservationWireVersion::hvo3;
  packet.sample.drive_mode_catalog = catalog("comfort,sport,track-plus");
  packet.sample.drive_mode = mode("sport");
  packet.sample.observation_timing->drive_mode = {
      at(1900) + 7ns, 750ms, VehicleObservationOrigin::simulation, VehicleAvailability::available};
  return packet;
}
std::string header_catalog(std::string text, std::string_view value) {
  const std::string prefix = " drive_modes=";
  const auto begin = text.find(prefix);
  check(begin != text.npos, "catalog header missing");
  const auto end = text.find(' ', begin + 1);
  check(end != text.npos, "header has no observations");
  text.replace(begin + prefix.size(), end - begin - prefix.size(), value);
  return text;
}
void drive_mode_roundtrip_and_version_shape() {
  const auto packet = mode_fixture();
  const auto encoded = encode_vehicle_observation_datagram(packet);
  const auto decoded = parse_vehicle_observation_datagram(encoded, at(2100));
  check(
      encoded.starts_with("HVO3 9 3 1000000000 2000000000 drive_modes=comfort,sport,track-plus ") &&
          tokens(encoded).size() == 42 && decoded &&
          decoded->version == VehicleObservationWireVersion::hvo3 &&
          decoded->sample.drive_mode_catalog == packet.sample.drive_mode_catalog &&
          decoded->sample.drive_mode == packet.sample.drive_mode &&
          decoded->sample.observation_timing->drive_mode.acquired_at == at(1900) + 7ns &&
          decoded->sample.observation_timing->drive_mode.maximum_age == 750ms &&
          decoded->sample.observation_timing->drive_mode.origin ==
              VehicleObservationOrigin::simulation &&
          decoded->sample.simulated && encode_vehicle_observation_datagram(*decoded) == encoded,
      "HVO3 lost ordered catalog, active mode, original date or version");
  auto order = tokens(encoded);
  std::swap(order[6], order.back());
  check(parse_vehicle_observation_datagram(join(order), at(2100)).has_value(),
        "mode observation required a fixed position");
  for (const auto key : {"comfort", "track-plus"}) {
    auto changed = packet;
    changed.sample.drive_mode = mode(key);
    const auto result =
        parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(changed), at(2100));
    check(result && result->sample.drive_mode == changed.sample.drive_mode,
          "supported source mode was inferred or replaced");
  }
  // Catalog membership is independent of which fields happen to be available.
  auto absent = packet;
  absent.sample.drive_mode.reset();
  absent.sample.observation_timing->drive_mode.availability = VehicleAvailability::disconnected;
  auto result =
      parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(absent), at(2100));
  check(result && result->sample.drive_mode_catalog == packet.sample.drive_mode_catalog &&
            !result->sample.drive_mode &&
            result->sample.observation_timing->drive_mode.acquired_at == at(1900) + 7ns,
        "mode loss erased static catalog or acquisition history");
  absent.sample.observation_timing->drive_mode.acquired_at = at(1250);
  absent.sample.observation_timing->drive_mode.availability = VehicleAvailability::stale;
  check(parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(absent), at(2100))
            .has_value(),
        "HVO3 exact stale mode boundary rejected");
  for (const auto version :
       {VehicleObservationWireVersion::hvo1, VehicleObservationWireVersion::hvo2}) {
    auto lost = packet;
    lost.version = version;
    check(!valid_vehicle_observation_packet(lost, at(2100)), "older HVO discarded mode state");
    rejects([&] { encode_vehicle_observation_datagram(lost); });
    lost = fixture();
    lost.version = version;
    lost.sample.drive_mode_catalog = packet.sample.drive_mode_catalog;
    rejects([&] { encode_vehicle_observation_datagram(lost); });
    const std::array<std::function<void(VehicleSignalTiming&)>, 4> timing_losses{
        [](auto& t) { t.acquired_at = at(1900); }, [](auto& t) { t.maximum_age = 501ms; },
        [](auto& t) { t.origin = VehicleObservationOrigin::calculated; },
        [](auto& t) {
          t.availability = VehicleAvailability::disconnected;
        }};
    for (const auto& change : timing_losses) {
      lost = fixture();
      lost.version = version;
      change(lost.sample.observation_timing->drive_mode);
      check(!valid_vehicle_observation_packet(lost, at(2100)),
            "older HVO discarded unknown-mode timing");
      rejects([&] { encode_vehicle_observation_datagram(lost); });
    }
  }
  auto legacy = packet.sample;
  legacy.observation_timing.reset();
  rejects([&] { encode_vehicle_datagram(legacy); });
  legacy.drive_mode.reset();
  rejects([&] { encode_vehicle_datagram(legacy); });
  for (const auto version :
       {VehicleObservationWireVersion::hvo1, VehicleObservationWireVersion::hvo2}) {
    auto old = fixture();
    old.version = version;
    const auto wire = encode_vehicle_observation_datagram(old);
    result = parse_vehicle_observation_datagram(wire, at(3000));
    check(result &&
              tokens(wire).size() == (version == VehicleObservationWireVersion::hvo1 ? 39 : 40) &&
              !result->sample.drive_mode && !result->sample.drive_mode_catalog &&
              !result->sample.observation_timing->drive_mode.acquired_at &&
              result->sample.observation_timing->drive_mode.maximum_age == 500ms &&
              result->sample.observation_timing->drive_mode.origin ==
                  VehicleObservationOrigin::measured &&
              result->sample.observation_timing->drive_mode.availability ==
                  VehicleAvailability::unavailable,
          "older HVO shape changed or manufactured mode freshness");
  }
  auto unknown = fixture();
  unknown.version = VehicleObservationWireVersion::hvo3;
  const auto unknown_wire = encode_vehicle_observation_datagram(unknown);
  check(tokens(unknown_wire)[5] == "drive_modes=-" &&
            unknown_wire.ends_with(" drive_mode=-,-,500000000,m,u"),
        "HVO3 missing catalog or mode was omitted instead of explicit unknown");
  result = parse_vehicle_observation_datagram(unknown_wire, at(2100));
  check(result && !result->sample.drive_mode_catalog && !result->sample.drive_mode,
        "unknown mode silently selected default catalog");
  // Provenance comes from the mode acquisition even without another simulated
  // signal; losing its value must not erase historical simulation identity.
  VehicleObservationPacket solo;
  solo.sequence = 1;
  solo.version = VehicleObservationWireVersion::hvo3;
  solo.sample.sampled_at = at(2000);
  solo.sample.observation_timing.emplace();
  solo.sample.observation_timing->generation = 1;
  solo.sample.observation_timing->started_at = at(1000);
  solo.sample.drive_mode_catalog = catalog("comfort");
  solo.sample.drive_mode = mode("comfort");
  solo.sample.simulated = true;
  solo.sample.observation_timing->drive_mode = {
      at(1999) + 1ns, 500ms, VehicleObservationOrigin::simulation, VehicleAvailability::available};
  result = parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(solo), at(2100));
  check(result && result->sample.simulated && result->sample.drive_mode == mode("comfort"),
        "mode-only simulation acquisition did not determine provenance");
  solo.sample.drive_mode.reset();
  solo.sample.observation_timing->drive_mode.acquired_at = at(1500);
  solo.sample.observation_timing->drive_mode.availability = VehicleAvailability::stale;
  result = parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(solo), at(2100));
  check(result && result->sample.simulated && !result->sample.drive_mode &&
            result->sample.observation_timing->drive_mode.acquired_at == at(1500),
        "stale mode lost simulation acquisition provenance");
  solo.sample.observation_timing->drive_mode.acquired_at.reset();
  solo.sample.observation_timing->drive_mode.availability = VehicleAvailability::disconnected;
  solo.sample.simulated = false;
  result = parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(solo), at(2100));
  check(result && !result->sample.simulated &&
            !result->sample.observation_timing->drive_mode.acquired_at,
        "unobserved simulated-mode origin fabricated acquisition/provenance");
  // Maximal catalog length and count have a bounded exact representation.
  std::string full;
  for (unsigned i = 0; i < 32; ++i) {
    if (!full.empty()) full += ',';
    const auto prefix = "mode" + std::to_string(i);
    full += prefix + std::string(64 - prefix.size(), 'x');
  }
  auto widest = unknown;
  widest.sample.drive_mode_catalog = catalog(full);
  widest.sample.drive_mode = widest.sample.drive_mode_catalog->identifiers[31];
  widest.sample.observation_timing->drive_mode = {
      at(1950), 500ms, VehicleObservationOrigin::measured, VehicleAvailability::available};
  result =
      parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(widest), at(2100));
  check(result && result->sample.drive_mode_catalog == widest.sample.drive_mode_catalog &&
            result->sample.drive_mode == widest.sample.drive_mode,
        "maximum 32-entry/64-character mode catalog did not roundtrip");
}
void drive_mode_malformed_and_bounds() {
  const auto good = mode_fixture();
  const auto encoded = encode_vehicle_observation_datagram(good);
  auto bad = [&](std::string text) {
    check(!parse_vehicle_observation_datagram(text, at(2100)),
          "malformed HVO3 mode/catalog accepted");
  };
  for (const auto value :
       {"", "comfort,", "comfort,,sport", ",comfort", "comfort,comfort", "1sport", "comfort/track",
        "comfort=sport", "comfort;track", "-", "comfort"})
    bad(header_catalog(encoded, value));  // Last two lack the advertised active sport.
  bad(header_catalog(encoded, "comfort,スポーツ"));
  bad(header_catalog(encoded, std::string(65, 'a')));
  std::string too_many;
  for (unsigned i = 0; i < 33; ++i) {
    if (i) too_many += ',';
    too_many += "mode" + std::to_string(i);
  }
  bad(header_catalog(encoded, too_many));
  auto parts = tokens(encoded);
  parts.erase(parts.begin() + 5);
  bad(join(parts));
  parts = tokens(encoded);
  parts[5] = "catalog=comfort,sport";
  bad(join(parts));
  parts = tokens(encoded);
  parts.insert(parts.begin() + 6, parts[5]);
  bad(join(parts));
  parts = tokens(encoded);
  parts.pop_back();
  bad(join(parts));
  parts = tokens(encoded);
  parts.back() = parts[6];
  bad(join(parts));
  parts = tokens(encoded);
  parts[6] = parts.back();
  bad(join(parts));
  for (const auto value :
       {"unknown,1900000007,750000000,s,a", "Sport,1900000007,750000000,s,a",
        "-,1900000007,750000000,s,a", "sport,-,750000000,s,a", "sport,2000000001,750000000,s,a",
        "sport,999999999,750000000,s,a", "sport,1900000007,750000000,s,u",
        "sport,1900000007,750000000,s,d", "sport,1250000000,750000000,s,a",
        "-,1250000001,750000000,s,s", "-,-,750000000,s,s", "-,-,0,m,u", "-,-,86400000000001,m,u"})
    bad(field(encoded, "drive_mode", value));
  bad(encoded + " drive_mode=-,-,500000000,m,u");
  for (const auto version : {"HVO1", "HVO2", "HVO4"}) {
    auto changed = encoded;
    changed.replace(0, 4, version);
    bad(changed);
  }
  auto invalid = good;
  invalid.sample.drive_mode = mode("unsupported");
  check(!valid_vehicle_observation_packet(invalid, at(2100)), "unsupported decoded mode accepted");
  rejects([&] { encode_vehicle_observation_datagram(invalid); });
  invalid = good;
  invalid.sample.drive_mode_catalog->count = 33;
  rejects([&] { encode_vehicle_observation_datagram(invalid); });
  invalid = good;
  invalid.sample.drive_mode_catalog->identifiers[1] =
      invalid.sample.drive_mode_catalog->identifiers[0];
  rejects([&] { encode_vehicle_observation_datagram(invalid); });
  const auto extra = maximum_vehicle_observation_datagram - encoded.size();
  const auto boundary =
      field(encoded, "speed_kmh", std::string(extra + 1, '0') + ",1950000000,100000000,m,a");
  check(boundary.size() == maximum_vehicle_observation_datagram &&
            parse_vehicle_observation_datagram(boundary, at(2100)),
        "HVO3 exact packet boundary rejected");
  bad(boundary + 'x');
}
TripState trip_state(std::string_view text) {
  const auto value = parse_trip_state_key(text);
  check(value.has_value(), "invalid test trip state");
  return *value;
}
HeadingState qualified_heading() {
  return HeadingState{359.999, HeadingReference::true_north, HeadingSource::vehicle_orientation};
}
VehicleObservationPacket trip_heading_fixture() {
  auto packet = mode_fixture();
  packet.version = VehicleObservationWireVersion::hvo4;
  packet.sample.trip = trip_state("current/1/12.5/3600/1800/12.5/total/7.2");
  packet.sample.heading = qualified_heading();
  packet.sample.observation_timing->trip = {
      at(1800) + 3ns, 1s, VehicleObservationOrigin::calculated, VehicleAvailability::available};
  packet.sample.observation_timing->heading = {
      at(1999) + 7ns, 100ms, VehicleObservationOrigin::measured, VehicleAvailability::available};
  return packet;
}
void trip_heading_roundtrip_compatibility_and_size() {
  const auto packet = trip_heading_fixture();
  const auto encoded = encode_vehicle_observation_datagram(packet);
  const auto decoded = parse_vehicle_observation_datagram(encoded, at(2100));
  check(
      encoded.starts_with("HVO4 ") && tokens(encoded).size() == 44 && decoded &&
          decoded->version == VehicleObservationWireVersion::hvo4 &&
          decoded->sample.trip == packet.sample.trip &&
          decoded->sample.heading == packet.sample.heading &&
          decoded->sample.observation_timing->trip.acquired_at == at(1800) + 3ns &&
          decoded->sample.observation_timing->trip.origin == VehicleObservationOrigin::calculated &&
          decoded->sample.observation_timing->heading.acquired_at == at(1999) + 7ns &&
          decoded->sample.observation_timing->heading.maximum_age == 100ms &&
          encode_vehicle_observation_datagram(*decoded) == encoded,
      "HVO4 lost coherent trip/qualified heading or source dates");
  for (const auto state :
       {"current/1/0/0/0/-/-/-", "current/1/-/100/-/-/-/-",
        "current/1/12.5/3600/1800/25/moving/7.2", "current/1/12.5/3600/1800/-/-/-"}) {
    auto changed = packet;
    changed.sample.trip = trip_state(state);
    const auto result =
        parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(changed), at(2100));
    check(result && result->sample.trip == changed.sample.trip,
          "HVO4 inferred missing trip components or lost average basis");
  }
  for (const auto reference : {HeadingReference::true_north, HeadingReference::magnetic_north}) {
    for (const auto source :
         {HeadingSource::vehicle_orientation, HeadingSource::course_over_ground}) {
      auto changed = packet;
      changed.sample.heading = HeadingState{0, reference, source};
      const auto result = parse_vehicle_observation_datagram(
          encode_vehicle_observation_datagram(changed), at(2100));
      check(result && result->sample.heading == changed.sample.heading,
            "zero heading lost its qualified source/reference");
    }
  }
  auto reordered = tokens(encoded);
  std::swap(reordered[6], reordered.back());
  std::swap(reordered[7], reordered[42]);
  check(parse_vehicle_observation_datagram(join(reordered), at(2100)).has_value(),
        "trip/heading observation order was not semantic");
  auto lost = packet;
  lost.sample.trip.reset();
  lost.sample.heading.reset();
  lost.sample.observation_timing->trip.availability = VehicleAvailability::disconnected;
  lost.sample.observation_timing->heading.availability = VehicleAvailability::disconnected;
  const auto unavailable =
      parse_vehicle_observation_datagram(encode_vehicle_observation_datagram(lost), at(2100));
  check(unavailable && !unavailable->sample.trip && !unavailable->sample.heading &&
            unavailable->sample.observation_timing->trip.acquired_at == at(1800) + 3ns &&
            unavailable->sample.observation_timing->heading.acquired_at == at(1999) + 7ns,
        "loss erased original trip/heading dates");
  auto legacy = packet.sample;
  legacy.observation_timing.reset();
  rejects([&] { encode_vehicle_datagram(legacy); });
  legacy.trip.reset();
  rejects([&] { encode_vehicle_datagram(legacy); });
  legacy = packet.sample;
  legacy.observation_timing.reset();
  legacy.heading.reset();
  rejects([&] { encode_vehicle_datagram(legacy); });
  for (const auto version :
       {VehicleObservationWireVersion::hvo1, VehicleObservationWireVersion::hvo2,
        VehicleObservationWireVersion::hvo3}) {
    auto previous = fixture();
    previous.version = version;
    const auto historical = encode_vehicle_observation_datagram(previous);
    const auto old = parse_vehicle_observation_datagram(historical, at(3000));
    check(
        old &&
            tokens(historical).size() == (version == VehicleObservationWireVersion::hvo1   ? 39
                                          : version == VehicleObservationWireVersion::hvo2 ? 40
                                                                                           : 42) &&
            encode_vehicle_observation_datagram(*old) == historical && !old->sample.trip &&
            !old->sample.heading && !old->sample.observation_timing->trip.acquired_at &&
            !old->sample.observation_timing->heading.acquired_at &&
            old->sample.observation_timing->trip.maximum_age == 500ms &&
            old->sample.observation_timing->heading.maximum_age == 500ms,
        "historical schema or unknown trip/heading timing changed");
    for (const bool trip_channel : {false, true}) {
      auto bad = previous;
      if (trip_channel)
        bad.sample.trip = packet.sample.trip;
      else
        bad.sample.heading = packet.sample.heading;
      if (trip_channel)
        bad.sample.observation_timing->trip = packet.sample.observation_timing->trip;
      else
        bad.sample.observation_timing->heading = packet.sample.observation_timing->heading;
      rejects([&] { encode_vehicle_observation_datagram(bad); });
      const std::array<std::function<void(VehicleSignalTiming&)>, 4> losses{
          [](auto& t) { t.acquired_at = at(1900); }, [](auto& t) { t.maximum_age = 501ms; },
          [](auto& t) { t.origin = VehicleObservationOrigin::calculated; },
          [](auto& t) {
            t.availability = VehicleAvailability::disconnected;
          }};
      for (const auto& change : losses) {
        bad = previous;
        change(trip_channel ? bad.sample.observation_timing->trip
                            : bad.sample.observation_timing->heading);
        check(!valid_vehicle_observation_packet(bad, at(2100)),
              "historical HVO silently lost trip/heading metadata");
        rejects([&] { encode_vehicle_observation_datagram(bad); });
      }
    }
  }
  auto unknown = fixture();
  unknown.version = VehicleObservationWireVersion::hvo4;
  const auto explicit_unknown = encode_vehicle_observation_datagram(unknown);
  check(tokens(explicit_unknown).size() == 44 &&
            explicit_unknown.ends_with(" trip=-,-,500000000,m,u heading=-,-,500000000,m,u"),
        "HVO4 omitted its new unknown observation records");
  // The widest supported catalog and trip identity, all 38 real values, full
  // integer identities and host-date precision must fit and survive exactly.
  auto widest = unknown;
  widest.sequence = std::numeric_limits<std::uint64_t>::max();
  widest.sample.observation_timing->generation = std::numeric_limits<std::uint64_t>::max();
  widest.sample.observation_timing->started_at = Clock::time_point{1ns};
  widest.sample.sampled_at =
      Clock::time_point{Clock::duration{std::numeric_limits<Clock::duration::rep>::max()}};
  auto& timing = *widest.sample.observation_timing;
  for (std::size_t i = 0; i < trip_heading_vehicle_observation_capacity; ++i)
    vehicle_signal_timing(timing, i) = {widest.sample.sampled_at, 24h,
                                        VehicleObservationOrigin::measured,
                                        VehicleAvailability::available};
  for (const auto& definition : reading_definitions)
    widest.sample.*definition.member = definition.maximum;
  widest.sample.speed_kmh = 0;
  widest.sample.simulated = false;
  for (auto& indicator : widest.sample.indicators)
    indicator = IndicatorState::flashing;
  widest.sample.transmission =
      TransmissionState{GearboxType::continuously_variable, TransmissionSelector::sport,
                        EngagedGear{EngagedGearKind::forward, 32}};
  std::string full;
  for (unsigned i = 0; i < 32; ++i) {
    if (i) full += ',';
    const auto prefix = "mode" + std::to_string(i);
    full += prefix + std::string(64 - prefix.size(), 'x');
  }
  widest.sample.drive_mode_catalog = catalog(full);
  widest.sample.drive_mode = widest.sample.drive_mode_catalog->identifiers[31];
  widest.sample.trip =
      trip_state(std::string(32, 't') + "/18446744073709551615/12.5/3600/1800/12.5/total/7.2");
  widest.sample.heading = qualified_heading();
  const auto maximum = encode_vehicle_observation_datagram(widest);
  auto complete = parse_vehicle_observation_datagram(maximum, widest.sample.sampled_at);
  check(maximum.size() < maximum_vehicle_observation_datagram && tokens(maximum).size() == 44 &&
            complete && complete->sample.trip == widest.sample.trip &&
            complete->sample.drive_mode_catalog == widest.sample.drive_mode_catalog &&
            encode_vehicle_observation_datagram(*complete) == maximum,
        "maximum identities/full snapshot were truncated or not bounded");
  const auto extra = maximum_vehicle_observation_datagram - maximum.size();
  const auto field_text = std::string(extra + 1, '0') + ',' +
                          std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             widest.sample.sampled_at.time_since_epoch())
                                             .count()) +
                          ",86400000000000,m,a";
  const auto boundary = field(maximum, "speed_kmh", field_text);
  check(boundary.size() == maximum_vehicle_observation_datagram &&
            parse_vehicle_observation_datagram(boundary, widest.sample.sampled_at),
        "HVO4 exact maximum datagram rejected");
  check(!parse_vehicle_observation_datagram(boundary + 'x', widest.sample.sampled_at),
        "HVO4 oversized complete datagram accepted");
}
void trip_heading_malformed() {
  const auto good = trip_heading_fixture();
  const auto encoded = encode_vehicle_observation_datagram(good);
  auto bad = [&](std::string text) {
    check(!parse_vehicle_observation_datagram(text, at(2100)),
          "malformed HVO4 trip/heading accepted");
  };
  for (const auto value :
       {"current/0/12.5/3600/1800/12.5/total/7.2", "current/1/-/-/-/-/-/-",
        "current/1/12.5/3600/3601/12.5/total/7.2", "current/1/12.5/3600/1800/12.5/-/7.2",
        "current/1/-/-/-/12.5/unknown/7.2", "current/1/nan/3600/1800/12.5/total/7.2",
        "current/1/12.5/inf/1800/12.5/total/7.2", "current/1/-1/3600/1800/12.5/total/7.2",
        "current/1/12.5/3600/1800/12.5/total/-1", "current/1/12.5/3600/1800/12.5/total",
        "current/1/12.5/3600/1800/12.5/total/7.2/extra"})
    bad(field(encoded, "trip", std::string(value) + ",1800000003,1000000000,c,a"));
  bad(field(encoded, "trip",
            std::string(33, 't') + "/1/12.5/3600/1800/12.5/total/7.2,1800000003,1000000000,c,a"));
  for (const auto value :
       {"360/true_north/vehicle_orientation", "-1/true_north/vehicle_orientation",
        "nan/true_north/vehicle_orientation", "inf/true_north/vehicle_orientation",
        "0/unknown/vehicle_orientation", "0/true_north/unknown", "0/true_north",
        "0/true_north/vehicle_orientation/extra"})
    bad(field(encoded, "heading", std::string(value) + ",1999000007,100000000,m,a"));
  for (const auto key : {"trip", "heading"}) {
    bad(field(encoded, key, "-,-,500000000,m,a"));
    bad(field(encoded, key, "-,-,500000000,m,s"));
    bad(field(encoded, key, "-,2100000001,500000000,m,d"));
    bad(field(encoded, key, "-,999999999,500000000,m,d"));
    bad(field(encoded, key, "-,-,0,m,u"));
    bad(field(encoded, key, "-,-,86400000000001,m,u"));
  }
  auto parts = tokens(encoded);
  parts.pop_back();
  bad(join(parts));
  parts = tokens(encoded);
  parts[42] = parts[43];
  bad(join(parts));
  parts = tokens(encoded);
  parts[43] = parts[42];
  bad(join(parts));
  bad(encoded + " heading=-,-,500000000,m,u");
  for (const auto version : {"HVO1", "HVO2", "HVO3", "HVO5"}) {
    auto changed = encoded;
    changed.replace(0, 4, version);
    bad(changed);
  }
  parts = tokens(encoded);
  parts[3] = "0";
  bad(join(parts));
  parts = tokens(encoded);
  parts[4] = "2100000001";
  bad(join(parts));
  parts = tokens(encoded);
  parts[5] = "drive_modes=sport,sport";
  bad(join(parts));
  check(!parse_vehicle_observation_datagram(encoded, at(1999)),
        "HVO4 future source publication accepted");
}
void malformed() {
  const auto encoded = encode_vehicle_observation_datagram(fixture());
  auto bad = [&](std::string text) {
    check(!parse_vehicle_observation_datagram(text, at(2100)), "malformed HVO1 accepted");
  };
  bad("");
  bad(" " + encoded);
  bad(encoded + " ");
  bad(encoded + "\n");
  bad(encoded + "\r");
  bad(encoded + std::string(1, '\0'));
  bad(encoded + " extra=1");
  bad(encoded + " simulated=1");
  bad(std::string(maximum_vehicle_observation_datagram + 1, 'x'));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,m,a,extra"));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,m"));
  bad(field(encoded, "speed_kmh", "0,,100000000,m,a"));
  bad(field(encoded, "speed_kmh", "0,1950000000,,m,a"));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,,a"));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,m,"));
  for (const auto value : {"nan", "inf", "-inf", "1e999", "301", "-1", "off", "+1", "1x"})
    bad(field(encoded, "speed_kmh", std::string(value) + ",1950000000,100000000,m,a"));
  for (const auto stamp : {"0", "-1", "+1", "1x", "18446744073709551616", "9223372036854775808",
                           "2000000001", "999999999"})
    bad(field(encoded, "speed_kmh", "0," + std::string(stamp) + ",100000000,m,a"));
  for (const auto age : {"0", "-1", "+1", "1x", "86400000000001", "18446744073709551615", "1e9"})
    bad(field(encoded, "speed_kmh", "0,1950000000," + std::string(age) + ",m,a"));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,x,a"));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,m,x"));
  bad(field(encoded, "speed_kmh", "0,-,100000000,m,a"));
  bad(field(encoded, "speed_kmh", "-,1950000000,100000000,m,a"));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,m,u"));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,m,d"));
  bad(field(encoded, "speed_kmh", "0,1950000000,100000000,m,s"));
  bad(field(encoded, "speed_kmh", "-,-,100000000,m,s"));
  bad(field(encoded, "speed_kmh", "0,1900000000,100000000,m,a"));  // Exact expiry at publication.
  bad(field(encoded, "speed_kmh", "-,1900000001,100000000,m,s"));  // Fresh date cannot claim stale.
  check(parse_vehicle_observation_datagram(
            field(encoded, "speed_kmh", "-,1900000000,100000000,m,s"), at(2100))
            .has_value(),
        "exact stale boundary rejected");
  check(parse_vehicle_observation_datagram(
            field(encoded, "speed_kmh", "0,1900000001,100000000,m,a"), at(2100))
            .has_value(),
        "fresh side of boundary rejected");
  check(parse_vehicle_observation_datagram(
            field(encoded, "speed_kmh", "0,1950000000,86400000000000,m,a"), at(2100))
            .has_value(),
        "maximum age policy boundary rejected");
  check(
      parse_vehicle_observation_datagram(field(encoded, "speed_kmh", "-,-,100000000,s,d"), at(2100))
          .has_value(),
      "never-observed disconnected simulation metadata rejected");
  for (const auto state : {"0", "1", "flashing", "OFF", "nan"})
    bad(field(encoded, "high_beam", std::string(state) + ",1800000000,2000000000,m,a"));
  auto parts = tokens(encoded);
  parts.pop_back();
  bad(join(parts));
  parts = tokens(encoded);
  parts.back() = parts[5];
  bad(join(parts));
  parts = tokens(encoded);
  parts.back() = "unknown=-,-,2000000000,m,u";
  bad(join(parts));
  parts = tokens(encoded);
  parts[5] += "=x";
  bad(join(parts));
  for (std::size_t i = 1; i < 5; ++i) {
    for (const auto n : {"0", "-1", "+1", "1.0", "1x", "18446744073709551616"}) {
      parts = tokens(encoded);
      parts[i] = n;
      bad(join(parts));
    }
  }
  parts = tokens(encoded);
  parts[3] = "2000000001";
  bad(join(parts));
  parts = tokens(encoded);
  parts[4] = "2100000001";
  bad(join(parts));
  parts = tokens(encoded);
  parts[3] = "9223372036854775808";
  bad(join(parts));
  parts = tokens(encoded);
  parts[4] = "9223372036854775808";
  bad(join(parts));
  auto doubled = encoded;
  doubled.insert(doubled.find(' '), " ");
  bad(doubled);
  auto tabbed = encoded;
  tabbed[tabbed.find(' ')] = '\t';
  bad(tabbed);
  check(!parse_vehicle_observation_datagram(encoded, at(1999)), "future publication accepted");
  check(!parse_vehicle_observation_datagram(encoded, Clock::time_point{}),
        "nonpositive receive clock accepted");
}
void encoder_validation() {
  const auto good = fixture();
  auto invalid = [&](VehicleObservationPacket packet) {
    check(!valid_vehicle_observation_packet(packet, at(2100)), "invalid decoded struct accepted");
    rejects([&] { (void)encode_vehicle_observation_datagram(packet); });
  };
  auto bad = good;
  bad.sequence = 0;
  invalid(bad);
  bad = good;
  bad.sample.observation_timing.reset();
  invalid(bad);
  bad = good;
  bad.sample.observation_timing->generation = 0;
  invalid(bad);
  bad = good;
  bad.sample.sampled_at = Clock::time_point{};
  invalid(bad);
  bad = good;
  bad.sample.observation_timing->started_at = at(2001);
  invalid(bad);
  bad = good;
  bad.sample.observation_timing->readings[0].maximum_age = 0ns;
  invalid(bad);
  bad = good;
  bad.sample.observation_timing->readings[0].maximum_age = 24h + 1ns;
  invalid(bad);
  bad = good;
  bad.sample.observation_timing->readings[0].acquired_at = at(1900);
  invalid(bad);
  bad = good;
  bad.sample.observation_timing->readings[0].availability = VehicleAvailability::stale;
  bad.sample.speed_kmh.reset();
  invalid(bad);  // Fresh-at-publication cannot claim stale.
  bad = good;
  bad.sample.observation_timing->readings[0].availability = VehicleAvailability::unavailable;
  invalid(bad);
  bad = good;
  bad.sample.observation_timing->readings[0].acquired_at.reset();
  invalid(bad);
  bad = good;
  bad.sample.simulated = false;
  invalid(bad);
  bad = good;
  bad.sample.range_km = std::numeric_limits<double>::quiet_NaN();
  invalid(bad);
  check(!valid_vehicle_observation_packet(good, at(1999)), "decoded future packet accepted");
  // Encoding never samples a fresh clock or edits the input to repair expiry.
  bad = good;
  bad.sample.sampled_at = at(2050);
  rejects([&] { (void)encode_vehicle_observation_datagram(bad); });
  check(bad.sample.speed_kmh == 0 &&
            bad.sample.observation_timing->readings[0].acquired_at == at(1950),
        "encoder mutated expired source");
}
}  // namespace
int main() try {
  roundtrip();
  malformed();
  encoder_validation();
  transmission_roundtrip_and_version_shape();
  transmission_malformed();
  drive_mode_roundtrip_and_version_shape();
  drive_mode_malformed_and_bounds();
  trip_heading_roundtrip_compatibility_and_size();
  trip_heading_malformed();
  std::cout << "HVO1/HVO2/HVO3/HVO4 complete snapshots, coherent trip/heading, historical shapes "
               "and strict bounds passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
