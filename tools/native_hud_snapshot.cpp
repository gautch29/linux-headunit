// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_hud.hpp>
#include <headunit/hud_configuration.hpp>
#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) try {
  using namespace headunit;
  if (argc < 2)
    throw std::invalid_argument(
        "usage: headunit-native-hud-snapshot OUTPUT.ppm [--config PATH] [--country CC] [--fixture "
        "| --calibration-grid] [--upright] [--vertical] [--rotate 0|1|2|3]");
  NativeHudContent content;
  HudCalibration calibration;
  unsigned width = 960, height = 360;
  bool upright = false, vertical = false, grid = false;
  std::optional<unsigned> rotation;
  std::string config;
  std::optional<std::string> country;
  const auto now = Clock::now();
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--fixture") {
      content.vehicle.speed_kmh = 82;
      content.vehicle.sampled_at = now;
      content.vehicle.simulated = true;
      content.navigation.available = true;
      content.navigation.route_state = 1;
      content.navigation.source = "HUD visual fixture";
      content.navigation.distance_m = 300;
      content.navigation.maneuver = NavigationManeuver{7, 2, "Turn right", "Fixture road", {}};
      content.speed_limit = HudSpeedLimitObservation{
          90,
          {now, std::chrono::seconds(1), VehicleObservationOrigin::simulation,
           VehicleAvailability::available},
          "visual-fixture"};
    } else if (arg == "--config" && i + 1 < argc && config.empty())
      config = argv[++i];
    else if (arg == "--country" && i + 1 < argc && !country) {
      country = argv[++i];
      if (*country == "auto") country->clear();
      if (!valid_hud_country(*country)) throw std::invalid_argument("invalid HUD country");
    } else if (arg == "--upright" || arg == "--unreflected")
      upright = true;
    else if (arg == "--vertical")
      vertical = true;
    else if (arg == "--calibration-grid")
      grid = true;
    else if (arg == "--rotate" && i + 1 < argc) {
      unsigned turns = 0;
      const std::string_view value(argv[++i]);
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), turns);
      if (error != std::errc{} || end != value.data() + value.size())
        throw std::invalid_argument("invalid HUD rotation");
      rotation = turns;
    } else
      throw std::invalid_argument("unknown HUD snapshot option");
  }
  if (!config.empty()) {
    const auto c = load_hud_configuration(config);
    calibration = c.calibration;
    width = c.width;
    height = c.height;
    content.country = c.country;
  }
  if (country) content.country = *country;
  if (content.speed_limit && hud_road_locale(content.country).unit == HudSpeedUnit::miles_per_hour)
    content.speed_limit->kmh = 55 * 1.609344;  // Explicit visual fixture, not a road reading.
  if (upright) calibration.reflect_horizontal = calibration.reflect_vertical = false;
  if (vertical) calibration.reflect_vertical = true;
  if (rotation) calibration.quarter_turns = *rotation;
  std::ofstream output(argv[1], std::ios::binary);
  if (!output) throw std::runtime_error("cannot open HUD snapshot");
  write_ppm(grid ? render_hud_calibration_grid(width, height, calibration)
                 : render_native_hud(content, now, width, height, calibration),
            output);
  if (!output) throw std::runtime_error("cannot write HUD snapshot");
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
