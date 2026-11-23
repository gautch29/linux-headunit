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
bool target_ink(const Frame& frame, unsigned x, unsigned y) {
  for (std::size_t i = 0; i + 2 < frame.text.size(); ++i) {
    if (frame.text[i].value != "CONSUMPTION") continue;
    for (const auto index : {i + 1, i + 2}) {
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
void only_consumption_text(const Frame& first, const Frame& second, bool expected_change) {
  std::size_t changed = 0;
  for (unsigned y = 0; y < first.image.height; ++y)
    for (unsigned x = 0; x < first.image.width; ++x) {
      const auto index = std::size_t(y) * first.image.width + x;
      if (first.image.pixels[index] == second.image.pixels[index]) continue;
      check(target_ink(first, x, y) || target_ink(second, x, y),
            "consumption changed unrelated text, geometry, rings, lamps or source detail");
      ++changed;
    }
  check((changed > 0) == expected_change, "unexpected consumption pixel identity");
  check(first.text.size() == second.text.size(), "consumption changed text allocations");
  bool target = false;
  unsigned remaining = 0;
  for (std::size_t i = 0; i < first.text.size(); ++i) {
    if (first.text[i].value == "CONSUMPTION") {
      target = true;
      remaining = 2;
      continue;
    }
    if (target && remaining) {
      --remaining;
      continue;
    }
    check(first.text[i].value == second.text[i].value && first.text[i].x == second.text[i].x &&
              first.text[i].y == second.text[i].y,
          "consumption changed unrelated label/detail or placement");
  }
}
TripState supplied(std::optional<double> consumption) {
  TripState value;
  value.identifier = *parse_trip_identifier("currentTrip");
  value.reset_generation = 1;
  value.distance_km = 60.;
  value.elapsed_seconds = 3600.;
  value.moving_seconds = 3600.;
  value.average_speed_kmh = 60.;
  value.average_speed_basis = TripAverageSpeedBasis::moving;
  value.fuel_efficiency_l_per_100km = consumption;
  return value;
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid consumption configuration/observation admitted");
}
}  // namespace

int main() try {
  using namespace headunit;
  using namespace std::chrono_literals;
  const auto now = Clock::time_point{10s};
  const std::string legacy =
      "version=1\nspeed_unit=kmh\nspeed_maximum_kmh=260\nrpm_maximum=8000\nrpm_redline=6500\nshow_"
      "rpm=true\n";
  const auto implicit = parse_native_cluster_configuration(legacy);
  const auto metric =
      parse_native_cluster_configuration(legacy + "fuel_consumption_unit=liters_per_100km\n");
  check(implicit.fuel_consumption_unit == NativeFuelConsumptionUnit::liters_per_100km,
        "old config lost independent metric consumption default");
  auto invalid_config = metric;
  invalid_config.fuel_consumption_unit = static_cast<NativeFuelConsumptionUnit>(99);
  check(!valid_native_cluster_configuration(invalid_config),
        "invalid C++ consumption enum admitted");
  for (const auto unit : {"liters_per_100km", "us_mpg", "imperial_mpg"}) {
    const auto full = legacy +
                      "temperature_unit=fahrenheit\ndistance_unit=miles\ndrive_mode_profiles_file=/"
                      "private/profile\nfuel_consumption_unit=" +
                      unit + "\n";
    const auto config =
        parse_native_cluster_configuration(full + std::string(4096 - full.size(), '\n'));
    check(config.temperature_unit == NativeTemperatureUnit::fahrenheit &&
              config.distance_unit == NativeDistanceUnit::miles &&
              config.drive_mode_profiles_file == "/private/profile",
          "ten-field config lost accepted optional chain");
    rejects([&] {
      (void)parse_native_cluster_configuration(full + std::string(4097 - full.size(), '\n'));
    });
    rejects([&] { (void)parse_native_cluster_configuration(full + "unknown=1\n"); });
    rejects(
        [&] { (void)parse_native_cluster_configuration(full + "fuel_consumption_unit=us_mpg\n"); });
  }
  for (const auto bad : {"mpg", "USmpg", "", "0", "litres_per_100km"})
    rejects([&] {
      (void)parse_native_cluster_configuration(legacy + "fuel_consumption_unit=" + bad + "\n");
    });

  auto sample = SimulatedVehicle(now).sample(now);
  sample.trip = supplied(7.4);
  sample.indicators.fill(IndicatorState::off);
  sample.indicator(Indicator::low_fuel) = IndicatorState::on;
  rejects([&] { (void)draw(sample, now, ClusterCenter::trip, invalid_config); });
  for (const auto page : pages) {
    const auto original = draw(sample, now, page, implicit);
    check(original.image.pixels == draw(sample, now, page, metric).image.pixels,
          "explicit default consumption changed pixels");
    for (const auto fuel :
         {NativeFuelConsumptionUnit::us_mpg, NativeFuelConsumptionUnit::imperial_mpg}) {
      auto converted = metric;
      converted.fuel_consumption_unit = fuel;
      only_consumption_text(original, draw(sample, now, page, converted),
                            page == ClusterCenter::trip);
    }
  }
  for (const bool mph : {false, true})
    for (const auto temperature :
         {NativeTemperatureUnit::celsius, NativeTemperatureUnit::fahrenheit})
      for (const auto distance : {NativeDistanceUnit::kilometers, NativeDistanceUnit::miles}) {
        auto config = metric;
        config.miles_per_hour = mph;
        config.temperature_unit = temperature;
        config.distance_unit = distance;
        const auto original = draw(sample, now, ClusterCenter::trip, config);
        for (const auto fuel :
             {NativeFuelConsumptionUnit::liters_per_100km, NativeFuelConsumptionUnit::us_mpg,
              NativeFuelConsumptionUnit::imperial_mpg}) {
          config.fuel_consumption_unit = fuel;
          const auto frame = draw(sample, now, ClusterCenter::trip, config);
          card(frame, "CONSUMPTION",
               fuel == NativeFuelConsumptionUnit::liters_per_100km ? "7.4"
               : fuel == NativeFuelConsumptionUnit::us_mpg         ? "31.8"
                                                                   : "38.2",
               fuel == NativeFuelConsumptionUnit::liters_per_100km ? "L/100 km"
               : fuel == NativeFuelConsumptionUnit::us_mpg         ? "mpg US"
                                                                   : "mpg Imperial",
               "Whole-trip mean");
          card(frame, "AVERAGE SPEED", mph ? "37.3" : "60.0", mph ? "mph" : "km/h",
               "Moving time basis");
          card(frame, "DISTANCE", distance == NativeDistanceUnit::miles ? "37.3" : "60.0",
               distance == NativeDistanceUnit::miles ? "mi" : "km");
          only_consumption_text(original, frame,
                                fuel != NativeFuelConsumptionUnit::liters_per_100km);
        }
      }
  struct Known {
    double consumption;
    const char* us;
    const char* imperial;
  };
  // Independent rounded reference values, including finite values beyond double MPG range.
  for (const auto& known :
       std::array{Known{8, "29.4", "35.3"}, Known{1, "235.2", "282.5"}, Known{200, "1.2", "1.4"},
                  Known{0, "∞", "∞"}, Known{1e-6, "2.35e+08", "2.82e+08"},
                  Known{1e-306, "2.35e+308", "2.82e+308"},
                  Known{std::numeric_limits<double>::denorm_min(), "4.76e+325", "5.72e+325"}}) {
    sample.trip = supplied(known.consumption);
    for (const auto fuel :
         {NativeFuelConsumptionUnit::us_mpg, NativeFuelConsumptionUnit::imperial_mpg}) {
      auto config = metric;
      config.fuel_consumption_unit = fuel;
      card(draw(sample, now, ClusterCenter::trip, config), "CONSUMPTION",
           fuel == NativeFuelConsumptionUnit::us_mpg ? known.us : known.imperial,
           fuel == NativeFuelConsumptionUnit::us_mpg ? "mpg US" : "mpg Imperial",
           "Whole-trip mean");
      check(sample.trip->fuel_efficiency_l_per_100km == known.consumption,
            "display conversion clamped or changed canonical consumption");
    }
    if (known.consumption == 0)
      card(draw(sample, now, ClusterCenter::trip, metric), "CONSUMPTION", "0.0", "L/100 km",
           "Whole-trip mean");
  }
  for (const auto absent :
       {std::optional<double>{}, std::optional<double>{-1.}, std::optional<double>{200.01},
        std::optional<double>{std::numeric_limits<double>::quiet_NaN()},
        std::optional<double>{std::numeric_limits<double>::infinity()}}) {
    sample.trip = supplied(absent);
    for (const auto fuel :
         {NativeFuelConsumptionUnit::liters_per_100km, NativeFuelConsumptionUnit::us_mpg,
          NativeFuelConsumptionUnit::imperial_mpg}) {
      auto config = metric;
      config.fuel_consumption_unit = fuel;
      card(draw(sample, now, ClusterCenter::trip, config), "CONSUMPTION", "--",
           fuel == NativeFuelConsumptionUnit::liters_per_100km ? "L/100 km"
           : fuel == NativeFuelConsumptionUnit::us_mpg         ? "mpg US"
                                                               : "mpg Imperial",
           "Whole-trip mean");
    }
  }
  VehicleAgePolicy ages;
  ages.trip = 200ms;
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, now, now);
  const auto trip = supplied(8);
  const std::array observations{
      VehicleObservation{"trip", trip, now, VehicleObservationOrigin::calculated}};
  cache.accept(7, 1, observations, now);
  const auto held = cache.sample(now);
  auto config = metric;
  config.fuel_consumption_unit = NativeFuelConsumptionUnit::us_mpg;
  card(draw(held, now + 199ms, ClusterCenter::trip, config), "CONSUMPTION", "29.4", "mpg US",
       "Whole-trip mean");
  card(draw(held, now + 200ms, ClusterCenter::trip, config), "CONSUMPTION", "--", "mpg US",
       "Whole-trip mean");
  const std::array speed{VehicleObservation{"speed_kmh", 80., now + 50ms}};
  cache.accept(7, 2, speed, now + 50ms);
  const auto repeated = cache.sample(now + 60ms);
  check(repeated.trip == held.trip && repeated.trip == trip &&
            repeated.observation_timing->trip.acquired_at == now &&
            repeated.observation_timing->trip.maximum_age == 200ms &&
            repeated.observation_timing->trip.origin == VehicleObservationOrigin::calculated &&
            repeated.observation_timing->generation == 7,
        "speed/poll/conversion refreshed canonical trip/date/age/origin");
  auto bad = trip;
  bad.fuel_efficiency_l_per_100km = -1.;
  rejects([&] {
    const std::array batch{VehicleObservation{"trip", bad, now + 61ms}};
    cache.accept(7, 3, batch, now + 61ms);
  });
  cache.withdraw(now + 70ms);
  const auto withdrawn = cache.sample(now + 70ms);
  card(draw(withdrawn, now + 70ms, ClusterCenter::trip, config), "CONSUMPTION", "--", "mpg US",
       "Whole-trip mean");
  check(withdrawn.observation_timing->trip.acquired_at == now &&
            withdrawn.observation_timing->trip.availability == VehicleAvailability::disconnected,
        "withdrawal erased original acquisition");
  cache.accept(7, 3, observations, now + 71ms);
  card(draw(cache.sample(now + 71ms), now + 71ms, ClusterCenter::trip, config), "CONSUMPTION", "--",
       "mpg US", "Whole-trip mean");
  cache.begin_generation(8, now + 80ms, now + 80ms);
  const auto successor = cache.sample(now + 80ms);
  card(draw(successor, now + 80ms, ClusterCenter::trip, config), "CONSUMPTION", "--", "mpg US",
       "Whole-trip mean");
  check(!successor.trip && !successor.observation_timing->trip.acquired_at,
        "successor inherited retired consumption");
  std::cout << "Production Cairo consumption units, bounded finite/zero text, unchanged unrelated "
               "pixels and original source retirement passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
