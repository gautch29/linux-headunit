// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster.hpp>
#include <headunit/vehicle_observation.hpp>
#include <cairo.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
struct TextDraw {
  std::string value;
  double x, y, size;
  cairo_text_extents_t ink;
};
std::vector<TextDraw> text_draws;
void observed_show_text(cairo_t* context, const char* value) {
  double x = 0, y = 0;
  cairo_get_current_point(context, &x, &y);
  cairo_text_extents_t ink{};
  cairo_text_extents(context, value, &ink);
  cairo_matrix_t matrix{};
  cairo_get_font_matrix(context, &matrix);
  text_draws.push_back({value, x, y, matrix.xx, ink});
  cairo_show_text(context, value);
}
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid pressure configuration/observation admitted");
}
}  // namespace

// Observe only the final text call in the actual source. Every full frame must
// match the independently linked production renderer; no production test seam.
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
constexpr std::array units{NativePressureUnit::kpa, NativePressureUnit::bar,
                           NativePressureUnit::psi};
struct Frame {
  Image image;
  std::vector<TextDraw> text;
};
Frame draw(const VehicleSample& sample, Clock::time_point now, ClusterCenter page,
           const NativeClusterConfiguration& config) {
  text_draws.clear();
  auto image = observed_render_native_cluster(sample, now, 1920, 720, {}, {}, page, false, config);
  check(image.pixels ==
            render_native_cluster(sample, now, 1920, 720, {}, {}, page, false, config).pixels,
        "pressure text observation changed production pixels");
  return {std::move(image), text_draws};
}
std::size_t pressure(const std::vector<TextDraw>& text, ClusterCenter page,
                     const std::string& number, const std::string& unit) {
  std::size_t found = text.size();
  for (std::size_t i = 1; i < text.size(); ++i) {
    if (text[i].value != "kPa" && text[i].value != "bar" && text[i].value != "psi") continue;
    check(found == text.size(), "extra pressure allocation");
    found = i;
    const auto& value = text[i - 1];
    const auto& label = text[i];
    check(value.value == number && label.value == unit, "wrong pressure Cairo number or unit");
    const double left = std::min(value.x + value.ink.x_bearing, label.x + label.ink.x_bearing);
    const double right = std::max(value.x + value.ink.x_bearing + value.ink.width,
                                  label.x + label.ink.x_bearing + label.ink.width);
    if (page == ClusterCenter::vehicle) {
      check(i >= 2 && text[i - 2].value == "OIL PRESSURE", "wrong Vehicle card");
      check(left >= 576 && right <= 560 + 752. / 3. - 16,
            "pressure escaped original Vehicle card clip");
      check(value.size == 42 && label.size == 19, "pressure required a different card font size");
    } else {
      check(page == ClusterCenter::gauges && i + 1 < text.size() &&
                text[i + 1].value == "OIL PRESSURE",
            "wrong Full Gauges dial");
      check(left >= 628 && right <= 756 && value.y + value.ink.y_bearing >= 342 &&
                label.y + label.ink.y_bearing + label.ink.height <= 470,
            "pressure escaped original dial interior");
      check(value.size == 32 && label.size == 16, "pressure changed original dial font size");
    }
  }
  const bool present = page == ClusterCenter::vehicle || page == ClusterCenter::gauges;
  check((found < text.size()) == present, "pressure allocation missing or added to another page");
  return found;
}
bool pressure_ink(const Frame& frame, unsigned x, unsigned y) {
  for (std::size_t i = 1; i < frame.text.size(); ++i) {
    if (frame.text[i].value != "kPa" && frame.text[i].value != "bar" &&
        frame.text[i].value != "psi")
      continue;
    for (const auto j : {i - 1, i}) {
      const auto& t = frame.text[j];
      if (double(x) >= std::floor(t.x + t.ink.x_bearing) - 1 &&
          double(x) <= std::ceil(t.x + t.ink.x_bearing + t.ink.width) + 1 &&
          double(y) >= std::floor(t.y + t.ink.y_bearing) - 1 &&
          double(y) <= std::ceil(t.y + t.ink.y_bearing + t.ink.height) + 1)
        return true;
    }
  }
  return false;
}
void only_pressure_text(const Frame& first, const Frame& second, bool expected_change) {
  check(first.text.size() == second.text.size(), "pressure changed text allocations");
  std::size_t changed = 0;
  for (unsigned y = 0; y < first.image.height; ++y)
    for (unsigned x = 0; x < first.image.width; ++x) {
      const auto index = std::size_t(y) * first.image.width + x;
      if (first.image.pixels[index] == second.image.pixels[index]) continue;
      check(pressure_ink(first, x, y) || pressure_ink(second, x, y),
            "pressure changed canonical arcs, geometry, lamps or unrelated pixels");
      ++changed;
    }
  check((changed > 0) == expected_change, "unexpected pressure pixel identity");
  for (std::size_t i = 0; i < first.text.size(); ++i) {
    const auto& a = first.text[i];
    const auto& b = second.text[i];
    if (a.value == "kPa" || a.value == "bar" || a.value == "psi") continue;
    if (i + 1 < first.text.size() &&
        (first.text[i + 1].value == "kPa" || first.text[i + 1].value == "bar" ||
         first.text[i + 1].value == "psi"))
      continue;
    check(a.value == b.value && a.x == b.x && a.y == b.y && a.size == b.size,
          "pressure changed other text or its placement/font");
  }
}
void save(const std::filesystem::path& path, const Image& image) {
  std::ofstream output(path, std::ios::binary);
  write_ppm(image, output);
  output.close();
  check(bool(output), "production pressure image retention failed");
}
void exhaustive_strings() {
  std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 720), cairo_surface_destroy);
  std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()),
                                                             cairo_destroy);
  std::size_t count = 0;
  for (const auto unit : units) {
    NativeClusterConfiguration config;
    config.pressure_unit = unit;
    const unsigned maximum = unit == NativePressureUnit::kpa   ? 1500
                             : unit == NativePressureUnit::bar ? 150
                                                               : 218;
    for (unsigned n = 0; n <= maximum; ++n) {
      VehicleSample sample;
      // Independent exact rational psi definition, not the production expression.
      const double kpa = unit == NativePressureUnit::kpa ? double(n)
                         : unit == NativePressureUnit::bar
                             ? double(n) * 10.
                             : std::min(1500., double(n) * (8896443230521. / 1290320000000.));
      sample.oil_pressure_kpa = kpa;
      check(valid_vehicle_sample(sample), "pressure enumerator exceeded source bounds");
      const auto expected = unit == NativePressureUnit::bar
                                ? std::to_string(n / 10) + "." + std::to_string(n % 10)
                                : std::to_string(n);
      for (const auto page : {ClusterCenter::vehicle, ClusterCenter::gauges}) {
        text_draws.clear();
        if (page == ClusterCenter::vehicle)
          vehicle_page(context.get(), sample, true, config);
        else
          gauges_format(context.get(), sample, true, config);
        pressure(text_draws, page, expected, pressure_label(config));
        ++count;
      }
    }
  }
  check(cairo_status(context.get()) == CAIRO_STATUS_SUCCESS, "actual pressure Cairo caller failed");
  std::cout << "EXHAUSTIVE production Vehicle/Full Gauges pressure draws=" << count << '\n';
}
}  // namespace

int main(int argc, char** argv) try {
  using namespace headunit;
  using namespace std::chrono_literals;
  check(argc == 2, "missing pressure image directory");
  const std::filesystem::path images(argv[1]);
  std::filesystem::create_directories(images);
  const auto now = Clock::time_point{10s};
  const std::string legacy =
      "version=1\nspeed_unit=kmh\nspeed_maximum_kmh=260\nrpm_maximum=8000\nrpm_redline=6500\nshow_"
      "rpm=true\n";
  const auto implicit = parse_native_cluster_configuration(legacy);
  check(implicit.pressure_unit == NativePressureUnit::kpa, "legacy pressure default changed");
  for (const auto& [name, unit] :
       {std::pair{"kpa", NativePressureUnit::kpa}, std::pair{"bar", NativePressureUnit::bar},
        std::pair{"psi", NativePressureUnit::psi}}) {
    const auto full =
        legacy +
        "temperature_unit=fahrenheit\ndistance_unit=miles\nfuel_consumption_unit=us_mpg\nspeed_"
        "precision=one_decimal\ndrive_mode_profiles_file=/private/profile\npressure_unit=" +
        name + "\n";
    const auto parsed =
        parse_native_cluster_configuration(full + std::string(4096 - full.size(), '\n'));
    check(parsed.pressure_unit == unit &&
              parsed.temperature_unit == NativeTemperatureUnit::fahrenheit &&
              parsed.distance_unit == NativeDistanceUnit::miles &&
              parsed.fuel_consumption_unit == NativeFuelConsumptionUnit::us_mpg &&
              parsed.speed_precision == NativeSpeedPrecision::one_decimal &&
              parsed.drive_mode_profiles_file == "/private/profile",
          "twelve fields displaced accepted configuration");
    rejects([&] {
      (void)parse_native_cluster_configuration(full + std::string(4097 - full.size(), '\n'));
    });
    rejects([&] { (void)parse_native_cluster_configuration(full + "unknown=1\n"); });
    rejects([&] { (void)parse_native_cluster_configuration(full + "pressure_unit=kpa\n"); });
    for (const auto& line : {"version=1\n", "speed_unit=kmh\n", "speed_maximum_kmh=260\n",
                             "rpm_maximum=8000\n", "rpm_redline=6500\n", "show_rpm=true\n"}) {
      auto missing = full;
      missing.erase(missing.find(line), std::string_view(line).size());
      rejects([&] { (void)parse_native_cluster_configuration(missing); });
    }
  }
  for (const auto bad : {"", "kPa", "Bar", "PSI", "0", "true", "psi ", "pascals"})
    rejects(
        [&] { (void)parse_native_cluster_configuration(legacy + "pressure_unit=" + bad + "\n"); });
  auto sample = SimulatedVehicle(now).sample(now);
  sample.indicators.fill(IndicatorState::off);
  sample.indicator(Indicator::oil_pressure) = IndicatorState::on;
  auto invalid = implicit;
  invalid.pressure_unit = static_cast<NativePressureUnit>(99);
  check(!valid_native_cluster_configuration(invalid), "invalid C++ pressure enum admitted");
  rejects([&] { (void)draw(sample, now, ClusterCenter::vehicle, invalid); });
  struct Known {
    double kpa;
    const char* metric;
    const char* bar;
    const char* psi;
  };
  const std::array references{Known{0, "0", "0.0", "0"},
                              Known{100, "100", "1.0", "15"},
                              Known{350, "350", "3.5", "51"},
                              Known{1000, "1000", "10.0", "145"},
                              Known{1500, "1500", "15.0", "218"},
                              Known{6.894757293168361, "7", "0.1", "1"},
                              Known{-0., "-0", "-0.0", "-0"},
                              Known{std::numeric_limits<double>::denorm_min(), "0", "0.0", "0"}};
  for (const auto& known : references) {
    sample.oil_pressure_kpa = known.kpa;
    for (const auto page : pages) {
      const auto original = draw(sample, now, page, implicit);
      for (const auto unit : units) {
        auto config = implicit;
        config.pressure_unit = unit;
        const auto frame = draw(sample, now, page, config);
        pressure(frame.text, page,
                 unit == NativePressureUnit::kpa   ? known.metric
                 : unit == NativePressureUnit::bar ? known.bar
                                                   : known.psi,
                 pressure_label(config));
        only_pressure_text(original, frame,
                           unit != NativePressureUnit::kpa &&
                               (page == ClusterCenter::vehicle || page == ClusterCenter::gauges));
        if (known.kpa == 0 || known.kpa == 1500) {
          const auto sign = std::signbit(known.kpa) ? "negative-zero"
                            : known.kpa == 0        ? "zero"
                                                    : "maximum";
          save(images / (std::to_string(static_cast<int>(page)) + "-" + pressure_label(config) +
                         "-" + sign + ".ppm"),
               frame.image);
        }
      }
    }
  }
  sample.oil_pressure_kpa = 350.;
  for (const bool mph : {false, true})
    for (const auto temperature :
         {NativeTemperatureUnit::celsius, NativeTemperatureUnit::fahrenheit})
      for (const auto distance : {NativeDistanceUnit::kilometers, NativeDistanceUnit::miles})
        for (const auto fuel :
             {NativeFuelConsumptionUnit::liters_per_100km, NativeFuelConsumptionUnit::us_mpg,
              NativeFuelConsumptionUnit::imperial_mpg})
          for (const auto precision :
               {NativeSpeedPrecision::whole, NativeSpeedPrecision::one_decimal}) {
            auto config = implicit;
            config.miles_per_hour = mph;
            config.temperature_unit = temperature;
            config.distance_unit = distance;
            config.fuel_consumption_unit = fuel;
            config.speed_precision = precision;
            for (const auto page : {ClusterCenter::vehicle, ClusterCenter::gauges}) {
              const auto original = draw(sample, now, page, config);
              for (const auto unit : units) {
                config.pressure_unit = unit;
                const auto frame = draw(sample, now, page, config);
                pressure(frame.text, page,
                         unit == NativePressureUnit::kpa   ? "350"
                         : unit == NativePressureUnit::bar ? "3.5"
                                                           : "51",
                         pressure_label(config));
                only_pressure_text(original, frame, unit != NativePressureUnit::kpa);
              }
              config.pressure_unit = NativePressureUnit::kpa;
            }
          }
  for (const auto absent :
       {std::optional<double>{}, std::optional<double>{-1}, std::optional<double>{1500.01},
        std::optional<double>{std::numeric_limits<double>::quiet_NaN()},
        std::optional<double>{std::numeric_limits<double>::infinity()}}) {
    sample.oil_pressure_kpa = absent;
    for (const auto page : pages)
      for (const auto unit : units) {
        auto config = implicit;
        config.pressure_unit = unit;
        pressure(draw(sample, now, page, config).text, page, "--", pressure_label(config));
      }
  }
  VehicleAgePolicy ages;
  ages.readings[5] = 200ms;
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, now, now);
  const std::array batch{
      VehicleObservation{"oil_pressure_kpa", 350., now, VehicleObservationOrigin::calculated}};
  cache.accept(7, 1, batch, now);
  const auto held = cache.sample(now);
  const auto verify = [&](const VehicleSample& value, Clock::time_point at, bool available) {
    for (const auto page : {ClusterCenter::vehicle, ClusterCenter::gauges})
      for (const auto unit : units) {
        auto config = implicit;
        config.pressure_unit = unit;
        pressure(draw(value, at, page, config).text, page,
                 !available                        ? "--"
                 : unit == NativePressureUnit::kpa ? "350"
                 : unit == NativePressureUnit::bar ? "3.5"
                                                   : "51",
                 pressure_label(config));
      }
  };
  verify(held, now + 199ms, true);
  verify(held, now + 200ms, false);
  const std::array speed{VehicleObservation{"speed_kmh", 80., now + 50ms}};
  cache.accept(7, 2, speed, now + 50ms);
  const auto repeated = cache.sample(now + 60ms);
  const auto& timing = repeated.observation_timing->readings[5];
  check(repeated.oil_pressure_kpa == held.oil_pressure_kpa && timing.acquired_at == now &&
            timing.maximum_age == 200ms && timing.origin == VehicleObservationOrigin::calculated &&
            repeated.observation_timing->generation == 7,
        "display/speed/poll changed canonical pressure/date/age/origin/generation");
  for (const double bad : {-1., 1500.01, std::numeric_limits<double>::infinity()})
    rejects([&] {
      const std::array invalid_batch{VehicleObservation{"oil_pressure_kpa", bad, now + 61ms}};
      cache.accept(7, 3, invalid_batch, now + 61ms);
    });
  cache.withdraw(now + 70ms);
  const auto withdrawn = cache.sample(now + 70ms);
  verify(withdrawn, now + 70ms, false);
  check(withdrawn.observation_timing->readings[5].acquired_at == now &&
            withdrawn.observation_timing->readings[5].availability ==
                VehicleAvailability::disconnected,
        "withdrawal erased original pressure acquisition");
  cache.accept(7, 3, batch, now + 71ms);
  verify(cache.sample(now + 71ms), now + 71ms, false);
  cache.begin_generation(8, now + 80ms, now + 80ms);
  verify(cache.sample(now + 80ms), now + 80ms, false);
  const std::array successor{VehicleObservation{"oil_pressure_kpa", 350., now + 81ms}};
  cache.accept(8, 1, successor, now + 81ms);
  verify(cache.sample(now + 81ms), now + 81ms, true);
  exhaustive_strings();
  std::cout
      << "Pressure production Cairo math/fit/independent units/unknown/expiry/retirement passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
