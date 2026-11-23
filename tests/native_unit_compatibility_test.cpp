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
        "text observation changed real production Cairo pixels");
  return {image, text_draws};
}
void temperatures(const Frame& frame, ClusterCenter page, const char* coolant, const char* oil,
                  const char* unit) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < frame.text.size(); ++i) {
    const auto& label = frame.text[i];
    if (label.value != "°C" && label.value != "°F") continue;
    check(i > 0 && label.value == unit, "wrong temperature label at real Cairo draw");
    const auto& number = frame.text[i - 1];
    check(number.value == (count % 2 == 0 ? coolant : oil), "wrong displayed temperature number");
    const double left = std::min(number.x + number.ink.x_bearing, label.x + label.ink.x_bearing);
    const double right = std::max(number.x + number.ink.x_bearing + number.ink.width,
                                  label.x + label.ink.x_bearing + label.ink.width);
    if (label.y == 604) {
      const double column = count % 2 == 0 ? 1440 : 1580;
      check(left >= column && right < column + 140, "temperature overflowed bottom allocation");
    } else if (page == ClusterCenter::gauges) {
      const double center = count % 2 == 0 ? 960 : 1228;
      check(left > center - 64 && right < center + 64,
            "temperature overflowed existing dial interior");
    } else {
      check(page == ClusterCenter::vehicle && left >= 834 && right < 1360,
            "temperature overflowed existing vehicle cards");
    }
    ++count;
  }
  check(count == (page == ClusterCenter::vehicle || page == ClusterCenter::gauges ? 4 : 2),
        "native format lost an existing temperature allocation");
}
bool temperature_ink(const Frame& frame, unsigned x, unsigned y) {
  for (std::size_t i = 1; i < frame.text.size(); ++i) {
    if (frame.text[i].value != "°C" && frame.text[i].value != "°F") continue;
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
void only_combined_text(const Frame& first, const Frame& second) {
  std::size_t changed = 0;
  for (unsigned y = 0; y < first.image.height; ++y) {
    for (unsigned x = 0; x < first.image.width; ++x) {
      const auto index = std::size_t(y) * first.image.width + x;
      if (first.image.pixels[index] == second.image.pixels[index]) continue;
      check(temperature_ink(first, x, y) || temperature_ink(second, x, y) ||
                target_ink(first, x, y) || target_ink(second, x, y),
            "combined units changed canonical arcs, lamps or unrelated pixels");
      ++changed;
    }
  }
  check(changed > 0, "combined units did not change their existing text");
}
}  // namespace

int main() try {
  using namespace headunit;
  using namespace std::chrono_literals;
  const auto now = Clock::time_point{10s};
  VehicleAgePolicy ages;
  ages.readings[3] = 100ms;
  ages.readings[4] = 200ms;
  ages.readings[8] = 300ms;
  ages.trip = 400ms;
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, now, now);
  const auto trip = supplied(60, 60);
  const std::array readings{
      VehicleObservation{"coolant_c", 90., now, VehicleObservationOrigin::simulation},
      VehicleObservation{"oil_c", 100., now, VehicleObservationOrigin::calculated},
      VehicleObservation{"range_km", 60., now, VehicleObservationOrigin::calculated},
      VehicleObservation{"trip", trip, now, VehicleObservationOrigin::simulation},
      VehicleObservation{"low_fuel", IndicatorState::on, now}};
  cache.accept(7, 1, readings, now);
  const auto held = cache.sample(now);
  NativeClusterConfiguration metric, combined;
  metric.miles_per_hour = true;
  combined = metric;
  combined.temperature_unit = NativeTemperatureUnit::fahrenheit;
  combined.distance_unit = NativeDistanceUnit::miles;
  for (const auto page : pages) {
    const auto original = draw(held, now, page, metric);
    const auto converted = draw(held, now, page, combined);
    temperatures(converted, page, "194", "212", "°F");
    distance(converted, page, page == ClusterCenter::trip ? "37.3" : "37", "mi");
    only_combined_text(original, converted);
  }
  card(draw(held, now, ClusterCenter::trip, combined), "AVERAGE SPEED", "37.3", "mph",
       "Total time basis");
  temperatures(draw(held, now + 100ms, ClusterCenter::gauges, combined), ClusterCenter::gauges,
               "--", "212", "°F");
  distance(draw(held, now + 299ms, ClusterCenter::gauges, combined), ClusterCenter::gauges, "37",
           "mi");
  distance(draw(held, now + 300ms, ClusterCenter::gauges, combined), ClusterCenter::gauges, "--",
           "mi");
  card(draw(held, now + 399ms, ClusterCenter::trip, combined), "AVERAGE SPEED", "37.3", "mph",
       "Total time basis");
  distance(draw(held, now + 400ms, ClusterCenter::trip, combined), ClusterCenter::trip, "--", "mi");
  check(held.coolant_c == 90 && held.oil_c == 100 && held.range_km == 60 && held.trip == trip &&
            held.observation_timing->readings[3].acquired_at == now &&
            held.observation_timing->readings[4].origin == VehicleObservationOrigin::calculated &&
            held.observation_timing->trip.acquired_at == now,
        "combined rendering changed canonical values or original acquisition");
  cache.withdraw(now + 50ms);
  const auto withdrawn = cache.sample(now + 50ms);
  temperatures(draw(withdrawn, now + 50ms, ClusterCenter::gauges, combined), ClusterCenter::gauges,
               "--", "--", "°F");
  distance(draw(withdrawn, now + 50ms, ClusterCenter::gauges, combined), ClusterCenter::gauges,
           "--", "mi");
  cache.accept(7, 2, readings, now + 51ms);
  distance(draw(cache.sample(now + 51ms), now + 51ms, ClusterCenter::trip, combined),
           ClusterCenter::trip, "--", "mi");
  cache.begin_generation(8, now + 60ms, now + 60ms);
  const auto successor = cache.sample(now + 60ms);
  temperatures(draw(successor, now + 60ms, ClusterCenter::vehicle, combined),
               ClusterCenter::vehicle, "--", "--", "°F");
  distance(draw(successor, now + 60ms, ClusterCenter::trip, combined), ClusterCenter::trip, "--",
           "mi");
  check(!successor.coolant_c && !successor.range_km && !successor.trip,
        "combined unit configuration revived a retired reading");
  std::cout << "Combined Fahrenheit/miles real Cairo text/pixels and independent expiry passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
