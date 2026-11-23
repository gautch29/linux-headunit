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
void only_temperature_text_changes(const Frame& celsius, const Frame& fahrenheit) {
  std::size_t changed = 0;
  for (unsigned y = 0; y < celsius.image.height; ++y)
    for (unsigned x = 0; x < celsius.image.width; ++x) {
      const auto index = std::size_t(y) * celsius.image.width + x;
      if (celsius.image.pixels[index] == fahrenheit.image.pixels[index]) continue;
      check(temperature_ink(celsius, x, y) || temperature_ink(fahrenheit, x, y),
            "temperature units changed arc, lamp, layout or other artwork");
      ++changed;
    }
  check(changed > 0, "opt-in temperature units did not change pixels");
}
}  // namespace

int main() try {
  using namespace headunit;
  const auto now = Clock::time_point{std::chrono::seconds(10)};
  SimulatedVehicle source(now);
  auto sample = source.sample(now);
  sample.indicators.fill(IndicatorState::off);
  sample.indicator(Indicator::coolant_warning) = IndicatorState::on;
  NativeClusterConfiguration celsius, fahrenheit;
  fahrenheit.temperature_unit = NativeTemperatureUnit::fahrenheit;
  const std::string legacy =
      "version=1\nspeed_unit=kmh\nspeed_maximum_kmh=260\nrpm_maximum=8000\nrpm_redline=6500\nshow_"
      "rpm=true\n";
  const auto implicit = parse_native_cluster_configuration(legacy);
  const auto explicit_celsius =
      parse_native_cluster_configuration(legacy + "temperature_unit=celsius\n");
  const auto explicit_fahrenheit =
      parse_native_cluster_configuration(legacy + "temperature_unit=fahrenheit\n");
  for (const auto page : pages) {
    sample.coolant_c = 0;
    sample.oil_c = -20;
    const auto old = draw(sample, now, page, celsius);
    check(old.image.pixels == draw(sample, now, page, implicit).image.pixels &&
              old.image.pixels == draw(sample, now, page, explicit_celsius).image.pixels,
          "old six-field configuration or explicit Celsius changed default pixels");
    temperatures(old, page, "0", "-20", "°C");
    const auto converted = draw(sample, now, page, explicit_fahrenheit);
    temperatures(converted, page, "32", "-4", "°F");
    only_temperature_text_changes(old, converted);
  }
  struct Known {
    double coolant, oil;
    const char* coolant_text;
    const char* oil_text;
  };
  // Independent expected values, including canonical bounds, negative values,
  // exact zero, signed rounded zero and the existing printf nearest rounding.
  for (const auto& known : std::array{Known{-50, 200, "-58", "392"}, Known{180, -50, "356", "-58"},
                                      Known{-40, -40, "-40", "-40"}, Known{-18, -17.5, "-0", "0"},
                                      Known{20.25, 20.75, "68", "69"}, Known{90, 100, "194", "212"},
                                      Known{-0., 0., "32", "32"}}) {
    sample.coolant_c = known.coolant;
    sample.oil_c = known.oil;
    for (const auto page : {ClusterCenter::vehicle, ClusterCenter::gauges}) {
      const auto c = draw(sample, now, page, celsius);
      const auto f = draw(sample, now, page, fahrenheit);
      temperatures(f, page, known.coolant_text, known.oil_text, "°F");
      only_temperature_text_changes(c, f);
    }
  }
  for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(), -50.01, 180.01}) {
    sample.coolant_c = invalid;
    sample.oil_c = 100;
    temperatures(draw(sample, now, ClusterCenter::gauges, fahrenheit), ClusterCenter::gauges, "--",
                 "--", "°F");
  }
  sample.coolant_c.reset();
  sample.oil_c = 0;
  temperatures(draw(sample, now, ClusterCenter::vehicle, fahrenheit), ClusterCenter::vehicle, "--",
               "32", "°F");
  sample.sampled_at = now - std::chrono::seconds(1);
  temperatures(draw(sample, now, ClusterCenter::gauges, fahrenheit), ClusterCenter::gauges, "--",
               "--", "°F");

  VehicleAgePolicy ages;
  ages.readings[3] = std::chrono::milliseconds(100);
  ages.readings[4] = std::chrono::milliseconds(200);
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, now, now);
  const std::array initial{
      VehicleObservation{"coolant_c", 90., now, VehicleObservationOrigin::simulation},
      VehicleObservation{"oil_c", 100., now, VehicleObservationOrigin::calculated}};
  cache.accept(7, 1, initial, now);
  const auto held = cache.sample(now);
  const auto original = held.observation_timing;
  for (const auto page : pages) {
    temperatures(draw(held, now + std::chrono::milliseconds(99), page, fahrenheit), page, "194",
                 "212", "°F");
    temperatures(draw(held, now + std::chrono::milliseconds(100), page, fahrenheit), page, "--",
                 "212", "°F");
    temperatures(draw(held, now + std::chrono::milliseconds(200), page, fahrenheit), page, "--",
                 "--", "°F");
  }
  check(held.coolant_c == 90 && held.oil_c == 100 &&
            held.observation_timing->readings[3].acquired_at == original->readings[3].acquired_at &&
            held.observation_timing->readings[4].origin == VehicleObservationOrigin::calculated,
        "display conversion mutated canonical readings or acquisition provenance");
  cache.withdraw(now + std::chrono::milliseconds(50));
  temperatures(draw(cache.sample(now + std::chrono::milliseconds(50)),
                    now + std::chrono::milliseconds(50), ClusterCenter::gauges, fahrenheit),
               ClusterCenter::gauges, "--", "--", "°F");
  cache.begin_generation(8, now + std::chrono::milliseconds(60),
                         now + std::chrono::milliseconds(60));
  temperatures(draw(cache.sample(now + std::chrono::milliseconds(60)),
                    now + std::chrono::milliseconds(60), ClusterCenter::gauges, fahrenheit),
               ClusterCenter::gauges, "--", "--", "°F");
  std::cout << "Real Cairo temperature text/units, bounds, canonical arcs, expiry and retirement "
               "passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
