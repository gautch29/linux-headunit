// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_vehicle_summary.hpp>
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
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
Clock::time_point at(std::int64_t ms) {
  return Clock::time_point{std::chrono::milliseconds(ms)};
}
void accept(VehicleObservationCache& owner, std::uint64_t generation, std::uint64_t sequence,
            Clock::time_point now, std::initializer_list<VehicleObservation> observations) {
  owner.accept(generation, sequence, {observations.begin(), observations.size()}, now);
}
const VehicleSummaryRow& row(const PrephoneVehicleSummary& model, std::string_view id) {
  const auto it = std::find_if(model.rows.begin(), model.rows.end(),
                               [id](const auto& item) { return item.id == id; });
  if (it == model.rows.end()) throw std::runtime_error("Missing summary row");
  return *it;
}
void no_values(const PrephoneVehicleSummary& model) {
  for (const auto& item : model.rows)
    check(!item.value, "Rejected source retained a displayed value");
}
template <class F>
void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "Production owner admitted a conflicting observation");
}
void missing_and_observed_absence() {
  VehicleObservationCache owner;
  owner.begin_generation(7, at(1000), at(1000));
  const auto blank = owner.sample(at(1000));
  auto model = prephone_vehicle_summary(blank, {7}, at(1000));
  check(model.state == VehicleSummaryState::admitted && model.rows.size() == 8,
        "Empty admitted snapshot changed scope");
  no_values(model);
  for (const auto& item : model.rows) {
    check(item.availability == VehicleSummaryAvailability::missing && !item.acquired_at &&
              !item.origin,
          "Never observed data acquired a source/date/zero");
  }
  accept(owner, 7, 1, at(1020),
         {{"fuel_percent", 0.0, at(1010)},
          {"ambient_c", -12.5, at(1015), VehicleObservationOrigin::simulation},
          {"oil_c", std::monostate{}, at(1017), VehicleObservationOrigin::measured,
           VehicleAvailability::unavailable}});
  model = prephone_vehicle_summary(owner.sample(at(1020)), {7}, at(1020));
  const auto& oil = row(model, "oil_c");
  check(oil.availability == VehicleSummaryAvailability::unavailable && !oil.value &&
            oil.acquired_at == at(1017) && oil.origin == VehicleObservationOrigin::measured,
        "Observed absence lost its evidence");
  check(row(model, "fuel_percent").value == 0 && row(model, "fuel_percent").unit == "%",
        "Known empty fuel became missing");
  check(row(model, "ambient_c").value == -12.5 &&
            row(model, "ambient_c").origin == VehicleObservationOrigin::simulation &&
            row(model, "ambient_c").unit == "C",
        "Negative simulated temperature changed");
  check(!row(model, "odometer_km").value && !row(model, "range_km").value,
        "Other rows were fabricated from fuel/temperature");
}
void mixed_rates_and_original_clock() {
  VehicleAgePolicy ages;
  ages.readings[2] = 100ms;
  ages.readings[7] = 2s;
  VehicleObservationCache owner(ages);
  owner.begin_generation(1, at(1000), at(1000));
  accept(owner, 1, 1, at(1070),
         {{"fuel_percent", 50.0, at(1001)},
          {"ambient_c", 14.0, at(1010), VehicleObservationOrigin::calculated}});
  const auto captured = owner.sample(at(1070));
  check(row(prephone_vehicle_summary(captured, {1}, at(1100)), "fuel_percent").value == 50,
        "Delayed fresh delivery was lost");
  auto model = prephone_vehicle_summary(captured, {1}, at(1101));
  check(!row(model, "fuel_percent").value &&
            row(model, "fuel_percent").availability == VehicleSummaryAvailability::stale,
        "Exact acquisition expiry was renewed by publication");
  check(row(model, "fuel_percent").acquired_at == at(1001) &&
            row(model, "fuel_percent").maximum_age == 100ms,
        "Expired evidence/policy changed");
  check(row(model, "ambient_c").value == 14 && model.source_sampled_at == at(1070),
        "Fast expiry changed slower reading/publication date");
  accept(owner, 1, 2, at(1500), {{"speed_kmh", 40.0, at(1500)}});
  model = prephone_vehicle_summary(owner.sample(at(1500)), {1}, at(1500));
  check(!row(model, "fuel_percent").value && row(model, "ambient_c").acquired_at == at(1010),
        "Unrelated speed update redated auxiliary data");
  const auto expiry = prephone_vehicle_summary(captured, {1}, at(3010));
  no_values(expiry);
  check(row(expiry, "ambient_c").availability == VehicleSummaryAvailability::stale &&
            row(expiry, "ambient_c").origin == VehicleObservationOrigin::calculated,
        "Repeated old snapshot renewed its freshness or origin");
  const auto future = prephone_vehicle_summary(captured, {1}, at(1069));
  no_values(future);
  check(future.state == VehicleSummaryState::invalid_snapshot && !future.source_sampled_at,
        "Future source publication accepted a host-clock reversal");
}
void atomic_failure_and_retirement() {
  VehicleObservationCache owner;
  owner.begin_generation(1, at(1000), at(1000));
  accept(owner, 1, 1, at(1000),
         {{"range_km", 400.0, at(1000), VehicleObservationOrigin::calculated}});
  const auto cached = owner.sample(at(1000));
  rejects([&] {
    accept(owner, 1, 2, at(1010),
           {{"range_km", 401.0, at(1010)}, {"fuel_percent", 101.0, at(1010)}});
  });
  auto model = prephone_vehicle_summary(owner.sample(at(1000)), {1}, at(1000));
  check(row(model, "range_km").value == 400 && row(model, "range_km").acquired_at == at(1000),
        "Rejected whole batch leaked a partial update");
  owner.withdraw(at(1020));
  model = prephone_vehicle_summary(owner.sample(at(1020)), {1}, at(1020));
  check(!row(model, "range_km").value &&
            row(model, "range_km").availability == VehicleSummaryAvailability::disconnected &&
            row(model, "range_km").acquired_at == at(1000),
        "Withdrawal redated/retained an available reading");
  model = prephone_vehicle_summary(cached, {1, true}, at(1020));
  no_values(model);
  check(model.state == VehicleSummaryState::disconnected &&
            row(model, "range_km").origin == VehicleObservationOrigin::calculated,
        "Still-fresh cached input bypassed explicit owner retirement");
  accept(owner, 1, 2, at(1030),
         {{"range_km", 400.0, at(1000), VehicleObservationOrigin::calculated}});
  no_values(prephone_vehicle_summary(owner.sample(at(1030)), {1}, at(1030)));
  accept(owner, 1, 3, at(1040),
         {{"range_km", 399.0, at(1040), VehicleObservationOrigin::calculated}});
  check(
      row(prephone_vehicle_summary(owner.sample(at(1040)), {1}, at(1040)), "range_km").value == 399,
      "New acquisition could not recover withdrawn input");
  owner.begin_generation(2, at(1050), at(1050));
  model = prephone_vehicle_summary(owner.sample(at(1050)), {2}, at(1050));
  no_values(model);
  check(model.source_generation == 2 && !row(model, "range_km").acquired_at,
        "Generation restart carried old dated rows");
  model = prephone_vehicle_summary(cached, {2}, at(1050));
  no_values(model);
  check(model.state == VehicleSummaryState::generation_mismatch && !model.source_generation &&
            !row(model, "range_km").acquired_at,
        "Old generation acquired its successor's identity");
}
void complete_receiver_pipeline() {
  VehicleObservationCache acquisition;
  acquisition.begin_generation(1, at(1000), at(1000));
  accept(acquisition, 1, 1, at(1010), {{"battery_voltage", 12.625, at(1003)}});
  VehicleObservationReceiver consumer;
  auto deliver = [&](VehicleSample sample, std::uint64_t sequence, Clock::time_point received) {
    const auto bytes = encode_vehicle_observation_datagram(
        {sample, sequence, VehicleObservationWireVersion::hvo4});
    const auto packet = parse_vehicle_observation_datagram(bytes, received);
    check(bool(packet), "Production codec rejected the admitted fixture");
    consumer.accept(*packet, received);
  };
  const auto old = acquisition.sample(at(1010));
  deliver(old, 1, at(1020));
  auto model = prephone_vehicle_summary(consumer.sample(at(1020)), {1}, at(1020));
  check(row(model, "battery_voltage").value == 12.625 &&
            row(model, "battery_voltage").acquired_at == at(1003) &&
            model.source_sampled_at == at(1010),
        "Codec/receiver/projection replaced source dates with receipt");
  no_values(prephone_vehicle_summary(consumer.sample(at(1503)), {1}, at(1503)));
  acquisition.begin_generation(2, at(1510), at(1510));
  deliver(acquisition.sample(at(1510)), 1, at(1510));
  no_values(prephone_vehicle_summary(consumer.sample(at(1510)), {2}, at(1510)));
  rejects([&] { deliver(old, 2, at(1520)); });
  accept(acquisition, 2, 1, at(1525), {{"battery_voltage", 13.0, at(1521)}});
  deliver(acquisition.sample(at(1525)), 2, at(1530));
  model = prephone_vehicle_summary(consumer.sample(at(1530)), {2}, at(1530));
  check(row(model, "battery_voltage").value == 13 &&
            row(model, "battery_voltage").acquired_at == at(1521) && model.source_generation == 2,
        "Rejected late old packet consumed successor sequence or revived old clock");
}
void malformed_and_undated() {
  VehicleSample legacy;
  legacy.sampled_at = at(1000);
  legacy.fuel_percent = 40;
  auto model = prephone_vehicle_summary(legacy, {1}, at(1000));
  no_values(model);
  check(model.state == VehicleSummaryState::undated_source && !model.source_generation,
        "Legacy ingress date was relabelled acquisition");
  model = prephone_vehicle_summary(legacy, {}, at(1000));
  check(model.state == VehicleSummaryState::awaiting_source, "Absent generation was fabricated");
  VehicleObservationCache owner;
  owner.begin_generation(1, at(1000), at(1000));
  accept(owner, 1, 1, at(1010), {{"fuel_percent", 25.0, at(1001)}});
  const auto good = owner.sample(at(1010));
  auto verify = [&](VehicleSample invalid) {
    const auto rejected = prephone_vehicle_summary(invalid, {1}, at(1010));
    no_values(rejected);
    check(rejected.state == VehicleSummaryState::invalid_snapshot && !rejected.source_generation &&
              !row(rejected, "fuel_percent").acquired_at,
          "Malformed snapshot partially displayed valid fields/dates");
  };
  auto invalid = good;
  invalid.oil_c = std::numeric_limits<double>::quiet_NaN();
  verify(invalid);
  invalid = good;
  invalid.observation_timing->readings[2].acquired_at = at(999);
  verify(invalid);
  invalid = good;
  invalid.observation_timing->readings[2].maximum_age = 0ns;
  verify(invalid);
  invalid = good;
  invalid.observation_timing->readings[2].availability = VehicleAvailability::disconnected;
  verify(invalid);
  invalid = good;
  invalid.observation_timing->readings[2].origin = static_cast<VehicleObservationOrigin>(99);
  verify(invalid);
}
struct CommaLocale : std::numpunct<char> {
  char do_decimal_point() const override { return ','; }
};
using Plist = std::unique_ptr<void, decltype(&plist_free)>;
std::string text(plist_t value) {
  check(value && plist_get_node_type(value) == PLIST_STRING, "JSON identity/date was not a string");
  std::uint64_t size = 0;
  const char* bytes = plist_get_string_ptr(value, &size);
  return std::string(bytes, size);
}
void json_identity_and_bounds() {
  const auto acquired = Clock::time_point{std::chrono::nanoseconds(9007199254740993LL)};
  const auto generation = std::numeric_limits<std::uint64_t>::max();
  VehicleObservationCache owner;
  owner.begin_generation(generation, acquired, acquired);
  std::array<VehicleObservation, 8> numbers;
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    const auto& definition = reading_definitions[i + 2];
    numbers[i] = {definition.key, definition.maximum, acquired};
  }
  owner.accept(generation, 1, numbers, acquired);
  accept(owner, generation, 2, acquired + 1ns, {{"battery_voltage", 12.625, acquired + 1ns}});
  const auto captured = owner.sample(acquired + 1ns);
  const auto previous_locale = std::locale();
  std::locale::global(std::locale(previous_locale, new CommaLocale));
  const auto encoded = prephone_vehicle_summary_json(captured, {generation}, acquired + 2ns);
  std::locale::global(previous_locale);
  check(encoded.size() <= prephone_vehicle_summary_json_limit,
        "Fixed maximum-value rows exceeded encoding bound");
  plist_t parsed = nullptr;
  check(plist_from_json(encoded.data(), static_cast<std::uint32_t>(encoded.size()), &parsed) ==
                PLIST_ERR_SUCCESS &&
            parsed,
        "Production JSON became locale-dependent/invalid");
  Plist object(parsed, plist_free);
  check(text(plist_dict_get_item(parsed, "sourceGeneration")) == "18446744073709551615",
        "64-bit generation lost precision");
  check(text(plist_dict_get_item(parsed, "sourceSampledNs")) == "9007199254740994",
        "Original publication lost precision or became render time");
  const auto rows = plist_dict_get_item(parsed, "rows");
  check(plist_array_get_size(rows) == 8, "JSON rows unbounded/missing");
  const auto fuel = plist_array_get_item(rows, 0);
  check(text(plist_dict_get_item(fuel, "acquiredNs")) == "9007199254740993",
        "Original nanosecond acquisition rounded");
  const auto battery = plist_array_get_item(rows, 2);
  double voltage = 0;
  plist_get_real_val(plist_dict_get_item(battery, "value"), &voltage);
  check(voltage == 12.625, "Canonical numeric reading changed under locale");
  const auto unavailable =
      prephone_vehicle_summary_json(captured, {generation, true}, acquired + 2ns);
  parsed = nullptr;
  check(plist_from_json(unavailable.data(), static_cast<std::uint32_t>(unavailable.size()),
                        &parsed) == PLIST_ERR_SUCCESS &&
            parsed,
        "Retirement JSON malformed");
  Plist retired(parsed, plist_free);
  const auto retired_fuel = plist_array_get_item(plist_dict_get_item(parsed, "rows"), 0);
  check(plist_get_node_type(plist_dict_get_item(retired_fuel, "value")) == PLIST_NULL &&
            text(plist_dict_get_item(retired_fuel, "availability")) == "disconnected",
        "Retirement JSON retained numeric value or implied zero");
}
}  // namespace
int main() {
  try {
    missing_and_observed_absence();
    mixed_rates_and_original_clock();
    atomic_failure_and_retirement();
    complete_receiver_pipeline();
    malformed_and_undated();
    json_identity_and_bounds();
    std::cout << "6 production projection/cache/codec/receiver scenarios passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
