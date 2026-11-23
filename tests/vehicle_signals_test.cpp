// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle.hpp>
#include <headunit/vehicle_wire.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/backup_cluster.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
using namespace headunit;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
TripState example_trip() {
  TripState result;
  result.identifier = *parse_trip_identifier("current");
  result.reset_generation = 1;
  result.distance_km = 60;
  result.elapsed_seconds = 3600;
  result.moving_seconds = 3000;
  result.average_speed_kmh = 60;
  result.average_speed_basis = TripAverageSpeedBasis::total;
  result.fuel_efficiency_l_per_100km = 8.5;
  return result;
}
void trip_contract() {
  const auto base = example_trip();
  check(valid_trip_state(base) && trip_state_key(base) == "current/1/60/3600/3000/60/total/8.5" &&
            parse_trip_state_key(trip_state_key(base)) == base,
        "canonical whole-current-trip statistics lost");
  check(!valid_trip_state({}) && trip_state_key({}).empty() && !valid_trip_identifier({}),
        "unknown trip became a zero trip");
  for (const auto key : {"C", "current", "trip_A-2"}) {
    const auto id = parse_trip_identifier(key);
    check(id && trip_identifier_key(*id) == key, "canonical trip identifier lost");
  }
  check(parse_trip_identifier(std::string(32, 'T')) && !parse_trip_identifier(std::string(33, 'T')),
        "trip identifier length bound failed");
  for (const auto key :
       {"", "1trip", "-trip", "_trip", "trip A", "trip/A", "trip,A", "trip+", "trip\n"})
    check(!parse_trip_identifier(key), "invalid trip identifier accepted");
  check(!parse_trip_identifier(std::string_view{"T\0A", 3}) &&
            !parse_trip_identifier(std::string_view{"T\xC3\xA9", 3}),
        "NUL/non-ASCII trip identifier accepted");
  auto bad_id = base.identifier;
  bad_id.length = 255;
  check(!valid_trip_identifier(bad_id) && trip_identifier_key(bad_id).empty(),
        "corrupt trip length exposed invalid bytes");
  bad_id = base.identifier;
  bad_id.bytes[bad_id.length] = 'x';
  check(!valid_trip_identifier(bad_id), "noncanonical unused trip bytes accepted");
  auto bad = base;
  bad.reset_generation = 0;
  check(!valid_trip_state(bad), "zero trip reset generation accepted");
  auto edge = base;
  edge.reset_generation = std::numeric_limits<std::uint64_t>::max();
  check(parse_trip_state_key(trip_state_key(edge)) == edge, "maximum trip reset generation lost");
  const std::array<std::optional<double> TripState::*, 5> statistics{
      &TripState::distance_km, &TripState::elapsed_seconds, &TripState::moving_seconds,
      &TripState::average_speed_kmh, &TripState::fuel_efficiency_l_per_100km};
  for (const auto member : statistics) {
    for (const auto invalid :
         {-1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
      bad = base;
      bad.*member = invalid;
      check(!valid_trip_state(bad) && trip_state_key(bad).empty(),
            "invalid finite trip statistic accepted");
    }
  }
  bad = base;
  bad.distance_km = maximum_trip_distance_km + 1;
  check(!valid_trip_state(bad), "excessive trip distance accepted");
  bad = base;
  bad.elapsed_seconds = maximum_trip_duration_seconds + 1;
  check(!valid_trip_state(bad), "excessive elapsed trip time accepted");
  bad = base;
  bad.moving_seconds = maximum_trip_duration_seconds + 1;
  check(!valid_trip_state(bad), "excessive moving trip time accepted");
  bad = base;
  bad.average_speed_kmh = maximum_trip_average_speed_kmh + 1;
  check(!valid_trip_state(bad), "excessive trip average accepted");
  bad = base;
  bad.fuel_efficiency_l_per_100km = maximum_trip_fuel_efficiency_l_per_100km + 1;
  check(!valid_trip_state(bad), "excessive trip fuel mean accepted");
  TripState partial;
  partial.identifier = base.identifier;
  partial.reset_generation = 1;
  check(!valid_trip_state(partial), "trip identity without statistic became available");
  partial.distance_km = 0;
  partial.elapsed_seconds = 0;
  partial.moving_seconds = 0;
  check(valid_trip_state(partial) && parse_trip_state_key(trip_state_key(partial)) == partial,
        "known zero reset counters became unavailable");
  partial.average_speed_basis = TripAverageSpeedBasis::total;
  check(!valid_trip_state(partial), "average basis without observed average accepted");
  partial.average_speed_kmh = 0;
  check(!valid_trip_state(partial), "zero average denominator accepted");
  partial.elapsed_seconds = 1;
  check(valid_trip_state(partial),
        "zero distance with positive total time and observed zero average rejected");
  partial.average_speed_basis = TripAverageSpeedBasis::moving;
  check(!valid_trip_state(partial), "zero moving denominator accepted");
  partial.moving_seconds = 1;
  check(valid_trip_state(partial), "qualified zero moving-basis average rejected");
  partial.fuel_efficiency_l_per_100km = 0;
  check(!valid_trip_state(partial), "fuel mean declared for zero travelled distance");
  partial.distance_km = 1;
  partial.elapsed_seconds = 3600;
  partial.moving_seconds = 3600;
  partial.average_speed_kmh = 1;
  check(valid_trip_state(partial), "known zero fuel mean with positive trip distance rejected");
  partial.distance_km.reset();
  check(!valid_trip_state(partial), "average/fuel mean used unavailable distance");
  bad = base;
  bad.average_speed_basis.reset();
  check(!valid_trip_state(bad), "average missing explicit duration basis accepted");
  bad = base;
  bad.average_speed_basis = static_cast<TripAverageSpeedBasis>(99);
  check(!valid_trip_state(bad), "invalid average basis accepted");
  bad = base;
  bad.average_speed_basis = TripAverageSpeedBasis::moving;
  check(!valid_trip_state(bad), "moving-basis average silently used elapsed time");
  bad.average_speed_kmh = 72;
  check(valid_trip_state(bad), "coherent moving-basis average rejected");
  bad = base;
  bad.average_speed_kmh = 60.5;
  check(valid_trip_state(bad), "whole-km/h average rounding boundary rejected");
  bad.average_speed_kmh = 60.5001;
  check(!valid_trip_state(bad), "incoherent average beyond declared rounding tolerance accepted");
  bad = base;
  bad.moving_seconds = 3601;
  check(!valid_trip_state(bad), "moving time larger than same-observation elapsed time accepted");
  bad = base;
  bad.elapsed_seconds = std::numeric_limits<double>::denorm_min();
  bad.moving_seconds.reset();
  check(!valid_trip_state(bad), "overflowing average division accepted");
  for (const auto key :
       {"", "current/1/60/3600/3000/60/total", "current/1/60/3600/3000/60/total/8.5/x",
        "current/0/1/-/-/-/-/-", "current/01/1/-/-/-/-/-", "current/+1/1/-/-/-/-/-",
        "current/18446744073709551616/1/-/-/-/-/-", "current/1/01/-/-/-/-/-",
        "current/1/1.0/-/-/-/-/-", "current/1/-0/-/-/-/-/-", "current/1/nan/-/-/-/-/-",
        "current/1/0/0/-/0/total/-", "current/1/1/1/-/1/unknown/-", "current/1/-/-/-/-/-/-"})
    check(!parse_trip_state_key(key), "malformed/noncanonical/incoherent trip key accepted");
  check(!parse_trip_state_key(std::string(513, 'x')), "oversized compound trip key accepted");
}
void trip_progress_contract() {
  auto at = [](int ms) {
    return std::chrono::steady_clock::time_point{std::chrono::milliseconds(ms)};
  };
  TripProgress progress;
  const auto base = example_trip();
  const auto pristine = progress;
  check(withdraw_trip_progress(progress) && progress == pristine,
        "unknown history was bound by withdrawal");
  check(valid_trip_progress(progress) && update_trip_progress(progress, base, at(1000)),
        "initial trip progress rejected");
  auto previous = progress;
  check(update_trip_progress(progress, base, at(1000)) && progress == previous,
        "identical equal-date progress changed watermarks");
  check(withdraw_trip_progress(progress) && progress.requires_new_acquisition,
        "known trip loss did not become sticky");
  previous = progress;
  check(!update_trip_progress(progress, base, at(1000)) && progress == previous,
        "old equal acquisition revived withdrawn trip history");
  check(update_trip_progress(progress, base, at(1001)) && !progress.requires_new_acquisition,
        "newer actual acquisition failed to recover trip history");
  auto lower_mean = base;
  lower_mean.fuel_efficiency_l_per_100km = 6;
  check(update_trip_progress(progress, lower_mean, at(1002)),
        "whole-trip fuel mean was treated as a monotonic fuel-use counter");
  previous = progress;
  auto changed = base;
  changed.fuel_efficiency_l_per_100km = 9;
  check(!update_trip_progress(progress, changed, at(1002)) && progress == previous,
        "equal-date trip statistics conflicted or partially changed");
  changed = base;
  changed.distance_km = 59;
  changed.average_speed_kmh.reset();
  changed.average_speed_basis.reset();
  check(!update_trip_progress(progress, changed, at(1100)) && progress == previous,
        "same-reset distance decreased");
  TripState missing;
  missing.identifier = base.identifier;
  missing.reset_generation = 1;
  missing.elapsed_seconds = 4000;
  check(update_trip_progress(progress, missing, at(1100)) && progress.distance_km == 60 &&
            progress.moving_seconds == 3000,
        "missing statistics discarded acquired counter watermarks");
  previous = progress;
  missing.elapsed_seconds = 3999;
  check(!update_trip_progress(progress, missing, at(1200)) && progress == previous,
        "missing-and-returning elapsed counter moved backwards");
  missing.elapsed_seconds.reset();
  missing.distance_km = 59;
  check(!update_trip_progress(progress, missing, at(1200)) && progress == previous,
        "missing-and-returning distance moved backwards");
  missing.distance_km.reset();
  missing.moving_seconds = 2999;
  check(!update_trip_progress(progress, missing, at(1200)) && progress == previous,
        "missing-and-returning moving counter moved backwards");
  TripState reset;
  reset.identifier = base.identifier;
  reset.reset_generation = 2;
  reset.distance_km = 0;
  reset.elapsed_seconds = 0;
  reset.moving_seconds = 0;
  check(!update_trip_progress(progress, reset, at(1100)) && progress == previous,
        "trip reset accepted without newer acquisition");
  check(update_trip_progress(progress, reset, at(1200)) && progress.distance_km == 0 &&
            progress.elapsed_seconds == 0 && progress.moving_seconds == 0,
        "new acquired reset did not permit zero counters");
  previous = progress;
  changed = reset;
  changed.identifier = *parse_trip_identifier("other");
  changed.reset_generation = 3;
  check(!update_trip_progress(progress, changed, at(1300)) && progress == previous,
        "lifetime current-trip identifier changed");
  changed = base;
  check(!update_trip_progress(progress, changed, at(1300)) && progress == previous,
        "trip reset generation regressed");
  changed = reset;
  changed.reset_generation = 3;
  check(!update_trip_progress(progress, changed, at(1199)) && progress == previous,
        "trip reset acquisition went backwards");
  TripProgress chronological;
  TripState moving;
  moving.identifier = base.identifier;
  moving.reset_generation = 1;
  moving.moving_seconds = 300;
  check(update_trip_progress(chronological, moving, at(1000)),
        "moving-only acquired trip rejected");
  TripState elapsed;
  elapsed.identifier = base.identifier;
  elapsed.reset_generation = 1;
  elapsed.elapsed_seconds = 250;
  previous = chronological;
  check(!update_trip_progress(chronological, elapsed, at(1100)) && chronological == previous,
        "new elapsed contradicted prior known moving time");
  elapsed.elapsed_seconds = 300;
  check(update_trip_progress(chronological, elapsed, at(1100)),
        "new elapsed did not cover equal retained moving time");
  moving.moving_seconds = 400;
  check(update_trip_progress(chronological, moving, at(1200)) &&
            valid_trip_progress(chronological) && chronological.elapsed_seconds == 300,
        "older elapsed was treated as the unavailable current elapsed counter");
  elapsed.elapsed_seconds = 350;
  previous = chronological;
  check(!update_trip_progress(chronological, elapsed, at(1300)) && chronological == previous,
        "later elapsed contradicted newer moving watermark");
  elapsed.elapsed_seconds = 400;
  check(update_trip_progress(chronological, elapsed, at(1300)),
        "later coherent elapsed failed to recover");
  auto malformed = chronological;
  malformed.elapsed_acquired_at.reset();
  check(!valid_trip_progress(malformed), "retained counter without acquisition date accepted");
  malformed = chronological;
  malformed.elapsed_acquired_at = at(1400);
  check(!valid_trip_progress(malformed), "future retained counter acquisition accepted");
  malformed = chronological;
  malformed.identifier.reset();
  check(!valid_trip_progress(malformed), "trip history lost its fixed identity");
  const auto invalid = malformed;
  check(!withdraw_trip_progress(malformed) && malformed == invalid,
        "invalid retained history was changed by withdrawal");
}
void heading_contract() {
  check(!valid_heading_state({}) && heading_state_key({}).empty(),
        "default unknown heading became north");
  for (const auto reference : {HeadingReference::true_north, HeadingReference::magnetic_north})
    for (const auto source :
         {HeadingSource::vehicle_orientation, HeadingSource::course_over_ground})
      for (const auto degrees : {0., 90., 180., 359.999}) {
        const HeadingState heading{degrees, reference, source};
        check(valid_heading_state(heading) &&
                  parse_heading_state_key(heading_state_key(heading)) == heading,
              "qualified heading/reference/source lost");
      }
  for (const auto degrees : {-1., 360., 361., std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()})
    check(!valid_heading_state(
              {degrees, HeadingReference::true_north, HeadingSource::vehicle_orientation}),
          "invalid heading wrapped or became an invented bearing");
  check(!valid_heading_state({0, HeadingReference::unknown, HeadingSource::vehicle_orientation}) &&
            !valid_heading_state({0, HeadingReference::true_north, HeadingSource::unknown}) &&
            !valid_heading_state(
                {0, static_cast<HeadingReference>(99), HeadingSource::vehicle_orientation}) &&
            !valid_heading_state({0, HeadingReference::true_north, static_cast<HeadingSource>(99)}),
        "unqualified heading accepted");
  for (const auto key : {"", "0/true_north", "0/true_north/course_over_ground/x",
                         "0/unknown/course_over_ground", "0/true_north/unknown",
                         "360/true_north/vehicle_orientation", "-1/true_north/vehicle_orientation",
                         "-0/true_north/vehicle_orientation", "00/true_north/vehicle_orientation",
                         "0.0/true_north/vehicle_orientation", "nan/true_north/vehicle_orientation",
                         "inf/true_north/vehicle_orientation", "90/True_North/vehicle_orientation"})
    check(!parse_heading_state_key(key), "malformed/noncanonical/unqualified heading key accepted");
  check(!parse_heading_state_key(std::string(129, 'x')), "oversized heading key accepted");
}
void drive_mode_contract() {
  check(!valid_drive_mode_identifier({}) && drive_mode_identifier_key({}).empty() &&
            !valid_drive_mode_catalog({}),
        "unknown drive mode/catalog was valid or fabricated");
  for (const auto key : {"G", "comfort", "GT", "sport_plus", "all-terrain-2", "Mode0"}) {
    const auto id = parse_drive_mode_identifier(key);
    check(id && valid_drive_mode_identifier(*id) && drive_mode_identifier_key(*id) == key,
          "canonical drive mode identifier lost");
  }
  const auto longest = parse_drive_mode_identifier(std::string(64, 'A'));
  check(longest && longest->length == 64 && drive_mode_identifier_key(*longest).size() == 64,
        "maximum drive mode identifier rejected");
  check(!parse_drive_mode_identifier(std::string(65, 'A')),
        "oversized drive mode identifier accepted");
  for (const auto key : {"", "1sport", "_sport", "-sport", "Sport+", " sport", "sport ",
                         "sport,track", "sport/track", "mode.name", "mode\n"})
    check(!parse_drive_mode_identifier(key), "invalid drive mode identifier accepted");
  check(!parse_drive_mode_identifier(std::string_view{"G\0T", 3}) &&
            !parse_drive_mode_identifier(std::string_view{"M\xC3\xA9", 3}),
        "NUL/non-ASCII drive mode accepted");
  auto bad = *parse_drive_mode_identifier("GT");
  bad.length = 255;
  check(!valid_drive_mode_identifier(bad) && drive_mode_identifier_key(bad).empty(),
        "malformed drive mode length exposed bytes outside fixed storage");
  bad = *parse_drive_mode_identifier("GT");
  bad.bytes[2] = 'x';
  check(!valid_drive_mode_identifier(bad) && drive_mode_identifier_key(bad).empty(),
        "noncanonical unused identifier bytes accepted");
  const auto catalog = parse_drive_mode_catalog_key("comfort,GT,gt,sport_plus,track");
  check(catalog && catalog->count == 5 && valid_drive_mode_catalog(*catalog) &&
            drive_mode_catalog_key(*catalog) == "comfort,GT,gt,sport_plus,track" &&
            parse_drive_mode_catalog_key(drive_mode_catalog_key(*catalog)) == catalog,
        "ordered case-sensitive drive mode catalog lost");
  check(contains_drive_mode(*catalog, *parse_drive_mode_identifier("GT")) &&
            !contains_drive_mode(*catalog, *parse_drive_mode_identifier("Sport")) &&
            !contains_drive_mode(*catalog, {}),
        "unsupported/default mode accepted by catalog");
  for (const auto key : {"", "comfort,comfort", "comfort,", ",comfort", "comfort,,track",
                         "comfort, track", "comfort,Sport+"})
    check(!parse_drive_mode_catalog_key(key), "malformed/duplicate drive mode catalog accepted");
  DriveModeCatalog full;
  full.count = 32;
  for (std::size_t i = 0; i < full.count; ++i) {
    std::string key(64, 'A');
    key[0] = static_cast<char>('A' + i / 26);
    key[1] = static_cast<char>('A' + i % 26);
    full.identifiers[i] = *parse_drive_mode_identifier(key);
  }
  const auto full_key = drive_mode_catalog_key(full);
  check(valid_drive_mode_catalog(full) && full_key.size() == 32 * 64 + 31 &&
            parse_drive_mode_catalog_key(full_key) == full,
        "maximum bounded drive mode catalog rejected");
  check(!parse_drive_mode_catalog_key(full_key + ",other"), "catalog beyond 32 modes accepted");
  std::string too_many;
  for (unsigned i = 0; i < 33; ++i) {
    if (i) too_many += ',';
    too_many += "mode" + std::to_string(i);
  }
  check(too_many.size() < 32 * 64 + 31 && !parse_drive_mode_catalog_key(too_many),
        "catalog count beyond 32 accepted within byte limit");
  auto invalid = *catalog;
  invalid.count = 255;
  check(!valid_drive_mode_catalog(invalid) && drive_mode_catalog_key(invalid).empty() &&
            !contains_drive_mode(invalid, catalog->identifiers[0]),
        "malformed catalog count exposed bytes outside fixed storage");
  invalid = *catalog;
  invalid.identifiers[0] = {};
  check(!valid_drive_mode_catalog(invalid), "empty active catalog slot accepted");
  invalid = *catalog;
  invalid.identifiers[1] = invalid.identifiers[0];
  check(!valid_drive_mode_catalog(invalid), "duplicate raw catalog accepted");
  invalid = *catalog;
  invalid.identifiers[5] = *parse_drive_mode_identifier("hidden");
  check(!valid_drive_mode_catalog(invalid), "noncanonical unused catalog slot accepted");
  invalid = *catalog;
  invalid.identifiers[31].bytes[0] = 'x';
  check(!valid_drive_mode_catalog(invalid), "unused catalog bytes accepted");
}
void transmission_contract() {
  const auto neutral = EngagedGear{EngagedGearKind::neutral, 0};
  const auto reverse = EngagedGear{EngagedGearKind::reverse, 0};
  check(!valid_transmission_state({}) && transmission_display_label(std::nullopt) == "--",
        "unknown transmission was inferred");
  for (const auto type : {GearboxType::manual, GearboxType::automatic,
                          GearboxType::automated_manual, GearboxType::continuously_variable}) {
    check(parse_gearbox_type_key(gearbox_type_key(type)) == type, "gearbox semantic key changed");
    const TransmissionState typed{type, std::nullopt, std::nullopt};
    check(valid_transmission_state(typed) && transmission_display_label(typed) == "--",
          "gearbox type fabricated a selector/gear");
    check(parse_transmission_state_key(transmission_state_key(typed)) == typed,
          "partially known transmission semantic state lost");
    const TransmissionState reversing{type, TransmissionSelector::reverse, reverse};
    check(parse_transmission_state_key(transmission_state_key(reversing)) == reversing,
          "complete longest gearbox/selector/gear key lost");
  }
  check(gearbox_type_key(static_cast<GearboxType>(99)).empty() &&
            !parse_gearbox_type_key("Automatic"),
        "invalid gearbox type key accepted");
  for (const auto selector :
       {TransmissionSelector::park, TransmissionSelector::reverse, TransmissionSelector::neutral,
        TransmissionSelector::drive, TransmissionSelector::low, TransmissionSelector::manual,
        TransmissionSelector::sport}) {
    check(parse_transmission_selector_key(transmission_selector_key(selector)) == selector,
          "selector semantic key changed");
    const std::string_view expected = selector == TransmissionSelector::park      ? "P"
                                      : selector == TransmissionSelector::reverse ? "R"
                                      : selector == TransmissionSelector::neutral ? "N"
                                      : selector == TransmissionSelector::drive   ? "D"
                                      : selector == TransmissionSelector::low     ? "L"
                                      : selector == TransmissionSelector::manual  ? "M"
                                                                                  : "S";
    check(transmission_display_label(TransmissionState{std::nullopt, selector, std::nullopt}) ==
              expected,
          "effective selector fallback label lost");
    for (const auto gear : {neutral, reverse, EngagedGear{EngagedGearKind::forward, 1},
                            EngagedGear{EngagedGearKind::forward, 32}}) {
      const bool coherent =
          selector == TransmissionSelector::park || selector == TransmissionSelector::neutral
              ? gear.kind == EngagedGearKind::neutral
          : selector == TransmissionSelector::reverse ? gear.kind == EngagedGearKind::reverse
                                                      : gear.kind != EngagedGearKind::reverse;
      check(valid_transmission_state({GearboxType::automatic, selector, gear}) == coherent,
            "incoherent selector/actual gear accepted");
      const TransmissionState state{GearboxType::automatic, selector, gear};
      if (coherent)
        check(parse_transmission_state_key(transmission_state_key(state)) == state,
              "coherent transmission compound key lost");
      else
        check(transmission_state_key(state).empty(),
              "incoherent transmission compound key serialized");
    }
  }
  check(transmission_selector_key(static_cast<TransmissionSelector>(99)).empty() &&
            !parse_transmission_selector_key("D"),
        "invalid/abbreviated selector key accepted");
  for (unsigned i = 1; i <= 32; ++i) {
    const auto gear = EngagedGear{EngagedGearKind::forward, static_cast<std::uint8_t>(i)};
    check(parse_engaged_gear_key(engaged_gear_key(gear)) == gear &&
              transmission_display_label(TransmissionState{
                  std::nullopt, TransmissionSelector::sport, gear}) == std::to_string(i),
          "actual forward gear was replaced by operating mode");
  }
  for (const auto gear : {neutral, reverse}) {
    check(parse_engaged_gear_key(engaged_gear_key(gear)) == gear,
          "neutral/reverse semantic key lost");
    check(transmission_display_label(TransmissionState{std::nullopt, std::nullopt, gear}) ==
              (gear.kind == EngagedGearKind::neutral ? "N" : "R"),
          "actual neutral/reverse display missing");
  }
  check(transmission_display_label(TransmissionState{GearboxType::automatic,
                                                     TransmissionSelector::park, neutral}) == "P" &&
            transmission_display_label(TransmissionState{
                GearboxType::manual, TransmissionSelector::manual, neutral}) == "N",
        "effective park or actual neutral display precedence changed");
  for (const auto gear :
       {EngagedGear{EngagedGearKind::neutral, 1}, EngagedGear{EngagedGearKind::reverse, 1},
        EngagedGear{EngagedGearKind::forward, 0}, EngagedGear{EngagedGearKind::forward, 33},
        EngagedGear{static_cast<EngagedGearKind>(99), 0}}) {
    check(engaged_gear_key(gear).empty() &&
              !valid_transmission_state({std::nullopt, std::nullopt, gear}),
          "invalid actual gear accepted");
  }
  for (const auto value :
       {"", "0", "33", "256", "01", "+1", "-1", "1.0", "1 ", " 1", "R", "Neutral"})
    check(!parse_engaged_gear_key(value), "noncanonical or invalid gear key accepted");
  for (const auto value : {"", "-", "-/-/-", "automatic/drive", "automatic/drive/1/",
                           "automatic//1", "automatic/reverse/1", "automatic/park/reverse",
                           "manual/-/01", "automatic/drive/1 ", "Automatic/-/-"})
    check(!parse_transmission_state_key(value),
          "malformed or incoherent transmission compound key accepted");
  check(!parse_transmission_state_key(std::string(8192, 'x')),
        "unbounded compound transmission key accepted");
  check(parse_transmission_state_key("-/-/32") ==
                TransmissionState{std::nullopt, std::nullopt,
                                  EngagedGear{EngagedGearKind::forward, 32}} &&
            parse_transmission_state_key("-/drive/-") ==
                TransmissionState{std::nullopt, TransmissionSelector::drive, std::nullopt},
        "known actual gear or effective selector required an inferred gearbox type");
  check(!valid_transmission_state({static_cast<GearboxType>(99), std::nullopt, std::nullopt}) &&
            !valid_transmission_state(
                {std::nullopt, static_cast<TransmissionSelector>(99), std::nullopt}),
        "unknown transmission enum accepted");
}
int main() try {
  trip_contract();
  trip_progress_contract();
  heading_contract();
  drive_mode_contract();
  transmission_contract();
  const auto now = Clock::time_point{std::chrono::milliseconds(2000)};
  SimulatedVehicle simulator(now);
  auto sample = simulator.sample(now);
  check(sample.simulated && valid_vehicle_sample(sample), "invalid simulation");
  check(sample.transmission && sample.transmission->selector == TransmissionSelector::drive &&
            sample.transmission->engaged_gear &&
            sample.transmission->engaged_gear->kind == EngagedGearKind::forward,
        "simulation lacks a coherent automatic Drive state");
  check(sample.drive_mode && sample.drive_mode_catalog &&
            drive_mode_identifier_key(*sample.drive_mode) == "sport",
        "simulation lacks its selected drive mode");
  // HUV2 carries neither typed transmission nor drive modes.
  bool huv2_rejected = false;
  try {
    (void)encode_vehicle_datagram(sample);
  } catch (const std::invalid_argument&) {
    huv2_rejected = true;
  }
  check(huv2_rejected, "HUV2 accepted typed transmission or drive modes");
  sample.transmission.reset();
  sample.drive_mode.reset();
  sample.drive_mode_catalog.reset();
  auto warning_sample = sample;
  for (auto& state : warning_sample.indicators)
    state.reset();
  auto snapshot = vehicle_indicator_snapshot(warning_sample, now);
  check(
      snapshot.fresh_data && snapshot.simulated && snapshot.known.none() && snapshot.visible.none(),
      "unknown telltales became off or visible");
  for (const auto& lamp : indicator_definitions) {
    const auto index = static_cast<std::size_t>(lamp.id);
    warning_sample.indicator(lamp.id) = IndicatorState::off;
    snapshot = vehicle_indicator_snapshot(warning_sample, now);
    check(snapshot.known.test(index) && !snapshot.visible.test(index),
          "known off telltale was lost or visible");
    warning_sample.indicator(lamp.id) = IndicatorState::on;
    check(vehicle_indicator_snapshot(warning_sample, now).visible.test(index),
          "active telltale missing");
    warning_sample.indicator(lamp.id) = IndicatorState::flashing;
    check(vehicle_indicator_snapshot(warning_sample, now).visible.test(index),
          "flash bright phase missing");
    warning_sample.sampled_at = now + std::chrono::milliseconds(400);
    snapshot = vehicle_indicator_snapshot(warning_sample, warning_sample.sampled_at);
    check(snapshot.known.test(index) && !snapshot.visible.test(index),
          "flash dark phase lost its known state or stayed visible");
    warning_sample.indicator(lamp.id).reset();
    warning_sample.sampled_at = now;
  }
  warning_sample.indicator(Indicator::oil_pressure) = IndicatorState::on;
  for (const auto time :
       {now - std::chrono::milliseconds(1), now + std::chrono::milliseconds(501)}) {
    snapshot = vehicle_indicator_snapshot(warning_sample, time);
    check(!snapshot.fresh_data && snapshot.known.none() && snapshot.visible.none(),
          "future or stale telltale retained");
  }
  warning_sample.indicator(Indicator::oil_pressure) = static_cast<IndicatorState>(99);
  snapshot = vehicle_indicator_snapshot(warning_sample, now);
  check(!snapshot.fresh_data && snapshot.visible.none(), "invalid telltale snapshot accepted");
  std::set<std::string_view> keys;
  for (const auto& d : reading_definitions)
    check(keys.insert(d.key).second, "duplicate reading key");
  for (const auto& d : indicator_definitions)
    check(keys.insert(d.key).second, "duplicate indicator key");
  auto packet = encode_vehicle_datagram(sample);
  auto received = parse_vehicle_datagram(packet, now);
  check(received && received->simulated && received->sampled_at == now, "HUV2 snapshot missing");
  {
    VehicleObservationCache source;
    source.begin_generation(1, now, now);
    const std::array values{VehicleObservation{"speed_kmh", 0., now}};
    source.accept(1, 1, values, now);
    bool rejected = false;
    try {
      (void)encode_vehicle_datagram(source.sample(now));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "HUV2 silently discarded acquisition metadata");
  }
  {
    auto typed = sample;
    typed.transmission = TransmissionState{GearboxType::manual, std::nullopt,
                                           EngagedGear{EngagedGearKind::forward, 2}};
    check(valid_vehicle_sample(typed), "valid legacy typed transmission rejected");
    check(current_vehicle_sample(typed, now + std::chrono::milliseconds(500)).transmission ==
              typed.transmission,
          "legacy inclusive transmission expiry changed");
    check(!current_vehicle_sample(
               typed, now + std::chrono::milliseconds(500) + std::chrono::nanoseconds(1))
               .transmission,
          "legacy stale actual gear retained");
    bool rejected = false;
    try {
      (void)encode_vehicle_datagram(typed);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "HUV2 silently discarded typed transmission");
    typed.transmission->selector = TransmissionSelector::reverse;
    check(!valid_vehicle_sample(typed) && !current_vehicle_sample(typed, now).transmission,
          "invalid selector/actual gear retained in sample");
  }
  {
    const auto modes = *parse_drive_mode_catalog_key("comfort,sport");
    auto typed = sample;
    typed.drive_mode_catalog = modes;
    typed.drive_mode = *parse_drive_mode_identifier("sport");
    check(valid_vehicle_sample(typed), "valid legacy effective drive mode rejected");
    const auto expired = current_vehicle_sample(typed, now + std::chrono::milliseconds(501));
    check(!expired.drive_mode && expired.drive_mode_catalog == modes,
          "legacy expiry retained mode or removed static capability catalog");
    typed.drive_mode = *parse_drive_mode_identifier("Sport");
    check(!valid_vehicle_sample(typed) && !current_vehicle_sample(typed, now).drive_mode,
          "unsupported case-sensitive drive mode accepted/retained");
    typed.drive_mode.reset();
    typed.drive_mode_catalog.reset();
    typed.transmission =
        TransmissionState{GearboxType::automatic, TransmissionSelector::sport, std::nullopt};
    check(valid_vehicle_sample(typed) && !current_vehicle_sample(typed, now).drive_mode,
          "transmission sport selector fabricated vehicle drive mode");
    typed.transmission.reset();
    typed.drive_mode = *parse_drive_mode_identifier("sport");
    check(!valid_vehicle_sample(typed), "effective mode accepted without vehicle catalog");
    typed.drive_mode.reset();
    typed.drive_mode_catalog = modes;
    typed.drive_mode_catalog->count = 0;
    const auto invalid = current_vehicle_sample(typed, now);
    check(!valid_vehicle_sample(typed) && !valid_vehicle_sample(invalid) &&
              invalid.drive_mode_catalog,
          "invalid static catalog became valid by filtering other readings");
  }
  {
    auto typed = sample;
    typed.trip = example_trip();
    typed.heading =
        HeadingState{90, HeadingReference::magnetic_north, HeadingSource::vehicle_orientation};
    check(valid_vehicle_sample(typed) &&
              current_vehicle_sample(typed, now + std::chrono::milliseconds(500)).trip ==
                  typed.trip &&
              current_vehicle_sample(typed, now + std::chrono::milliseconds(500)).heading ==
                  typed.heading,
          "legacy typed trip/heading inclusive expiry changed");
    const auto stale = current_vehicle_sample(
        typed, now + std::chrono::milliseconds(500) + std::chrono::nanoseconds(1));
    check(!stale.trip && !stale.heading, "legacy stale trip/heading retained known values");
    bool rejected = false;
    try {
      (void)encode_vehicle_datagram(typed);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "HUV2 silently discarded typed trip/heading");
    typed.trip.reset();
    rejected = false;
    try {
      (void)encode_vehicle_datagram(typed);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "HUV2 silently discarded typed heading alone");
    typed.heading.reset();
    typed.trip = example_trip();
    rejected = false;
    try {
      (void)encode_vehicle_datagram(typed);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "HUV2 silently discarded typed trip alone");
    typed.trip->reset_generation = 0;
    check(!valid_vehicle_sample(typed) && !current_vehicle_sample(typed, now).trip,
          "invalid current-trip epoch retained");
  }
  for (const auto& d : reading_definitions)
    check(received.value().*d.member == sample.*d.member, "reading roundtrip");
  for (const auto& d : indicator_definitions) {
    for (auto state : {IndicatorState::off, IndicatorState::on, IndicatorState::flashing}) {
      sample.indicator(d.id) = state;
      const auto value = parse_vehicle_datagram(encode_vehicle_datagram(sample), now);
      check(value && value->indicator(d.id) == state, "indicator state lost");
    }
    sample.indicator(d.id).reset();
  }
  for (const auto& d : reading_definitions) {
    VehicleSample edge;
    edge.sampled_at = now;
    for (double n : {d.minimum, d.maximum}) {
      edge.*d.member = n;
      check(bool(parse_vehicle_datagram(encode_vehicle_datagram(edge), now)),
            "valid reading boundary rejected");
    }
    for (double n : {d.minimum - 1, d.maximum + 1, std::numeric_limits<double>::quiet_NaN()}) {
      edge.*d.member = n;
      check(!valid_vehicle_sample(edge), "bad reading accepted");
      bool rejected = false;
      try {
        (void)encode_vehicle_datagram(edge);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      check(rejected, "bad reading serialized");
    }
  }
  const auto partial = parse_vehicle_datagram(
      "HUV2 fuel_percent=0 coolant_c=-10 check_engine=off high_beam=flash", now);
  check(partial && partial->fuel_percent == 0 && partial->coolant_c == -10 && !partial->speed_kmh &&
            partial->indicator(Indicator::check_engine) == IndicatorState::off &&
            !partial->indicator(Indicator::oil_pressure),
        "unknown/zero/off conflated");
  for (const auto* bad : {"HUV2 ", "HUV2 fuel_percent=101", "HUV2 coolant_c=nan", "HUV2 low_fuel=1",
                          "HUV2 low_fuel=off low_fuel=on", "HUV2 check_engine=off ",
                          "HUV2 speed_kmh=4  engine_rpm=9", "HUV2 typo=1", "HUV2 simulated=yes"})
    check(!parse_vehicle_datagram(bad, now), "malformed HUV2 accepted");
  check(!parse_vehicle_datagram(std::string(2049, 'x'), now), "oversized packet accepted");
  const auto legacy = parse_vehicle_datagram("HUV1 10 1200", now);
  check(legacy && !legacy->fuel_percent && !legacy->indicator(Indicator::check_engine),
        "legacy packet fabricated new signals");
  const auto envelope =
      parse_vehicle_datagram("HUF1 1800000000 HUV2 speed_kmh=42 low_fuel=on", now);
  check(envelope && envelope->sampled_at == now - std::chrono::milliseconds(200) &&
            envelope->speed_kmh == 42,
        "broker ingress time was not retained");
  check(!fresh(*envelope, now + std::chrono::milliseconds(301), std::chrono::milliseconds(500)),
        "delayed broker sample was renewed by consumer receipt");
  for (const auto* bad :
       {"HUF1 2000000001 HUV1 10 1200", "HUF1 -1 HUV1 10 1200", "HUF1 0 HUV1 10 1200",
        "HUF1 9223372036854775808 HUV1 10 1200", "HUF1 1x HUV1 10 1200", "HUF1  HUV1 10 1200",
        "HUF1 800000000 HUF1 800000000 HUV1 10 1200"})
    check(!parse_vehicle_datagram(bad, now), "invalid/nested broker timestamp accepted");
  check(!parse_vehicle_datagram("HUF1 800000000 " + std::string(maximum_vehicle_datagram + 1, 'x'),
                                now),
        "broker envelope allowed an oversized inner snapshot");
  VehicleSample off;
  off.sampled_at = now;
  off.fuel_percent = 50;
  off.coolant_c = 88;
  for (auto& lamp : off.indicators)
    lamp = IndicatorState::off;
  const auto base = render_backup_cluster(off, now);
  for (const auto& d : indicator_definitions) {
    auto on = off;
    on.indicator(d.id) = IndicatorState::on;
    check(render_backup_cluster(on, now).pixels != base.pixels, "indicator not displayed");
    on.indicator(d.id) = IndicatorState::flashing;
    const auto blink_on = render_backup_cluster(on, now);
    // Keep sample fresh while changing only the flash phase.
    on.sampled_at = now + std::chrono::milliseconds(400);
    check(render_backup_cluster(on, on.sampled_at).pixels != blink_on.pixels,
          "indicator not flashing");
    on.indicator(d.id).reset();
    auto off_later = off;
    off_later.sampled_at = on.sampled_at;
    check(render_backup_cluster(on, on.sampled_at).pixels ==
              render_backup_cluster(off_later, on.sampled_at).pixels,
          "unknown indicator must stay hidden");
  }
  check(render_backup_cluster(off, now + std::chrono::seconds(1)).pixels ==
            render_backup_cluster({}, now + std::chrono::seconds(1)).pixels,
        "stale values or lights retained");
  std::cout << "Vehicle readings, typed transmission/mode/trip/heading, 24 indicators, HUV2 and "
               "unavailable/flash rendering passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
