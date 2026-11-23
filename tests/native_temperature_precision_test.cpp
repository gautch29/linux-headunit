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
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
struct TextDraw {
  std::string value;
  double x, y, size;
  cairo_text_extents_t ink;
};
std::vector<TextDraw> text_draws;
std::array<double, 3> maximum_ink_width{};
void observed_show_text(cairo_t* c, const char* value) {
  double x = 0, y = 0;
  cairo_get_current_point(c, &x, &y);
  cairo_text_extents_t ink{};
  cairo_text_extents(c, value, &ink);
  cairo_matrix_t matrix{};
  cairo_get_font_matrix(c, &matrix);
  text_draws.push_back({value, x, y, matrix.xx, ink});
  cairo_show_text(c, value);
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
  throw std::runtime_error("invalid temperature precision or observation admitted");
}
}  // namespace

// Observe actual final Cairo text calls. Every complete frame is compared with
// the independently linked production renderer. No production hook is added.
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
  auto image = observed_render_native_cluster(sample, now, 1920, 720, {}, {}, page, false, config);
  check(image.pixels ==
            render_native_cluster(sample, now, 1920, 720, {}, {}, page, false, config).pixels,
        "temperature observation changed production pixels");
  return {std::move(image), text_draws};
}
bool label(const TextDraw& t) {
  return t.value == "°C" || t.value == "°F";
}
void temperatures(const std::vector<TextDraw>& text, ClusterCenter page, const std::string& coolant,
                  const std::string& oil, bool fahrenheit, bool component = false) {
  std::size_t count = 0;
  for (std::size_t i = 1; i < text.size(); ++i) {
    const auto& unit = text[i];
    if (!label(unit)) continue;
    const auto& number = text[i - 1];
    if (number.value != (count % 2 == 0 ? coolant : oil))
      std::cerr << "Temperature mismatch page=" << static_cast<int>(page) << " unit=" << unit.value
                << " actual=" << number.value << " expected=" << (count % 2 == 0 ? coolant : oil)
                << '\n';
    check(unit.value == (fahrenheit ? "°F" : "°C") &&
              number.value == (count % 2 == 0 ? coolant : oil),
          "full temperature string or unchanged unit missing from Cairo");
    const double left = std::min(number.x + number.ink.x_bearing, unit.x + unit.ink.x_bearing);
    const double right = std::max(number.x + number.ink.x_bearing + number.ink.width,
                                  unit.x + unit.ink.x_bearing + unit.ink.width);
    const std::size_t site = unit.y == 604 ? 2 : page == ClusterCenter::gauges ? 1 : 0;
    maximum_ink_width[site] = std::max(maximum_ink_width[site], right - left);
    if (unit.y == 604) {
      const double x = count % 2 == 0 ? 1440 : 1580;
      check(left >= x && right <= x + 140 && number.size == 28 && unit.size == 15 &&
                number.y == 604 && number.y + number.ink.y_bearing >= 570 &&
                number.y + number.ink.y_bearing + number.ink.height <= 616,
            "temperature escaped original bottom cell or changed font/baseline");
    } else if (page == ClusterCenter::gauges) {
      const double x = count % 2 == 0 ? 960 : 1228;
      check(left >= x - 64 && right <= x + 64 && number.size == 32 && unit.size == 16 &&
                number.y == 200 && unit.y == 224 && number.y + number.ink.y_bearing >= 128 &&
                unit.y + unit.ink.y_bearing + unit.ink.height <= 256,
            "temperature escaped original dial interior or changed font/baseline");
    } else {
      const double x = 560 + (count % 2 + 1) * (752. / 3. + 24);
      check(page == ClusterCenter::vehicle && left >= x + 16 && right <= x + 752. / 3. - 16 &&
                number.size == 42 && unit.size == 19 && number.y == 276 && unit.y == 276 &&
                number.y + number.ink.y_bearing >= 184 &&
                unit.y + unit.ink.y_bearing + unit.ink.height <= 326,
            "temperature escaped original Vehicle clip or changed font/baseline");
    }
    ++count;
  }
  const bool center = page == ClusterCenter::vehicle || page == ClusterCenter::gauges;
  check(count == (component ? 2 : center ? 4 : 2), "temperature allocation lost or added");
}
bool ink(const TextDraw& t, unsigned x, unsigned y) {
  return double(x) >= std::floor(t.x + t.ink.x_bearing) - 1 &&
         double(x) <= std::ceil(t.x + t.ink.x_bearing + t.ink.width) + 1 &&
         double(y) >= std::floor(t.y + t.ink.y_bearing) - 1 &&
         double(y) <= std::ceil(t.y + t.ink.y_bearing + t.ink.height) + 1;
}
bool temperature_ink(const Frame& frame, unsigned x, unsigned y) {
  for (std::size_t i = 1; i < frame.text.size(); ++i) {
    if (label(frame.text[i]) && (ink(frame.text[i - 1], x, y) || ink(frame.text[i], x, y)))
      return true;
  }
  return false;
}
void only_temperature_text(const Frame& whole, const Frame& decimal) {
  check(whole.text.size() == decimal.text.size(), "precision changed text allocations");
  for (std::size_t i = 0; i < whole.text.size(); ++i) {
    const auto& a = whole.text[i];
    const auto& b = decimal.text[i];
    if (label(a)) {
      check(a.value == b.value && a.y == b.y && a.size == b.size,
            "precision changed temperature label font or baseline");
      continue;
    }
    if (i + 1 < whole.text.size() && label(whole.text[i + 1])) {
      check(a.size == b.size && a.y == b.y, "precision changed temperature numeric font/baseline");
      check(a.size == 32 ? std::abs(a.x + a.ink.x_advance / 2 - b.x - b.ink.x_advance / 2) < .01
                         : a.x == b.x,
            "precision moved original numeric center or left anchor");
      continue;
    }
    check(a.value == b.value && a.x == b.x && a.y == b.y && a.size == b.size,
          "temperature precision changed unrelated text, pressure or speed");
  }
  for (unsigned y = 0; y < whole.image.height; ++y) {
    for (unsigned x = 0; x < whole.image.width; ++x) {
      const auto n = std::size_t(y) * whole.image.width + x;
      if (whole.image.pixels[n] == decimal.image.pixels[n]) continue;
      check(temperature_ink(whole, x, y) || temperature_ink(decimal, x, y),
            "precision changed non-temperature pixels, arc, lamp or geometry");
    }
  }
}
void exhaustive_strings() {
  std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 720), cairo_surface_destroy);
  std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()),
                                                             cairo_destroy);
  std::size_t count = 0;
  for (const bool fahrenheit : {false, true}) {
    NativeClusterConfiguration config;
    config.temperature_precision = NativeTemperaturePrecision::one_decimal;
    config.temperature_unit =
        fahrenheit ? NativeTemperatureUnit::fahrenheit : NativeTemperatureUnit::celsius;
    // Enumerate every possible rounded string in the original finite domains.
    // This is a finite output inventory, not a claim to enumerate binary64 inputs.
    // An exact inverse of 0 F can contract to a negative residual on FMA
    // hosts. An interior positive-zero fixture preserves formatter authority.
    const int minimum = fahrenheit ? -580 : -500;
    const int maximum = fahrenheit ? 3920 : 2000;
    for (int n = minimum; n <= maximum; ++n) {
      const double displayed = double(n) / 10;
      const double source = n == minimum   ? -50.
                            : n == maximum ? 200.
                            : fahrenheit   ? (n == 0 ? -17.76 : (displayed - 32.) / 1.8)
                                           : displayed;
      VehicleSample sample;
      sample.oil_c = source;
      if (source <= 180.) sample.coolant_c = source;
      check(valid_vehicle_sample(sample), "enumerator exceeded original source domain");
      const int magnitude = std::abs(n);
      const auto expected = std::string(n < 0 ? "-" : "") + std::to_string(magnitude / 10) + "." +
                            std::to_string(magnitude % 10);
      for (const auto page :
           {ClusterCenter::vehicle, ClusterCenter::gauges, ClusterCenter::minimal}) {
        text_draws.clear();
        if (page == ClusterCenter::vehicle)
          vehicle_page(context.get(), sample, true, config);
        else if (page == ClusterCenter::gauges)
          gauges_format(context.get(), sample, true, config);
        else
          readings(context.get(), sample, true, config);
        temperatures(text_draws, page, sample.coolant_c ? expected : "--", expected, fahrenheit,
                     true);
        ++count;
      }
    }
    VehicleSample negative_zero;
    negative_zero.coolant_c = fahrenheit ? (-32.001 / 1.8) : -0.;
    negative_zero.oil_c = negative_zero.coolant_c;
    check(valid_vehicle_sample(negative_zero), "signed rounded zero source rejected");
    for (const auto page :
         {ClusterCenter::vehicle, ClusterCenter::gauges, ClusterCenter::minimal}) {
      text_draws.clear();
      if (page == ClusterCenter::vehicle)
        vehicle_page(context.get(), negative_zero, true, config);
      else if (page == ClusterCenter::gauges)
        gauges_format(context.get(), negative_zero, true, config);
      else
        readings(context.get(), negative_zero, true, config);
      temperatures(text_draws, page, "-0.0", "-0.0", fahrenheit, true);
      ++count;
    }
  }
  check(cairo_status(context.get()) == CAIRO_STATUS_SUCCESS, "exhaustive actual Cairo failed");
  std::cout << "EXHAUSTIVE production card/dial/strip decimal draws=" << count << '\n';
}
void save(const std::filesystem::path& path, const Image& image) {
  std::ofstream stream(path, std::ios::binary);
  write_ppm(image, stream);
  stream.close();
  check(bool(stream), "temperature precision image retention failed");
}
void parser() {
  const std::string required =
      "version=1\nspeed_unit=kmh\nspeed_maximum_kmh=260\nrpm_maximum=8000\nrpm_redline=6500\nshow_"
      "rpm=true\n";
  check(parse_native_cluster_configuration(required).temperature_precision ==
            NativeTemperaturePrecision::whole,
        "old configuration lost whole default");
  for (const auto name : {"whole", "one_decimal"}) {
    const auto raw = required +
                     "temperature_unit=fahrenheit\ndistance_unit=miles\nfuel_consumption_unit=us_"
                     "mpg\nspeed_precision=one_decimal\npressure_unit=psi\ndrive_mode_profiles_"
                     "file=/private/profile\ntemperature_precision=" +
                     name + "\n";
    const auto config =
        parse_native_cluster_configuration(raw + std::string(4096 - raw.size(), '\n'));
    check(config.temperature_precision == (std::string_view(name) == "whole"
                                               ? NativeTemperaturePrecision::whole
                                               : NativeTemperaturePrecision::one_decimal) &&
              config.temperature_unit == NativeTemperatureUnit::fahrenheit &&
              config.pressure_unit == NativePressureUnit::psi &&
              config.speed_precision == NativeSpeedPrecision::one_decimal &&
              config.distance_unit == NativeDistanceUnit::miles &&
              config.fuel_consumption_unit == NativeFuelConsumptionUnit::us_mpg &&
              config.drive_mode_profiles_file == "/private/profile",
          "thirteen fields displaced accepted configuration");
    rejects([&] {
      (void)parse_native_cluster_configuration(raw + std::string(4097 - raw.size(), '\n'));
    });
    rejects([&] { (void)parse_native_cluster_configuration(raw + "unknown=1\n"); });
    rejects(
        [&] { (void)parse_native_cluster_configuration(raw + "temperature_precision=whole\n"); });
    for (const auto& line : {"version=1\n", "speed_unit=kmh\n", "speed_maximum_kmh=260\n",
                             "rpm_maximum=8000\n", "rpm_redline=6500\n", "show_rpm=true\n"}) {
      auto missing = raw;
      missing.erase(missing.find(line), std::string_view(line).size());
      rejects([&] { (void)parse_native_cluster_configuration(missing); });
    }
  }
  for (const auto name : {"", "Whole", "one-decimal", "0", "true", "one_decimal "})
    rejects([&] {
      (void)parse_native_cluster_configuration(required + "temperature_precision=" + name + "\n");
    });
  NativeClusterConfiguration invalid;
  invalid.temperature_precision = static_cast<NativeTemperaturePrecision>(99);
  check(!valid_native_cluster_configuration(invalid), "invalid C++ precision enum accepted");
  rejects([&] { (void)draw({}, Clock::time_point{}, ClusterCenter::vehicle, invalid); });
}
}  // namespace

int main(int argc, char** argv) try {
  using namespace headunit;
  using namespace std::chrono_literals;
  check(argc == 2, "temperature image directory required");
  const std::filesystem::path images(argv[1]);
  std::filesystem::create_directories(images);
  parser();
  exhaustive_strings();
  const auto now = Clock::time_point{10s};
  auto sample = SimulatedVehicle(now).sample(now);
  sample.indicators.fill(IndicatorState::off);
  sample.indicator(Indicator::coolant_warning) = IndicatorState::on;
  struct Known {
    double coolant, oil;
    const char* c;
    const char* o;
    const char* f;
    const char* p;
  };
  const std::array references{Known{0., -0., "0.0", "-0.0", "32.0", "32.0"},
                              Known{-50., 200., "-50.0", "200.0", "-58.0", "392.0"},
                              Known{180., -50., "180.0", "-50.0", "356.0", "-58.0"},
                              Known{-40., -40., "-40.0", "-40.0", "-40.0", "-40.0"},
                              Known{-18., -17.7, "-18.0", "-17.7", "-0.4", "0.1"},
                              Known{20.25, 20.75, "20.2", "20.8", "68.5", "69.3"},
                              Known{-17.8, -17.76, "-17.8", "-17.8", "-0.0", "0.0"},
                              Known{-0.001, 99.96, "-0.0", "100.0", "32.0", "211.9"}};
  std::size_t frames = 0;
  for (std::size_t k = 0; k < references.size(); ++k) {
    const auto& known = references[k];
    sample.coolant_c = known.coolant;
    sample.oil_c = known.oil;
    for (const bool fahrenheit : {false, true}) {
      for (const auto page : pages) {
        NativeClusterConfiguration whole;
        whole.temperature_unit =
            fahrenheit ? NativeTemperatureUnit::fahrenheit : NativeTemperatureUnit::celsius;
        whole.pressure_unit = k % 3 == 0   ? NativePressureUnit::kpa
                              : k % 3 == 1 ? NativePressureUnit::bar
                                           : NativePressureUnit::psi;
        whole.speed_precision =
            k % 2 ? NativeSpeedPrecision::whole : NativeSpeedPrecision::one_decimal;
        whole.miles_per_hour = k % 2;
        whole.distance_unit = NativeDistanceUnit::miles;
        whole.fuel_consumption_unit = NativeFuelConsumptionUnit::imperial_mpg;
        auto decimal = whole;
        decimal.temperature_precision = NativeTemperaturePrecision::one_decimal;
        const auto before = draw(sample, now, page, whole);
        const auto after = draw(sample, now, page, decimal);
        temperatures(after.text, page, fahrenheit ? known.f : known.c,
                     fahrenheit ? known.p : known.o, fahrenheit);
        only_temperature_text(before, after);
        auto omitted = whole;
        omitted.temperature_precision = NativeTemperaturePrecision{};
        check(before.image.pixels == draw(sample, now, page, omitted).image.pixels,
              "explicit whole changed default pixels");
        if (k == 1 || k == 2) {
          save(images / (std::to_string(static_cast<int>(page)) + (fahrenheit ? "-F-" : "-C-") +
                         std::to_string(k) + ".ppm"),
               after.image);
        }
        ++frames;
      }
    }
  }
  const auto verify = [&](const VehicleSample& value, Clock::time_point at, const char* coolant,
                          const char* oil) {
    for (const bool fahrenheit : {false, true}) {
      NativeClusterConfiguration whole;
      whole.temperature_unit =
          fahrenheit ? NativeTemperatureUnit::fahrenheit : NativeTemperatureUnit::celsius;
      auto decimal = whole;
      decimal.temperature_precision = NativeTemperaturePrecision::one_decimal;
      for (const auto page : pages) {
        const auto before = draw(value, at, page, whole);
        const auto after = draw(value, at, page, decimal);
        temperatures(after.text, page, coolant, oil, fahrenheit);
        only_temperature_text(before, after);
      }
    }
  };
  sample.coolant_c.reset();
  sample.oil_c = 0.;
  for (const bool fahrenheit : {false, true}) {
    NativeClusterConfiguration config;
    config.temperature_unit =
        fahrenheit ? NativeTemperatureUnit::fahrenheit : NativeTemperatureUnit::celsius;
    config.temperature_precision = NativeTemperaturePrecision::one_decimal;
    for (const auto page : pages) {
      const auto frame = draw(sample, now, page, config);
      temperatures(frame.text, page, "--", fahrenheit ? "32.0" : "0.0", fahrenheit);
    }
  }
  sample.coolant_c.reset();
  sample.oil_c.reset();
  verify(sample, now, "--", "--");
  for (const auto invalid : {-50.01, 180.01, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
    sample.coolant_c = invalid;
    sample.oil_c = 0.;
    verify(sample, now, "--", "--");
  }
  for (const auto invalid : {-50.01, 200.01}) {
    sample.coolant_c = 0.;
    sample.oil_c = invalid;
    verify(sample, now, "--", "--");
  }
  sample.coolant_c = -40.;
  sample.oil_c = -40.;
  sample.sampled_at = now - 1s;
  verify(sample, now, "--", "--");
  VehicleAgePolicy ages;
  ages.readings[3] = 100ms;
  ages.readings[4] = 200ms;
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, now, now);
  const std::array acquired{
      VehicleObservation{"coolant_c", -40., now, VehicleObservationOrigin::simulation},
      VehicleObservation{"oil_c", -40., now, VehicleObservationOrigin::calculated}};
  cache.accept(7, 1, acquired, now);
  const auto held = cache.sample(now);
  const std::array bad{VehicleObservation{"coolant_c", 0., now + 1ms},
                       VehicleObservation{"oil_c", 200.01, now + 1ms}};
  rejects([&] { cache.accept(7, 2, bad, now + 1ms); });
  verify(cache.sample(now + 1ms), now + 1ms, "-40.0", "-40.0");
  verify(held, now + 99ms, "-40.0", "-40.0");
  verify(held, now + 100ms, "--", "-40.0");
  verify(held, now + 200ms, "--", "--");
  cache.withdraw(now + 20ms);
  verify(cache.sample(now + 20ms), now + 20ms, "--", "--");
  cache.accept(7, 2, acquired, now + 21ms);
  verify(cache.sample(now + 21ms), now + 21ms, "--", "--");
  cache.begin_generation(8, now + 30ms, now + 30ms);
  verify(cache.sample(now + 30ms), now + 30ms, "--", "--");
  rejects([&] { cache.accept(7, 3, acquired, now + 31ms); });
  const std::array fresh{
      VehicleObservation{"coolant_c", -40., now + 31ms, VehicleObservationOrigin::measured},
      VehicleObservation{"oil_c", -40., now + 31ms, VehicleObservationOrigin::calculated}};
  cache.accept(8, 1, fresh, now + 31ms);
  verify(cache.sample(now + 31ms), now + 31ms, "-40.0", "-40.0");
  check(held.coolant_c == -40. && held.oil_c == -40. && held.observation_timing->generation == 7 &&
            held.observation_timing->readings[3].acquired_at == now &&
            held.observation_timing->readings[3].origin == VehicleObservationOrigin::simulation &&
            held.observation_timing->readings[4].origin == VehicleObservationOrigin::calculated,
        "precision mutated source values, original dates, generation or provenance");
  std::cout << "MAXIMUM actual combined value/unit ink widths card=" << maximum_ink_width[0]
            << " dial=" << maximum_ink_width[1] << " strip=" << maximum_ink_width[2] << '\n';
  std::cout << "Production/independent full-frame known comparisons=" << frames
            << "; unavailable, exact expiry, atomic rejection and retirement passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
