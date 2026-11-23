// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster.hpp>
#include <headunit/vehicle_observation.hpp>
#include <cairo.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
struct TextDraw {
  std::string value;
  double x, y;
  cairo_text_extents_t ink;
};
std::vector<TextDraw> text_draws;
void observed_show_text(cairo_t* context, const char* value) {
  double x = 0, y = 0;
  cairo_get_current_point(context, &x, &y);
  cairo_text_extents_t ink{};
  cairo_text_extents(context, value, &ink);
  text_draws.push_back({value, x, y, ink});
  cairo_show_text(context, value);
}
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
}  // namespace

// Compile the actual renderer source with only its final Cairo text call
// observed. No production hook/API is added. Delegate to real Cairo, then compare
// every frame with the independently linked, unmodified production renderer.
#define cairo_show_text observed_show_text
#define draw_native_cluster observed_draw_native_cluster
#define render_native_cluster observed_render_native_cluster
#define selected_cluster_center observed_selected_cluster_center
#define native_cluster_map observed_native_cluster_map
#include "../src/native_cluster.cpp"
#undef cairo_show_text
#undef draw_native_cluster
#undef render_native_cluster
#undef selected_cluster_center
#undef native_cluster_map

namespace {
using namespace headunit;
constexpr std::array pages{
    ClusterCenter::navigation, ClusterCenter::media, ClusterCenter::vehicle, ClusterCenter::trip,
    ClusterCenter::minimal,    ClusterCenter::sport, ClusterCenter::gauges};
struct Frame {
  Image image;
  std::vector<TextDraw> text;
};
Frame draw(const VehicleSample& sample, Clock::time_point now, ClusterCenter page,
           const NativeClusterConfiguration& config) {
  text_draws.clear();
  const auto image =
      observed_render_native_cluster(sample, now, 1920, 720, {}, {}, page, false, config);
  check(image.pixels ==
            render_native_cluster(sample, now, 1920, 720, {}, {}, page, false, config).pixels,
        "text observation changed actual production Cairo pixels");
  return {image, text_draws};
}
std::size_t card(const Frame& frame, const char* name, const char* number, const char* unit,
                 const char* detail = "") {
  for (std::size_t i = 0; i + 3 < frame.text.size(); ++i) {
    if (frame.text[i].value != name) continue;
    if (frame.text[i + 1].value != number || frame.text[i + 2].value != unit ||
        frame.text[i + 3].value != detail)
      throw std::runtime_error(std::string("wrong Cairo card ") + name + ": " +
                               frame.text[i + 1].value + " " + frame.text[i + 2].value + " / " +
                               frame.text[i + 3].value);
    const auto& value = frame.text[i + 1];
    const auto& label = frame.text[i + 2];
    const double left = value.x + value.ink.x_bearing;
    const double right = std::max(value.x + value.ink.x_bearing + value.ink.width,
                                  label.x + label.ink.x_bearing + label.ink.width);
    const auto column = std::lround((frame.text[i].x - 582) / (824. / 3.));
    const double start = 560 + double(column) * (824. / 3.);
    check(left >= start + 16 && right <= start + 752. / 3. - 16,
          "numeric text/unit escaped existing card allocation");
    return i;
  }
  throw std::runtime_error("existing card allocation absent");
}
void distance(const Frame& frame, ClusterCenter page, const char* number, const char* unit) {
  if (page == ClusterCenter::vehicle) {
    card(frame, "RANGE", number, unit);
  } else if (page == ClusterCenter::trip) {
    card(frame, "DISTANCE", number, unit);
  } else if (page == ClusterCenter::gauges) {
    for (std::size_t i = 2; i < frame.text.size(); ++i) {
      if (frame.text[i].value != "RANGE") continue;
      const auto& value = frame.text[i - 2];
      const auto& label = frame.text[i - 1];
      check(value.value == number && label.value == unit, "wrong actual Cairo range dial text");
      check(value.x + value.ink.x_bearing >= 896 &&
                value.x + value.ink.x_bearing + value.ink.width <= 1024 &&
                label.x + label.ink.x_bearing >= 896 &&
                label.x + label.ink.x_bearing + label.ink.width <= 1024,
            "range text escaped existing dial interior");
      return;
    }
    throw std::runtime_error("existing range dial absent");
  }
}
bool target_ink(const Frame& frame, unsigned x, unsigned y, bool speed = false) {
  for (std::size_t i = 1; i < frame.text.size(); ++i) {
    const auto& label = frame.text[i];
    if (label.value != "km" && label.value != "mi" &&
        !(speed && (label.value == "km/h" || label.value == "mph")))
      continue;
    for (const auto index : {i - 1, i}) {
      const auto& t = frame.text[index];
      if (double(x) >= std::floor(t.x + t.ink.x_bearing) - 1 &&
          double(x) <= std::ceil(t.x + t.ink.x_bearing + t.ink.width) + 1 &&
          double(y) >= std::floor(t.y + t.ink.y_bearing) - 1 &&
          double(y) <= std::ceil(t.y + t.ink.y_bearing + t.ink.height) + 1)
        return true;
    }
  }
  return false;
}
void only_target_text(const Frame& first, const Frame& second, bool expected_change,
                      bool average_only = false) {
  std::size_t changed = 0;
  for (unsigned y = 0; y < first.image.height; ++y)
    for (unsigned x = 0; x < first.image.width; ++x) {
      // Speed-unit configuration already controls the outer instrument ticks.
      // The related Trip repair must change only average text inside the center.
      if (average_only && !cluster_layout::center.contains(x, y)) continue;
      const auto index = std::size_t(y) * first.image.width + x;
      if (first.image.pixels[index] == second.image.pixels[index]) continue;
      check(target_ink(first, x, y, average_only) || target_ink(second, x, y, average_only),
            "units changed ring, lamp, consumption, source detail or unrelated pixels");
      ++changed;
    }
  check((changed > 0) == expected_change, "target text did not preserve expected pixel contract");
}
TripState supplied(double kilometers, std::optional<double> speed = {}) {
  TripState value;
  value.identifier = *parse_trip_identifier("currentTrip");
  value.reset_generation = 1;
  value.distance_km = kilometers;
  if (speed) {
    value.elapsed_seconds = 3600.;
    value.average_speed_kmh = speed;
    value.average_speed_basis = TripAverageSpeedBasis::total;
  }
  return value;
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid source observation accepted");
}
}  // namespace

int main() try {
  using namespace headunit;
  using namespace std::chrono_literals;
  const auto now = Clock::time_point{10s};
  auto sample = SimulatedVehicle(now).sample(now);
  sample.indicators.fill(IndicatorState::off);
  sample.indicator(Indicator::low_fuel) = IndicatorState::on;
  sample.range_km = 1.609344;
  sample.trip = supplied(1.609344, 1.609344);
  sample.trip->fuel_efficiency_l_per_100km = 7.4;
  const std::string legacy =
      "version=1\nspeed_unit=kmh\nspeed_maximum_kmh=260\nrpm_maximum=8000\nrpm_redline=6500\nshow_"
      "rpm=true\n";
  NativeClusterConfiguration kilometers, miles;
  miles.distance_unit = NativeDistanceUnit::miles;
  const auto implicit = parse_native_cluster_configuration(legacy);
  const auto explicit_km =
      parse_native_cluster_configuration(legacy + "distance_unit=kilometers\n");
  for (const auto page : pages) {
    const auto original = draw(sample, now, page, kilometers);
    check(original.image.pixels == draw(sample, now, page, implicit).image.pixels &&
              original.image.pixels == draw(sample, now, page, explicit_km).image.pixels,
          "default/omitted/explicit kilometer configuration changed pixels");
    const auto converted = draw(sample, now, page, miles);
    distance(converted, page, page == ClusterCenter::trip ? "1.0" : "1", "mi");
    only_target_text(original, converted,
                     page == ClusterCenter::vehicle || page == ClusterCenter::gauges ||
                         page == ClusterCenter::trip);
  }
  struct Known {
    double km;
    const char* range;
    const char* trip;
  };
  // Independent known mile values and rounded results, never computed expected values.
  for (const auto& known : std::array{
           Known{0, "0", "0.0"}, Known{1.609344, "1", "1.0"}, Known{1609.344, "1000", "1000.0"},
           Known{3000, "1864", "1864.1"}, Known{1, "1", "0.6"}, Known{0.8, "0", "0.5"},
           Known{0.81, "1", "0.5"}, Known{1.68, "1", "1.0"}, Known{1.70, "1", "1.1"}}) {
    sample.range_km = known.km;
    sample.trip = supplied(known.km);
    for (const auto page : {ClusterCenter::vehicle, ClusterCenter::gauges, ClusterCenter::trip}) {
      const auto original = draw(sample, now, page, kilometers);
      const auto converted = draw(sample, now, page, miles);
      distance(converted, page, page == ClusterCenter::trip ? known.trip : known.range, "mi");
      only_target_text(original, converted, true);
    }
  }
  sample.trip = supplied(9999999);
  distance(draw(sample, now, ClusterCenter::trip, miles), ClusterCenter::trip, "6213711.3", "mi");
  sample.trip = supplied(60, 60);
  sample.trip->moving_seconds = 3600.;
  sample.trip->fuel_efficiency_l_per_100km = 7.4;
  // Speed and distance options are independent in all four combinations.
  for (const auto unit : {NativeDistanceUnit::kilometers, NativeDistanceUnit::miles}) {
    for (const bool mph : {false, true}) {
      auto config = kilometers;
      config.distance_unit = unit;
      config.miles_per_hour = mph;
      for (const auto basis : {TripAverageSpeedBasis::total, TripAverageSpeedBasis::moving}) {
        sample.trip->average_speed_basis = basis;
        const auto result = draw(sample, now, ClusterCenter::trip, config);
        distance(result, ClusterCenter::trip, unit == NativeDistanceUnit::miles ? "37.3" : "60.0",
                 unit == NativeDistanceUnit::miles ? "mi" : "km");
        card(result, "AVERAGE SPEED", mph ? "37.3" : "60.0", mph ? "mph" : "km/h",
             basis == TripAverageSpeedBasis::moving ? "Moving time basis" : "Total time basis");
        card(result, "CONSUMPTION", "7.4", "L/100 km", "Whole-trip mean");
        if (mph) {
          auto kmh = config;
          kmh.miles_per_hour = false;
          only_target_text(draw(sample, now, ClusterCenter::trip, kmh), result, true, true);
        }
      }
    }
  }
  sample.trip = supplied(1.609344, 1.609344);
  auto mph = kilometers;
  mph.miles_per_hour = true;
  card(draw(sample, now, ClusterCenter::trip, mph), "AVERAGE SPEED", "1.0", "mph",
       "Total time basis");
  sample.trip = supplied(0, 0);
  card(draw(sample, now, ClusterCenter::trip, mph), "AVERAGE SPEED", "0.0", "mph",
       "Total time basis");
  sample.trip = supplied(0);
  card(draw(sample, now, ClusterCenter::trip, mph), "AVERAGE SPEED", "--", "mph");
  sample.trip->distance_km.reset();
  sample.trip->elapsed_seconds = 12.;
  distance(draw(sample, now, ClusterCenter::trip, miles), ClusterCenter::trip, "--", "mi");
  for (const auto invalid : {-0.01, 3000.01, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
    sample.range_km = invalid;
    distance(draw(sample, now, ClusterCenter::vehicle, miles), ClusterCenter::vehicle, "--", "mi");
  }
  sample.range_km.reset();
  distance(draw(sample, now, ClusterCenter::gauges, miles), ClusterCenter::gauges, "--", "mi");
  sample.trip = supplied(-1);
  distance(draw(sample, now, ClusterCenter::trip, miles), ClusterCenter::trip, "--", "mi");
  sample.trip =
      supplied(1, 20);  // Invalid supplied mean/basis coherence, never infer a replacement.
  card(draw(sample, now, ClusterCenter::trip, mph), "AVERAGE SPEED", "--", "mph");
  sample = SimulatedVehicle(now).sample(now);
  sample.trip = supplied(60, 60);
  sample.sampled_at = now - 1s;
  distance(draw(sample, now, ClusterCenter::trip, miles), ClusterCenter::trip, "--", "mi");
  card(draw(sample, now, ClusterCenter::trip, mph), "AVERAGE SPEED", "--", "mph");

  VehicleAgePolicy ages;
  ages.readings[8] = 100ms;
  ages.trip = 200ms;
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, now, now);
  const auto trip = supplied(60, 60);
  const std::array observations{
      VehicleObservation{"range_km", 1.609344, now, VehicleObservationOrigin::calculated},
      VehicleObservation{"trip", trip, now, VehicleObservationOrigin::simulation},
      VehicleObservation{"speed_kmh", 80., now, VehicleObservationOrigin::measured}};
  cache.accept(7, 1, observations, now);
  const auto held = cache.sample(now);
  auto combined = miles;
  combined.miles_per_hour = true;
  distance(draw(held, now + 99ms, ClusterCenter::gauges, combined), ClusterCenter::gauges, "1",
           "mi");
  distance(draw(held, now + 100ms, ClusterCenter::gauges, combined), ClusterCenter::gauges, "--",
           "mi");
  card(draw(held, now + 199ms, ClusterCenter::trip, combined), "AVERAGE SPEED", "37.3", "mph",
       "Total time basis");
  distance(draw(held, now + 200ms, ClusterCenter::trip, combined), ClusterCenter::trip, "--", "mi");
  card(draw(held, now + 200ms, ClusterCenter::trip, combined), "AVERAGE SPEED", "--", "mph");
  const auto repeated = cache.sample(now + 50ms);
  const auto same_timing = [](const VehicleSignalTiming& first, const VehicleSignalTiming& second) {
    return first.acquired_at == second.acquired_at && first.maximum_age == second.maximum_age &&
           first.origin == second.origin && first.availability == second.availability;
  };
  check(held.range_km == 1.609344 && held.trip == trip && repeated.range_km == held.range_km &&
            repeated.trip == held.trip &&
            same_timing(held.observation_timing->readings[8],
                        repeated.observation_timing->readings[8]) &&
            same_timing(held.observation_timing->trip, repeated.observation_timing->trip) &&
            held.observation_timing->generation == repeated.observation_timing->generation &&
            held.observation_timing->started_at == repeated.observation_timing->started_at &&
            held.observation_timing->readings[8].acquired_at == now &&
            held.observation_timing->readings[8].origin == VehicleObservationOrigin::calculated &&
            held.observation_timing->trip.acquired_at == now &&
            held.observation_timing->trip.origin == VehicleObservationOrigin::simulation,
        "render/repeat read changed original canonical values, basis, age or provenance");
  auto invalid_trip = trip;
  invalid_trip.average_speed_basis.reset();
  rejects([&] {
    const std::array bad{VehicleObservation{"trip", invalid_trip, now + 51ms}};
    cache.accept(7, 2, bad, now + 51ms);
  });
  rejects([&] {
    const std::array bad{VehicleObservation{"range_km", -1., now + 51ms}};
    cache.accept(7, 2, bad, now + 51ms);
  });
  cache.withdraw(now + 60ms);
  const auto withdrawn = cache.sample(now + 60ms);
  distance(draw(withdrawn, now + 60ms, ClusterCenter::gauges, combined), ClusterCenter::gauges,
           "--", "mi");
  distance(draw(withdrawn, now + 60ms, ClusterCenter::trip, combined), ClusterCenter::trip, "--",
           "mi");
  check(withdrawn.observation_timing->readings[8].acquired_at == now &&
            withdrawn.observation_timing->trip.acquired_at == now &&
            withdrawn.observation_timing->trip.availability == VehicleAvailability::disconnected,
        "withdrawal erased original acquisition or fabricated application receipt");
  cache.accept(7, 2, observations, now + 61ms);
  distance(draw(cache.sample(now + 61ms), now + 61ms, ClusterCenter::trip, combined),
           ClusterCenter::trip, "--", "mi");
  cache.begin_generation(8, now + 70ms, now + 70ms);
  const auto successor = cache.sample(now + 70ms);
  distance(draw(successor, now + 70ms, ClusterCenter::gauges, combined), ClusterCenter::gauges,
           "--", "mi");
  card(draw(successor, now + 70ms, ClusterCenter::trip, combined), "AVERAGE SPEED", "--", "mph");
  check(!successor.range_km && !successor.trip && !successor.observation_timing->trip.acquired_at,
        "successor inherited retired source data");
  std::cout << "Real Cairo distance/average units, known conversions, pixels, canonical "
               "arcs/source and original expiry/retirement passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
