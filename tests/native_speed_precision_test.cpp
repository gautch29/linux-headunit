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
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
struct TextDraw {
  std::string value;
  double x, y;
  double size;
  double requested_x, requested_y;
  cairo_text_extents_t ink;
};
std::vector<TextDraw> text_draws;
double requested_x = 0, requested_y = 0;
void observed_move_to(cairo_t* context, double x, double y) {
  requested_x = x;
  requested_y = y;
  cairo_move_to(context, x, y);
}
void observed_show_text(cairo_t* context, const char* value) {
  double x = 0, y = 0;
  cairo_get_current_point(context, &x, &y);
  cairo_text_extents_t ink{};
  cairo_text_extents(context, value, &ink);
  cairo_matrix_t matrix{};
  cairo_get_font_matrix(context, &matrix);
  text_draws.push_back({value, x, y, matrix.xx, requested_x, requested_y, ink});
  cairo_show_text(context, value);
}
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
}  // namespace

// Observe actual Cairo calls without replacing text, fonts, layout or clipping.
// Every image must equal the independently linked, unmodified production path.
#define cairo_show_text observed_show_text
#define cairo_move_to observed_move_to
#define draw_native_cluster observed_draw_native_cluster
#define render_native_cluster observed_render_native_cluster
#define selected_cluster_center observed_selected_cluster_center
#define native_cluster_map observed_native_cluster_map
#include "../src/native_cluster.cpp"
#undef cairo_show_text
#undef cairo_move_to
#undef draw_native_cluster
#undef render_native_cluster
#undef selected_cluster_center
#undef native_cluster_map

namespace {
using namespace headunit;
struct Frame {
  Image image;
  std::vector<TextDraw> text;
  std::size_t speed;
};
Frame draw(const VehicleSample& sample, Clock::time_point now, ClusterCenter page,
           const NativeClusterConfiguration& configuration) {
  text_draws.clear();
  auto image =
      observed_render_native_cluster(sample, now, 1920, 720, {}, {}, page, false, configuration);
  check(
      image.pixels ==
          render_native_cluster(sample, now, 1920, 720, {}, {}, page, false, configuration).pixels,
      "text observation changed actual production pixels");
  const double unit_y = page == ClusterCenter::minimal ? 382
                        : page == ClusterCenter::sport ? 372
                                                       : 404;
  for (std::size_t i = 1; i < text_draws.size(); ++i) {
    if (text_draws[i].y == unit_y &&
        (text_draws[i].value == "km/h" || text_draws[i].value == "mph"))
      return {std::move(image), text_draws, i - 1};
  }
  throw std::runtime_error("instantaneous speed allocation missing");
}
bool ink(const TextDraw& text, unsigned x, unsigned y) {
  return double(x) >= std::floor(text.x + text.ink.x_bearing) - 1 &&
         double(x) <= std::ceil(text.x + text.ink.x_bearing + text.ink.width) + 1 &&
         double(y) >= std::floor(text.y + text.ink.y_bearing) - 1 &&
         double(y) <= std::ceil(text.y + text.ink.y_bearing + text.ink.height) + 1;
}
void only_speed_text(const Frame& whole, const Frame& decimal) {
  check(whole.text.size() == decimal.text.size() && whole.speed == decimal.speed,
        "precision changed text allocations");
  for (std::size_t i = 0; i < whole.text.size(); ++i) {
    if (i == whole.speed) continue;
    check(whole.text[i].value == decimal.text[i].value && whole.text[i].x == decimal.text[i].x &&
              whole.text[i].y == decimal.text[i].y && whole.text[i].size == decimal.text[i].size,
          "precision changed unrelated text or placement");
  }
  for (unsigned y = 0; y < whole.image.height; ++y) {
    for (unsigned x = 0; x < whole.image.width; ++x) {
      const auto index = std::size_t(y) * whole.image.width + x;
      if (whole.image.pixels[index] == decimal.image.pixels[index]) continue;
      check(ink(whole.text[whole.speed], x, y) || ink(decimal.text[decimal.speed], x, y),
            "precision changed pixels outside instantaneous speed ink");
    }
  }
}
void save(const std::filesystem::path& path, const Image& image) {
  std::ofstream output(path, std::ios::binary);
  write_ppm(image, output);
  output.close();
  check(bool(output), "production precision image retention failed");
}
struct Site {
  double x, y, maximum, unit_y, unit_size;
  Region allocation;
};
Site site(ClusterCenter page) {
  if (page == ClusterCenter::minimal) return {960, 330, 192, 382, 28, layout::minimal};
  if (page == ClusterCenter::sport) return {700, 326, 158, 372, 26, {480, 96, 522, 430}};
  return {300, 358, 124, 404, 24, layout::speed};
}
void verify_size(const Frame& frame, ClusterCenter page, cairo_t* comparison = nullptr) {
  const auto& value = frame.text[frame.speed];
  const auto& unit = frame.text[frame.speed + 1];
  const auto target = site(page);
  check(value.size >= 124 && value.size <= target.maximum, "speed size grew or fell below124");
  check(value.requested_y == target.y &&
            std::abs(value.requested_x + value.ink.x_advance / 2 - target.x) < 1e-8,
        "numeric fitting moved original baseline or center");
  check(value.x + value.ink.x_bearing >= target.allocation.x &&
            value.x + value.ink.x_bearing + value.ink.width <=
                target.allocation.x + target.allocation.width &&
            value.y + value.ink.y_bearing >= target.allocation.y &&
            value.y + value.ink.y_bearing + value.ink.height <= unit.y + unit.ink.y_bearing,
        "numeric ink overlaps original allocation or reserved unit ink");
  std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(nullptr,
                                                                             cairo_surface_destroy);
  std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(nullptr, cairo_destroy);
  if (!comparison) {
    surface.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 720));
    context.reset(cairo_create(surface.get()));
    comparison = context.get();
  }
  const auto recorded = text_draws.size();
  text(comparison, target.x, target.y, target.maximum, value.value, tone::text, Weight::bold,
       Align::center);
  const auto original = text_draws.back();
  text_draws.resize(recorded);
  const double left = original.x + original.ink.x_bearing;
  const bool original_fits =
      left >= target.allocation.x &&
      left + original.ink.width <= target.allocation.x + target.allocation.width &&
      original.y + original.ink.y_bearing >= target.allocation.y &&
      original.y + original.ink.y_bearing + original.ink.height <= unit.y + unit.ink.y_bearing;
  check(original_fits ? value.size == target.maximum : value.size < target.maximum,
        "optional value shrank unnecessarily or kept an overflowing maximum");
}
void parser_contract() {
  const std::string required =
      "version=1\nspeed_unit=kmh\nspeed_maximum_kmh=260\n"
      "rpm_maximum=8000\nrpm_redline=6500\nshow_rpm=true\n";
  const std::string optional =
      "temperature_unit=fahrenheit\ndistance_unit=miles\n"
      "fuel_consumption_unit=us_mpg\nspeed_precision=one_decimal\n"
      "drive_mode_profiles_file=/private/profile\n";
  check(parse_native_cluster_configuration(required).speed_precision == NativeSpeedPrecision::whole,
        "version1 omission changed whole default");
  for (const auto& [key, precision] :
       {std::pair{"whole", NativeSpeedPrecision::whole},
        std::pair{"one_decimal", NativeSpeedPrecision::one_decimal}}) {
    check(parse_native_cluster_configuration(required + "speed_precision=" + key + "\n")
                  .speed_precision == precision,
          "explicit precision enum lost");
  }
  VehicleSample sample;
  sample.sampled_at = Clock::time_point{std::chrono::seconds(10)};
  sample.speed_kmh = 99.96;
  const auto legacy = parse_native_cluster_configuration(required);
  const auto explicit_whole =
      parse_native_cluster_configuration(required + "speed_precision=whole\n");
  for (const auto page :
       {ClusterCenter::navigation, ClusterCenter::media, ClusterCenter::vehicle,
        ClusterCenter::trip, ClusterCenter::minimal, ClusterCenter::sport, ClusterCenter::gauges}) {
    check(render_native_cluster(sample, sample.sampled_at, 1920, 720, {}, {}, page, false, legacy)
                  .pixels == render_native_cluster(sample, sample.sampled_at, 1920, 720, {}, {},
                                                   page, false, explicit_whole)
                                 .pixels,
          "explicit whole changed omitted configuration pixels");
  }
  const auto full = required + optional;
  const auto bounded = full + std::string(4096 - full.size(), '\n');
  const auto parsed = parse_native_cluster_configuration(bounded);
  check(parsed.speed_precision == NativeSpeedPrecision::one_decimal &&
            parsed.temperature_unit == NativeTemperatureUnit::fahrenheit &&
            parsed.distance_unit == NativeDistanceUnit::miles &&
            parsed.fuel_consumption_unit == NativeFuelConsumptionUnit::us_mpg &&
            parsed.drive_mode_profiles_file == "/private/profile",
        "eleven fields displaced old configuration");
  const auto rejects = [](const std::string& value) {
    try {
      (void)parse_native_cluster_configuration(value);
    } catch (const std::invalid_argument&) {
      return;
    }
    throw std::runtime_error("invalid precision configuration admitted");
  };
  for (const auto& suffix : {"speed_precision=\n", "speed_precision=Whole\n", "speed_precision=1\n",
                             "speed_precision=one-decimal\n", "speed_precision=true\n",
                             "speed_precision=whole\nspeed_precision=whole\n"})
    rejects(required + suffix);
  rejects(bounded + "\n");
  rejects(full + "unknown=1\n");
  for (const auto& field : {"version=1\n", "speed_unit=kmh\n", "speed_maximum_kmh=260\n",
                            "rpm_maximum=8000\n", "rpm_redline=6500\n", "show_rpm=true\n"}) {
    auto missing = full;
    missing.erase(missing.find(field), std::string_view(field).size());
    rejects(missing);
  }
  auto invalid = parsed;
  invalid.speed_precision = static_cast<NativeSpeedPrecision>(99);
  check(!valid_native_cluster_configuration(invalid), "invalid C++ precision enum admitted");
  bool rejected = false;
  try {
    (void)render_native_cluster({}, Clock::time_point{std::chrono::seconds(10)}, 1920, 720, {}, {},
                                ClusterCenter::vehicle, false, invalid);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "renderer admitted invalid precision enum");
}
void all_decimal_strings() {
  std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 720), cairo_surface_destroy);
  std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()),
                                                             cairo_destroy);
  auto* c = context.get();
  std::size_t count = 0;
  for (const bool mph : {false, true}) {
    NativeClusterConfiguration config;
    config.miles_per_hour = mph;
    config.speed_precision = NativeSpeedPrecision::one_decimal;
    const unsigned maximum_tenth = mph ? 1864 : 3000;
    for (unsigned tenth = 0; tenth <= maximum_tenth; ++tenth) {
      VehicleSample sample;
      sample.sampled_at = Clock::time_point{std::chrono::seconds(10)};
      sample.speed_kmh = (double(tenth) / 10) * (mph ? 1.609344 : 1.);
      check(valid_vehicle_sample(sample), "enumerator exceeded existing source bounds");
      const auto expected = std::to_string(tenth / 10) + "." + std::to_string(tenth % 10);
      for (const auto page :
           {ClusterCenter::vehicle, ClusterCenter::minimal, ClusterCenter::sport}) {
        const auto target = site(page);
        text_draws.clear();
        speed_text(c, sample, true, config, target.x, target.y, target.maximum, target.allocation,
                   target.unit_y, target.unit_size);
        text(c, target.x, target.unit_y, target.unit_size, mph ? "mph" : "km/h", tone::muted,
             Weight::regular, Align::center);
        check(text_draws.size() == 2 && text_draws[0].value == expected,
              "production fitted component lost a full admitted one-decimal string");
        // The same actual text/center/ink/size assertions used by the full-frame
        // production caller comparisons; no copied speed-formatting algorithm.
        verify_size({{}, text_draws, 0}, page, c);
        ++count;
      }
    }
    VehicleSample signed_zero;
    signed_zero.sampled_at = Clock::time_point{std::chrono::seconds(10)};
    signed_zero.speed_kmh = -0.;
    check(valid_vehicle_sample(signed_zero), "admitted signed zero rejected");
    for (const auto page : {ClusterCenter::vehicle, ClusterCenter::minimal, ClusterCenter::sport}) {
      const auto target = site(page);
      text_draws.clear();
      speed_text(c, signed_zero, true, config, target.x, target.y, target.maximum,
                 target.allocation, target.unit_y, target.unit_size);
      text(c, target.x, target.unit_y, target.unit_size, mph ? "mph" : "km/h", tone::muted,
           Weight::regular, Align::center);
      check(text_draws[0].value == "-0.0", "formatting silently changed signed canonical zero");
      verify_size({{}, text_draws, 0}, page, c);
      ++count;
    }
  }
  check(cairo_status(c) == CAIRO_STATUS_SUCCESS, "actual Cairo exhaustive speed component failed");
  std::cout << "EXHAUSTIVE actual Cairo speed-value draws=" << count << '\n';
}
void negative_bearing_and_minimum() {
  std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 720), cairo_surface_destroy);
  std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()),
                                                             cairo_destroy);
  auto* c = context.get();
  std::string glyph;
  int left_boundary = 0;
  for (const auto* candidate : {"j", "f", "T", "́", "̂", "̧"}) {
    font(c, 158, Weight::bold);
    cairo_text_extents_t maximum{};
    cairo_text_extents(c, candidate, &maximum);
    font(c, 124, Weight::bold);
    cairo_text_extents_t minimum{};
    cairo_text_extents(c, candidate, &minimum);
    const double maximum_left = 700 - maximum.x_advance / 2 + maximum.x_bearing;
    const double minimum_left = 700 - minimum.x_advance / 2 + minimum.x_bearing;
    const int boundary = int(std::ceil((maximum_left + minimum_left) / 2));
    if (maximum.x_bearing < 0 && maximum_left < boundary && minimum_left >= boundary) {
      glyph = candidate;
      left_boundary = boundary;
      break;
    }
  }
  check(!glyph.empty(), "real negative-bearing Cairo component fixture unavailable");
  const Region allocation{left_boundary, 96, 200, 430};
  const double fitted = fitted_speed_size(c, glyph, 700, 326, 158, allocation, 400);
  check(fitted >= 124 && fitted < 158, "negative bearing ignored or below floor");
  text_draws.clear();
  text(c, 700, 326, fitted, glyph, tone::text, Weight::bold, Align::center);
  check(text_draws[0].ink.x_bearing < 0 &&
            text_draws[0].x + text_draws[0].ink.x_bearing >= allocation.x,
        "advance-only sizing clipped actual negative-bearing glyph");
  std::cout << "NEGATIVE_BEARING actual glyph=" << glyph
            << " bearing=" << text_draws[0].ink.x_bearing << " size=" << fitted << '\n';
  bool rejected = false;
  try {
    (void)fitted_speed_size(c, "300.0", 700, 326, 158, {699, 96, 2, 430}, 400);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  check(rejected, "minimum failure silently clamped or fell back");
}
void unit_interactions_and_retirement(VehicleObservationCache& cache, Clock::time_point now,
                                      std::size_t sequence) {
  using namespace std::chrono_literals;
  constexpr std::array pages{
      ClusterCenter::navigation, ClusterCenter::media, ClusterCenter::vehicle, ClusterCenter::trip,
      ClusterCenter::minimal,    ClusterCenter::sport, ClusterCenter::gauges};
  const auto acquired = now + std::chrono::milliseconds(sequence);
  TripState trip;
  trip.identifier = *parse_trip_identifier("currentTrip");
  trip.reset_generation = 1;
  trip.distance_km = 60.;
  trip.elapsed_seconds = 3600.;
  trip.moving_seconds = 3600.;
  trip.average_speed_kmh = 60.;
  trip.average_speed_basis = TripAverageSpeedBasis::moving;
  trip.fuel_efficiency_l_per_100km = 8.;
  const std::array auxiliary{VehicleObservation{"engine_rpm", 4000., acquired},
                             VehicleObservation{"fuel_percent", 50., acquired},
                             VehicleObservation{"coolant_c", 90., acquired},
                             VehicleObservation{"oil_c", 100., acquired},
                             VehicleObservation{"range_km", 60., acquired},
                             VehicleObservation{"trip", trip, acquired}};
  cache.accept(1, ++sequence, auxiliary, acquired);
  const auto held = cache.sample(acquired);
  for (const bool mph : {false, true}) {
    for (const auto temperature :
         {NativeTemperatureUnit::celsius, NativeTemperatureUnit::fahrenheit}) {
      for (const auto distance : {NativeDistanceUnit::kilometers, NativeDistanceUnit::miles}) {
        for (const auto consumption :
             {NativeFuelConsumptionUnit::liters_per_100km, NativeFuelConsumptionUnit::us_mpg,
              NativeFuelConsumptionUnit::imperial_mpg}) {
          NativeClusterConfiguration whole;
          whole.miles_per_hour = mph;
          whole.temperature_unit = temperature;
          whole.distance_unit = distance;
          whole.fuel_consumption_unit = consumption;
          auto decimal = whole;
          decimal.speed_precision = NativeSpeedPrecision::one_decimal;
          for (const auto page : pages) {
            const auto before = draw(held, acquired, page, whole);
            const auto after = draw(held, acquired, page, decimal);
            only_speed_text(before, after);
            verify_size(after, page);
          }
        }
      }
    }
  }
  const auto unknown = [&](const VehicleSample& value, Clock::time_point at) {
    for (const bool mph : {false, true}) {
      NativeClusterConfiguration whole;
      whole.miles_per_hour = mph;
      auto decimal = whole;
      decimal.speed_precision = NativeSpeedPrecision::one_decimal;
      for (const auto page : pages) {
        const auto before = draw(value, at, page, whole);
        const auto after = draw(value, at, page, decimal);
        check(before.image.pixels == after.image.pixels && after.text[after.speed].value == "--" &&
                  after.text[after.speed].size == site(page).maximum,
              "unknown/retired/invalid speed changed original path or became zero");
      }
    }
  };
  for (const auto invalid_speed : {-1., 300.0001, std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN()}) {
    auto invalid = held;
    invalid.speed_kmh = invalid_speed;
    unknown(invalid, acquired);
  }
  unknown(held, acquired + 100ms);
  NativeClusterConfiguration decimal;
  decimal.speed_precision = NativeSpeedPrecision::one_decimal;
  const auto just_fresh = draw(held, acquired + 99ms, ClusterCenter::sport, decimal);
  check(just_fresh.text[just_fresh.speed].value == "300.0", "speed expired before original limit");
  cache.withdraw(acquired + 20ms);
  unknown(cache.sample(acquired + 20ms), acquired + 20ms);
  const std::array replay{
      VehicleObservation{"speed_kmh", 300., acquired, VehicleObservationOrigin::calculated}};
  cache.accept(1, ++sequence, replay, acquired + 21ms);
  unknown(cache.sample(acquired + 21ms), acquired + 21ms);
  cache.begin_generation(2, acquired + 30ms, acquired + 30ms);
  unknown(cache.sample(acquired + 30ms), acquired + 30ms);
  const std::array zero{
      VehicleObservation{"speed_kmh", 0., acquired + 31ms, VehicleObservationOrigin::simulation}};
  cache.accept(2, 1, zero, acquired + 31ms);
  const auto successor = cache.sample(acquired + 31ms);
  for (const auto page : pages) {
    const auto frame = draw(successor, acquired + 31ms, page, decimal);
    check(frame.text[frame.speed].value == "0.0", "genuine successor zero became unavailable");
    verify_size(frame, page);
  }
  check(successor.observation_timing->generation == 2 &&
            successor.observation_timing->readings[0].acquired_at == acquired + 31ms &&
            successor.observation_timing->readings[0].origin ==
                VehicleObservationOrigin::simulation &&
            held.speed_kmh == 300. && held.engine_rpm == 4000. && held.coolant_c == 90. &&
            held.oil_c == 100. && held.range_km == 60. && held.trip == trip &&
            held.observation_timing->readings[0].acquired_at == acquired,
        "fit mutated retained source data or manufactured successor acquisition");
}

}  // namespace

int main(int argc, char** argv) try {
  using namespace headunit;
  using namespace std::chrono_literals;
  constexpr std::array pages{
      ClusterCenter::navigation, ClusterCenter::media, ClusterCenter::vehicle, ClusterCenter::trip,
      ClusterCenter::minimal,    ClusterCenter::sport, ClusterCenter::gauges};
  check(argc == 2, "precision image retention directory required");
  const std::filesystem::path images(argv[1]);
  std::filesystem::create_directories(images);
  parser_contract();
  all_decimal_strings();
  negative_bearing_and_minimum();
  const auto now = Clock::time_point{10s};
  VehicleAgePolicy ages;
  ages.readings[0] = 100ms;
  VehicleObservationCache cache(ages);
  cache.begin_generation(1, now, now);
  std::size_t sequence = 0, overflow = 0;
  struct Case {
    double speed;
    const char* kmh;
    const char* mph;
  };
  const std::array cases{Case{0., "0.0", "0.0"},      Case{.04, "0.0", "0.0"},
                         Case{.08, "0.1", "0.0"},     Case{1.609344, "1.6", "1.0"},
                         Case{99.94, "99.9", "62.1"}, Case{99.96, "100.0", "62.1"},
                         Case{300., "300.0", "186.4"}};
  for (const auto& value : cases) {
    const auto acquired = now + std::chrono::milliseconds(++sequence);
    const std::array batch{VehicleObservation{"speed_kmh", value.speed, acquired,
                                              VehicleObservationOrigin::calculated}};
    cache.accept(1, sequence, batch, acquired);
    const auto sample = cache.sample(acquired);
    for (const bool mph : {false, true}) {
      NativeClusterConfiguration whole;
      whole.miles_per_hour = mph;
      auto decimal = whole;
      decimal.speed_precision = NativeSpeedPrecision::one_decimal;
      for (const auto page : pages) {
        const auto original = draw(sample, acquired, page, whole);
        const auto converted = draw(sample, acquired, page, decimal);
        only_speed_text(original, converted);
        const auto& text = converted.text[converted.speed];
        check(text.value == (mph ? value.mph : value.kmh), "actual decimal speed text incorrect");
        const double left = text.x + text.ink.x_bearing;
        const double right = left + text.ink.width;
        const double minimum = page == ClusterCenter::minimal ? 560
                               : page == ClusterCenter::sport ? 480
                                                              : 60;
        const double maximum = page == ClusterCenter::minimal ? 1360
                               : page == ClusterCenter::sport ? 1002
                                                              : 540;
        const auto& unit = converted.text[converted.speed + 1];
        const bool fits =
            left >= minimum && right <= maximum && text.y + text.ink.y_bearing >= 96 &&
            text.y + text.ink.y_bearing + text.ink.height <= unit.y + unit.ink.y_bearing;
        verify_size(converted, page);
        if (value.speed == 300. &&
            (page == ClusterCenter::minimal || page == ClusterCenter::sport ||
             page == ClusterCenter::vehicle)) {
          const auto prefix = std::to_string(static_cast<int>(page)) + (mph ? "-mph" : "-kmh");
          save(images / (prefix + "-whole.ppm"), original.image);
          save(images / (prefix + "-one-decimal.ppm"), converted.image);
        }
        std::cout << "FIT page=" << static_cast<int>(page) << " unit=" << (mph ? "mph" : "kmh")
                  << " source=" << value.speed << " text=" << text.value << " inkLeft=" << left
                  << " inkRight=" << right << " allocationLeft=" << minimum
                  << " allocationRight=" << maximum << " size=" << text.size << " fits=" << fits
                  << '\n';
        if (!fits) ++overflow;
      }
      check(draw(sample, acquired + 100ms, ClusterCenter::sport, decimal)
                    .text[draw(sample, acquired + 100ms, ClusterCenter::sport, decimal).speed]
                    .value == "--",
            "late render renewed original acquisition");
    }
    check(sample.speed_kmh == value.speed && sample.observation_timing->generation == 1 &&
              sample.observation_timing->readings[0].acquired_at == acquired &&
              sample.observation_timing->readings[0].origin == VehicleObservationOrigin::calculated,
          "formatting changed original source identity/value");
  }
  unit_interactions_and_retirement(cache, now, sequence);
  std::cout << "FIT overflowing allocations=" << overflow << '\n';
  check(overflow == 0, "one_decimal does not fit unchanged native instantaneous-speed allocation");
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
