// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_indicator_summary.hpp>
#include <headunit/vehicle_observation_receiver.hpp>
#include <plist/plist.h>
#include <algorithm>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <stdexcept>

using namespace headunit;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
Clock::time_point at(std::int64_t ms) {
  return Clock::time_point{std::chrono::milliseconds(ms)};
}
PrephoneIndicatorAdmission admitted(std::uint64_t generation, bool retired = false) {
  return {{generation, retired}, PrephoneIndicatorClockDomain::host_steady};
}
const PrephoneIndicatorRow& row(const PrephoneIndicatorSummary& model, Indicator indicator) {
  return model.rows.at(static_cast<std::size_t>(indicator));
}
void no_values(const PrephoneIndicatorSummary& model) {
  for (const auto& item : model.rows)
    check(!item.value, "Unavailable source retained lamp value");
}
void no_source(const PrephoneIndicatorSummary& model) {
  no_values(model);
  check(!model.source_generation && !model.source_started_at && !model.source_sampled_at,
        "Rejected snapshot retained source identity");
  for (const auto& item : model.rows)
    check(!item.acquired_at && !item.origin && !item.age && !item.maximum_age,
          "Rejected snapshot partially exposed metadata");
}
template <class F>
void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "Production source admitted invalid observation");
}
void accept(VehicleObservationCache& source, std::uint64_t generation, std::uint64_t sequence,
            Clock::time_point now, std::initializer_list<VehicleObservation> observations) {
  source.accept(generation, sequence, {observations.begin(), observations.size()}, now);
}
void catalogue_and_canonical_states() {
  VehicleObservationCache source;
  source.begin_generation(1, at(1000), at(1000));
  const auto missing = prephone_indicator_summary(source.sample(at(1000)), admitted(1), at(1000));
  check(missing.rows.size() == 24 && missing.state == VehicleSummaryState::admitted,
        "Fixed catalogue/scope changed");
  for (std::size_t i = 0; i < missing.rows.size(); ++i) {
    const auto& item = missing.rows[i];
    const auto& definition = indicator_definitions[i];
    check(item.indicator == definition.id && item.id == definition.key &&
              item.label == definition.label && item.color == definition.color && !item.value &&
              item.availability == VehicleSummaryAvailability::missing && !item.acquired_at &&
              !item.origin && !item.age && item.maximum_age == 500ms,
          "Definitions became observations or changed catalogue");
  }
  std::uint64_t sequence = 0;
  for (auto state : {IndicatorState::off, IndicatorState::on, IndicatorState::flashing}) {
    const auto time = at(1001 + static_cast<std::int64_t>(sequence));
    std::array<VehicleObservation, 24> batch;
    for (std::size_t i = 0; i < batch.size(); ++i)
      batch[i] = {indicator_definitions[i].key, state, time, VehicleObservationOrigin::simulation};
    source.accept(1, ++sequence, batch, time);
    const auto model = prephone_indicator_summary(source.sample(time), admitted(1), time);
    for (const auto& item : model.rows)
      check(item.value == state && item.origin == VehicleObservationOrigin::simulation &&
                item.availability == VehicleSummaryAvailability::available && item.age == 0ns,
            "Canonical state/explicit simulated provenance changed");
  }
}
void mixed_expiry_and_copy_dates() {
  VehicleAgePolicy ages;
  ages.indicators[0] = 100ms;
  ages.indicators[1] = 500ms;
  VehicleObservationCache source(ages);
  source.begin_generation(7, at(1000), at(1000));
  accept(source, 7, 1, at(1020),
         {{"left_turn", IndicatorState::flashing, at(1010)},
          {"right_turn", IndicatorState::flashing, at(1015)},
          {"low_beam", IndicatorState::off, at(1017)},
          {"high_beam", std::monostate{}, at(1018), VehicleObservationOrigin::calculated,
           VehicleAvailability::unavailable}});
  const auto original = source.sample(at(1020));
  const auto legacy_before = prephone_vehicle_summary_json(original, {7}, at(1020));
  auto model = prephone_indicator_summary(original, admitted(7), at(1050));
  check(!row(model, Indicator::hazard).value &&
            row(model, Indicator::hazard).availability == VehicleSummaryAvailability::missing,
        "Two directions inferred a hazard");
  check(row(model, Indicator::low_beam).value == IndicatorState::off &&
            row(model, Indicator::high_beam).availability ==
                VehicleSummaryAvailability::unavailable &&
            row(model, Indicator::high_beam).origin == VehicleObservationOrigin::calculated,
        "Known OFF or observed absence lost original meaning");
  for (const auto now : {at(1050), at(1109), at(1110), at(1515)}) {
    auto copied = original;
    model = prephone_indicator_summary(copied, admitted(7), now);
    check(model.source_sampled_at == at(1020) && model.source_started_at == at(1000) &&
              model.rendered_at == now &&
              row(model, Indicator::left_turn).acquired_at == at(1010) &&
              row(model, Indicator::left_turn).maximum_age == 100ms &&
              row(model, Indicator::left_turn).age == now - at(1010),
          "Repeated copy renewed publication/acquisition/immutable age");
    check(row(model, Indicator::left_turn).value ==
              (now < at(1110) ? std::optional{IndicatorState::flashing} : std::nullopt),
          "Left independent exact expiry changed");
    check(row(model, Indicator::right_turn).value ==
              (now < at(1515) ? std::optional{IndicatorState::flashing} : std::nullopt),
          "Another lamp renewed right expiry or invented animation state");
    const auto bytes = prephone_indicator_summary_json(copied, admitted(7), now);
    check(bytes.size() <= prephone_indicator_summary_json_limit, "Serialization exceeded bound");
  }
  // The cache's own subsequent publication also preserves individual acquisition.
  accept(source, 7, 2, at(1110), {{"speed_kmh", 42.0, at(1110)}});
  model = prephone_indicator_summary(source.sample(at(1110)), admitted(7), at(1110));
  check(!row(model, Indicator::left_turn).value &&
            row(model, Indicator::left_turn).availability == VehicleSummaryAvailability::stale &&
            row(model, Indicator::right_turn).value == IndicatorState::flashing,
        "New speed refreshed lamps");
  check(prephone_vehicle_summary_json(original, {7}, at(1020)) == legacy_before,
        "Indicator projection changed legacy eight-row encoding");
}
void flashing_has_no_phase() {
  VehicleAgePolicy ages;
  ages.indicators.fill(5s);
  VehicleObservationCache source(ages);
  source.begin_generation(1, at(1000), at(1000));
  accept(source, 1, 1, at(1001), {{"left_turn", IndicatorState::flashing, at(1001)}});
  const auto snapshot = source.sample(at(1001));
  bool lit = false;
  bool dark = false;
  for (std::int64_t ms = 1001; ms < 2001; ms += 10) {
    const auto now = at(ms);
    lit |= indicator_flash_visible(now);
    dark |= !indicator_flash_visible(now);
    check(row(prephone_indicator_summary(snapshot, admitted(1), now), Indicator::left_turn).value ==
              IndicatorState::flashing,
          "Flashing summary was turned into observed ON/OFF phase");
  }
  check(lit && dark, "Fixture did not cover both animation phases");
}
void atomic_acquisition_failures() {
  VehicleObservationCache source;
  source.begin_generation(1, at(1000), at(1000));
  accept(source, 1, 1, at(1010), {{"low_beam", IndicatorState::off, at(1005)}});
  const auto expected =
      prephone_indicator_summary_json(source.sample(at(1010)), admitted(1), at(1010));
  auto bad = [&](VehicleObservation observation, Clock::time_point now = at(1020)) {
    rejects([&] {
      accept(source, 1, 2, now, {{"right_turn", IndicatorState::on, at(1010)}, observation});
    });
    check(
        prephone_indicator_summary_json(source.sample(at(1010)), admitted(1), at(1010)) == expected,
        "Atomic failure consumed time/sequence or changed lamp");
  };
  bad({"low_beam", IndicatorState::on, at(999)});
  bad({"low_beam", IndicatorState::on, at(1021)});
  bad({"low_beam", IndicatorState::on, at(1004)});
  bad({"low_beam", IndicatorState::on, at(1005)});
  bad({"low_beam", static_cast<IndicatorState>(99), at(1006)});
  bad({"low_beam", IndicatorState::on, at(1006), static_cast<VehicleObservationOrigin>(99)});
  bad({"low_beam", IndicatorState::on, at(1006), VehicleObservationOrigin::measured,
       VehicleAvailability::stale});
  bad({"low_beam", IndicatorState::on, at(1006)}, at(1009));
  rejects([&] { accept(source, 2, 2, at(1020), {{"low_beam", IndicatorState::on, at(1011)}}); });
  accept(source, 1, 2, at(1011), {{"low_beam", IndicatorState::on, at(1011)}});
  check(row(prephone_indicator_summary(source.sample(at(1011)), admitted(1), at(1011)),
            Indicator::low_beam)
                .value == IndicatorState::on,
        "Rejected failures prevented genuine recovery");
}
void withdrawal_generation_and_owner_end() {
  VehicleObservationCache source;
  source.begin_generation(1, at(1000), at(1000));
  accept(source, 1, 1, at(1005), {{"low_beam", IndicatorState::off, at(1005)}});
  const auto original = source.sample(at(1005));
  auto retired = prephone_indicator_summary(original, admitted(1, true), at(1006));
  no_values(retired);
  check(retired.state == VehicleSummaryState::disconnected && retired.source_generation == 1 &&
            row(retired, Indicator::low_beam).acquired_at == at(1005) &&
            row(retired, Indicator::low_beam).availability ==
                VehicleSummaryAvailability::disconnected,
        "End-of-owner retained a fresh lamp or redated old source");
  source.withdraw(at(1010));
  accept(source, 1, 2, at(1011), {{"low_beam", IndicatorState::off, at(1005)}});
  auto current = prephone_indicator_summary(source.sample(at(1011)), admitted(1), at(1011));
  no_values(current);
  check(row(current, Indicator::low_beam).acquired_at == at(1005) &&
            row(current, Indicator::low_beam).availability ==
                VehicleSummaryAvailability::disconnected,
        "Equal-date repeat revived withdrawn lamp");
  accept(source, 1, 3, at(1012), {{"low_beam", IndicatorState::off, at(1012)}});
  check(row(prephone_indicator_summary(source.sample(at(1012)), admitted(1), at(1012)),
            Indicator::low_beam)
                .value == IndicatorState::off,
        "Fresh recovery lost known OFF");
  source.begin_generation(2, at(1020), at(1020));
  current = prephone_indicator_summary(source.sample(at(1020)), admitted(2), at(1020));
  no_values(current);
  check(!row(current, Indicator::low_beam).acquired_at && current.source_generation == 2,
        "Successor inherited retired source acquisition");
  current = prephone_indicator_summary(original, admitted(2), at(1020));
  no_source(current);
  check(current.state == VehicleSummaryState::generation_mismatch, "Foreign generation admitted");
}
void invalid_snapshot_and_clock_admission() {
  VehicleObservationCache source;
  source.begin_generation(1, at(1000), at(1000));
  accept(source, 1, 1, at(1010), {{"low_beam", IndicatorState::on, at(1005)}});
  const auto good = source.sample(at(1010));
  auto verify = [&](VehicleSample invalid, Clock::time_point now = at(1010)) {
    const auto model = prephone_indicator_summary(invalid, admitted(1), now);
    no_source(model);
    check(model.state == VehicleSummaryState::invalid_snapshot, "Malformed snapshot admitted");
  };
  auto invalid = good;
  invalid.indicator(Indicator::low_beam) = static_cast<IndicatorState>(99);
  verify(invalid);
  invalid = good;
  invalid.observation_timing->indicators[4].acquired_at = at(999);
  verify(invalid);
  invalid = good;
  invalid.observation_timing->indicators[4].acquired_at = at(1011);
  verify(invalid);
  invalid = good;
  invalid.observation_timing->indicators[4].maximum_age = 0ns;
  verify(invalid);
  invalid = good;
  invalid.observation_timing->indicators[4].maximum_age = 25h;
  verify(invalid);
  invalid = good;
  invalid.observation_timing->indicators[4].origin = static_cast<VehicleObservationOrigin>(99);
  verify(invalid);
  invalid = good;
  invalid.observation_timing->indicators[4].availability = static_cast<VehicleAvailability>(99);
  verify(invalid);
  invalid = good;
  invalid.observation_timing->indicators[4].availability = VehicleAvailability::disconnected;
  verify(invalid);
  invalid = good;
  invalid.observation_timing->generation = 0;
  verify(invalid);
  invalid = good;
  invalid.simulated = true;
  verify(invalid);
  invalid = good;
  invalid.sampled_at = at(999);
  verify(invalid);
  verify(good, at(1009));
  verify(good, Clock::time_point{});
  for (auto domain :
       {PrephoneIndicatorClockDomain::unqualified, static_cast<PrephoneIndicatorClockDomain>(99)}) {
    const auto model = prephone_indicator_summary(good, {{1}, domain}, at(1010));
    no_source(model);
    check(model.state == VehicleSummaryState::invalid_snapshot &&
              model.clock_domain == PrephoneIndicatorClockDomain::unqualified,
          "Unqualified/foreign clock acquired comparable source dates");
  }
  const auto absent = prephone_indicator_summary(good, admitted(0), at(1010));
  no_source(absent);
  check(absent.state == VehicleSummaryState::awaiting_source, "Missing owner invented source");
  SimulatedVehicle legacy(at(1000));
  const auto undated = prephone_indicator_summary(legacy.sample(at(1010)), admitted(1), at(1010));
  no_source(undated);
  check(undated.state == VehicleSummaryState::undated_source, "Legacy simulation dated by redraw");
}
void production_receiver_copies() {
  VehicleObservationCache source;
  VehicleObservationReceiver receiver;
  source.begin_generation(1, at(1000), at(1000));
  accept(source, 1, 1, at(1010), {{"left_turn", IndicatorState::flashing, at(1005)}});
  const auto original = source.sample(at(1010));
  auto deliver = [&](VehicleSample snapshot, std::uint64_t sequence, Clock::time_point now) {
    const auto bytes = encode_vehicle_observation_datagram(
        {snapshot, sequence, VehicleObservationWireVersion::hvo4});
    const auto parsed = parse_vehicle_observation_datagram(bytes, now);
    check(bool(parsed), "Production codec rejected fixture");
    receiver.accept(*parsed, now);
  };
  deliver(original, 1, at(1020));
  deliver(original, 2, at(1030));
  auto model = prephone_indicator_summary(receiver.sample(at(1030)), admitted(1), at(1030));
  check(model.source_sampled_at == at(1010) &&
            row(model, Indicator::left_turn).acquired_at == at(1005),
        "Receipt/copy renewed original clocks");
  model = prephone_indicator_summary(receiver.sample(at(1505)), admitted(1), at(1505));
  no_values(model);
  source.withdraw(at(1506));
  deliver(source.sample(at(1506)), 3, at(1506));
  rejects([&] { deliver(original, 4, at(1507)); });
  no_values(prephone_indicator_summary(receiver.sample(at(1507)), admitted(1), at(1507)));
  source.begin_generation(2, at(1510), at(1510));
  deliver(source.sample(at(1510)), 1, at(1510));
  rejects([&] { deliver(original, 5, at(1511)); });
  no_values(prephone_indicator_summary(receiver.sample(at(1511)), admitted(2), at(1511)));
}
using Plist = std::unique_ptr<void, decltype(&plist_free)>;
std::string text(plist_t node) {
  check(node && plist_get_node_type(node) == PLIST_STRING, "JSON string/date type changed");
  std::uint64_t size = 0;
  const char* data = plist_get_string_ptr(node, &size);
  return {data, size};
}
struct CommaLocale : std::numpunct<char> {
  char do_decimal_point() const override { return ','; }
};
void json_schema_and_bound() {
  const auto acquired =
      Clock::time_point{std::chrono::nanoseconds(std::numeric_limits<std::int64_t>::max() - 3)};
  const auto generation = std::numeric_limits<std::uint64_t>::max();
  VehicleAgePolicy policy;
  policy.indicators.fill(24h);
  VehicleObservationCache source(policy);
  source.begin_generation(generation, acquired, acquired);
  std::array<VehicleObservation, 24> observations;
  for (std::size_t i = 0; i < observations.size(); ++i)
    observations[i] = {indicator_definitions[i].key, IndicatorState::flashing, acquired,
                       VehicleObservationOrigin::simulation};
  source.accept(generation, 1, observations, acquired);
  const auto snapshot = source.sample(acquired + 1ns);
  const auto previous = std::locale();
  std::locale::global(std::locale(previous, new CommaLocale));
  const auto bytes =
      prephone_indicator_summary_json(snapshot, admitted(generation), acquired + 2ns);
  std::locale::global(previous);
  check(bytes.size() <= prephone_indicator_summary_json_limit, "Maximum-width JSON exceeded bound");
  plist_t parsed = nullptr;
  check(plist_from_json(bytes.data(), static_cast<std::uint32_t>(bytes.size()), &parsed) ==
                PLIST_ERR_SUCCESS &&
            parsed,
        "JSON invalid or locale dependent");
  Plist object(parsed, plist_free);
  check(plist_dict_get_size(parsed) == 8 &&
            text(plist_dict_get_item(parsed, "sourceGeneration")) == "18446744073709551615" &&
            text(plist_dict_get_item(parsed, "sourceSampledNs")) == "9223372036854775805" &&
            text(plist_dict_get_item(parsed, "renderedNs")) == "9223372036854775806" &&
            text(plist_dict_get_item(parsed, "clockDomain")) == "host-steady",
        "Exact schema/64-bit identity/source/render separation lost");
  const auto rows = plist_dict_get_item(parsed, "rows");
  check(plist_array_get_size(rows) == 24, "JSON row bound changed");
  for (std::size_t i = 0; i < 24; ++i) {
    const auto item = plist_array_get_item(rows, static_cast<std::uint32_t>(i));
    check(plist_dict_get_size(item) == 9 &&
              text(plist_dict_get_item(item, "id")) == indicator_definitions[i].key &&
              text(plist_dict_get_item(item, "label")) == indicator_definitions[i].label &&
              text(plist_dict_get_item(item, "value")) == "FLASHING" &&
              text(plist_dict_get_item(item, "acquiredNs")) == "9223372036854775804" &&
              text(plist_dict_get_item(item, "ageNs")) == "2" &&
              text(plist_dict_get_item(item, "maximumAgeNs")) == "86400000000000" &&
              text(plist_dict_get_item(item, "origin")) == "simulation",
          "JSON canonical state/original acquisition/provenance changed");
  }
  const auto retired =
      prephone_indicator_summary_json(snapshot, admitted(generation, true), acquired + 2ns);
  parsed = nullptr;
  check(plist_from_json(retired.data(), static_cast<std::uint32_t>(retired.size()), &parsed) ==
                PLIST_ERR_SUCCESS &&
            parsed,
        "Retirement JSON invalid");
  Plist retired_object(parsed, plist_free);
  const auto lamp = plist_array_get_item(plist_dict_get_item(parsed, "rows"), 0);
  check(plist_get_node_type(plist_dict_get_item(lamp, "value")) == PLIST_NULL &&
            text(plist_dict_get_item(lamp, "availability")) == "disconnected",
        "JSON retirement implied OFF or retained observed lamp");
  std::cout << "maximum-width fixture JSON " << bytes.size() << " bytes / "
            << prephone_indicator_summary_json_limit << " limit\n";
}
}  // namespace
int main() {
  try {
    catalogue_and_canonical_states();
    mixed_expiry_and_copy_dates();
    flashing_has_no_phase();
    atomic_acquisition_failures();
    withdrawal_generation_and_owner_end();
    invalid_snapshot_and_clock_admission();
    production_receiver_copies();
    json_schema_and_bound();
    std::cout << "8 production indicator/cache/codec/receiver scenarios passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
