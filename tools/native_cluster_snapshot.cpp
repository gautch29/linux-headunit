// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster.hpp>
#include <headunit/vehicle_wire.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc, char** argv) try {
  if (argc < 2 || argc > 31)
    throw std::invalid_argument(
        "usage: headunit-native-cluster-snapshot OUTPUT.ppm [MEDIA.json] [--backdrop MAP.ppm] "
        "[--navigation-demo] [--center map|media|vehicle|trip|minimal|sport|gauges] "
        "[--native-cluster-config FILE] [--sample HUV2_FILE] [--unavailable-vehicle] "
        "[--transmission-fixture "
        "TYPE/SELECTOR/GEAR|unavailable] [--drive-mode-catalog ID,ID] [--drive-mode-fixture "
        "ID|unavailable] [--trip-fixture KEY|unavailable] [--heading-fixture KEY|unavailable] "
        "[--indicator-fixture all|KEY,KEY] [--turn-signal-fixture "
        "lit|mid-fade|off|hazard|unknown|stale]");
  const auto now = headunit::Clock::now();
  headunit::SimulatedVehicle source(now);
  headunit::NavigationSnapshot navigation;
  std::string media;
  auto center = headunit::ClusterCenter::navigation;
  std::optional<headunit::TransmissionState> transmission;
  std::optional<headunit::DriveModeCatalog> drive_modes;
  std::optional<headunit::DriveModeIdentifier> drive_mode;
  std::optional<headunit::TripState> trip;
  std::optional<headunit::HeadingState> heading;
  bool mode_fixture = false, trip_fixture = false, heading_fixture = false;
  std::string backdrop, sample_file;
  headunit::NativeClusterConfiguration configuration;
  bool unavailable_vehicle = false;
  std::optional<std::string> indicator_fixture;
  std::optional<std::string> turn_fixture;
  for (int i = 2; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--native-cluster-config" && i + 1 < argc) {
      configuration = headunit::load_native_cluster_configuration(argv[++i]);
    } else if (std::string_view(argv[i]) == "--sample" && i + 1 < argc && sample_file.empty()) {
      sample_file = argv[++i];
    } else if (std::string_view(argv[i]) == "--unavailable-vehicle" && !unavailable_vehicle) {
      unavailable_vehicle = true;
    } else if (std::string_view(argv[i]) == "--navigation-demo") {
      navigation.available = true;
      navigation.route_state = 1;
      navigation.distance_m = 300;
      navigation.maneuver =
          headunit::NavigationManeuver{7, 2, "Turn right onto Rue des Lilas", "Rue des Lilas", 90};
    } else if (std::string_view(argv[i]) == "--turn-signal-fixture" && i + 1 < argc &&
               !turn_fixture) {
      turn_fixture = argv[++i];
    } else if (std::string_view(argv[i]) == "--indicator-fixture" && i + 1 < argc &&
               !indicator_fixture) {
      indicator_fixture = argv[++i];
    } else if (std::string_view(argv[i]) == "--backdrop" && i + 1 < argc && backdrop.empty()) {
      backdrop = argv[++i];
    } else if (std::string_view(argv[i]) == "--center" && i + 1 < argc) {
      const std::string_view choice(argv[++i]);
      if (choice == "map")
        center = headunit::ClusterCenter::navigation;
      else if (choice == "media")
        center = headunit::ClusterCenter::media;
      else if (choice == "vehicle")
        center = headunit::ClusterCenter::vehicle;
      else if (choice == "trip")
        center = headunit::ClusterCenter::trip;
      else if (choice == "minimal")
        center = headunit::ClusterCenter::minimal;
      else if (choice == "sport")
        center = headunit::ClusterCenter::sport;
      else if (choice == "gauges")
        center = headunit::ClusterCenter::gauges;
      else
        throw std::invalid_argument("unknown center page");
    } else if (std::string_view(argv[i]) == "--transmission-fixture" && i + 1 < argc) {
      const std::string_view value(argv[++i]);
      transmission =
          value == "unavailable" ? std::nullopt : headunit::parse_transmission_state_key(value);
      if (value != "unavailable" && !transmission)
        throw std::invalid_argument("invalid coherent transmission fixture");
    } else if (std::string_view(argv[i]) == "--drive-mode-catalog" && i + 1 < argc) {
      if (drive_modes) throw std::invalid_argument("duplicate drive-mode catalog fixture");
      drive_modes = headunit::parse_drive_mode_catalog_key(argv[++i]);
      if (!drive_modes) throw std::invalid_argument("invalid drive-mode catalog fixture");
    } else if (std::string_view(argv[i]) == "--drive-mode-fixture" && i + 1 < argc) {
      if (mode_fixture) throw std::invalid_argument("duplicate drive-mode fixture");
      mode_fixture = true;
      const std::string_view value(argv[++i]);
      if (value != "unavailable") {
        drive_mode = headunit::parse_drive_mode_identifier(value);
        if (!drive_mode) throw std::invalid_argument("invalid drive-mode fixture");
      }
    } else if (std::string_view(argv[i]) == "--trip-fixture" && i + 1 < argc) {
      if (trip_fixture) throw std::invalid_argument("duplicate trip fixture");
      trip_fixture = true;
      const std::string_view value(argv[++i]);
      if (value != "unavailable") {
        trip = headunit::parse_trip_state_key(value);
        if (!trip) throw std::invalid_argument("invalid coherent trip fixture");
      }
    } else if (std::string_view(argv[i]) == "--heading-fixture" && i + 1 < argc) {
      if (heading_fixture) throw std::invalid_argument("duplicate heading fixture");
      heading_fixture = true;
      const std::string_view value(argv[++i]);
      if (value != "unavailable") {
        heading = headunit::parse_heading_state_key(value);
        if (!heading) throw std::invalid_argument("invalid qualified heading fixture");
      }
    } else if (!std::string_view(argv[i]).starts_with("--") && media.empty())
      media = argv[i];
    else
      throw std::invalid_argument("unexpected snapshot argument");
  }
  if (mode_fixture && !drive_modes)
    throw std::invalid_argument("drive-mode fixture requires its configured catalog");
  if (drive_mode && !headunit::contains_drive_mode(*drive_modes, *drive_mode))
    throw std::invalid_argument("drive-mode fixture is not in the configured catalog");
  auto sample = source.sample(now);
  if (!sample_file.empty()) {
    std::ifstream input(sample_file, std::ios::binary);
    std::string packet(headunit::maximum_vehicle_datagram + 1, '\0');
    input.read(packet.data(), std::streamsize(packet.size()));
    packet.resize(std::size_t(input.gcount()));
    const auto decoded = headunit::parse_vehicle_datagram(packet, now);
    if (!input.eof() || !decoded) throw std::invalid_argument("invalid bounded HUV fixture");
    sample = *decoded;
    sample.simulated = true;
  }
  if (unavailable_vehicle) {
    sample = {};
    sample.sampled_at = now;
    sample.simulated = true;
  }
  if (indicator_fixture) {
    sample.indicators.fill(headunit::IndicatorState::off);
    if (*indicator_fixture == "all") {
      sample.indicators.fill(headunit::IndicatorState::on);
    } else {
      std::string_view keys(*indicator_fixture);
      if (keys.empty() || keys.size() > 512)
        throw std::invalid_argument("invalid indicator fixture");
      while (!keys.empty()) {
        const auto comma = keys.find(',');
        const auto key = keys.substr(0, comma);
        const auto found = std::find_if(
            headunit::indicator_definitions.begin(), headunit::indicator_definitions.end(),
            [&](const auto& definition) { return definition.key == key; });
        if (found == headunit::indicator_definitions.end() ||
            sample.indicator(found->id) == headunit::IndicatorState::on)
          throw std::invalid_argument("unknown or duplicate indicator fixture");
        sample.indicator(found->id) = headunit::IndicatorState::on;
        if (comma == std::string_view::npos) break;
        keys.remove_prefix(comma + 1);
        if (keys.empty()) throw std::invalid_argument("empty indicator fixture key");
      }
    }
  }
  if (indicator_fixture &&
      sample.indicator(headunit::Indicator::hazard) == headunit::IndicatorState::on) {
    for (const auto direction : {headunit::Indicator::left_turn, headunit::Indicator::right_turn}) {
      if (sample.indicator(direction) == headunit::IndicatorState::off)
        sample.indicator(direction) = headunit::IndicatorState::flashing;
    }
  }
  sample.transmission = transmission;
  sample.drive_mode_catalog = drive_modes;
  sample.drive_mode = drive_mode;
  sample.trip = trip;
  sample.heading = heading;
  // Design review: a 1920x720 binary PPM stands in for the phone's map
  // surface beneath the transparent Map page.
  std::optional<headunit::TurnSignalDisplay> lamps;
  if (turn_fixture) {
    if (indicator_fixture || !sample_file.empty() || unavailable_vehicle)
      throw std::invalid_argument("turn fixture must be isolated from other vehicle fixtures");
    const auto& choice = *turn_fixture;
    if (choice != "lit" && choice != "mid-fade" && choice != "off" && choice != "hazard" &&
        choice != "unknown" && choice != "stale")
      throw std::invalid_argument("unknown turn-signal fixture");
    // Explicit synthetic acquisition/transition history, never phone/car evidence.
    sample.simulated = true;
    sample.sampled_at = now - std::chrono::milliseconds(200);
    sample.indicators.fill(headunit::IndicatorState::off);
    headunit::TurnSignalAnimation signals;
    signals.update(sample, sample.sampled_at);
    sample.indicator(headunit::Indicator::left_turn) = headunit::IndicatorState::on;
    if (choice == "hazard") {
      sample.indicator(headunit::Indicator::hazard) = headunit::IndicatorState::on;
      sample.indicator(headunit::Indicator::right_turn) = headunit::IndicatorState::on;
    }
    signals.update(sample, now - std::chrono::milliseconds(160));
    if (choice == "mid-fade" || choice == "off") {
      sample.indicator(headunit::Indicator::left_turn) = headunit::IndicatorState::off;
      signals.update(sample, now - std::chrono::milliseconds(choice == "mid-fade" ? 60 : 120));
    }
    if (choice == "unknown") {
      sample.indicator(headunit::Indicator::left_turn).reset();
      sample.indicator(headunit::Indicator::right_turn).reset();
    }
    if (choice == "stale") sample.sampled_at = now - std::chrono::seconds(1);
    lamps = signals.update(sample, now);
  }
  std::vector<std::uint32_t> frame(1920 * 720);
  headunit::draw_native_cluster(
      {sample, media, navigation, center, true, configuration, {}, lamps ? &*lamps : nullptr}, now,
      frame.data(), 1920, 720, 1920 * 4);
  headunit::Image image{1920, 720, std::vector<headunit::Rgb>(frame.size())};
  if (!backdrop.empty()) {
    std::ifstream in(backdrop, std::ios::binary);
    std::string magic;
    unsigned w = 0, h = 0, depth = 0;
    in >> magic >> w >> h >> depth;
    in.get();
    if (!in || magic != "P6" || w != 1920 || h != 720 || depth != 255)
      throw std::invalid_argument("backdrop must be a 1920x720 binary PPM");
    in.read(reinterpret_cast<char*>(image.pixels.data()), std::streamsize(image.pixels.size() * 3));
    if (!in) throw std::invalid_argument("truncated backdrop");
  }
  for (std::size_t i = 0; i < frame.size(); ++i) {
    const auto p = frame[i];
    const unsigned keep = 255 - (p >> 24);
    auto& q = image.pixels[i];
    q = {std::uint8_t(((p >> 16) & 255) + q.r * keep / 255),
         std::uint8_t(((p >> 8) & 255) + q.g * keep / 255),
         std::uint8_t((p & 255) + q.b * keep / 255)};
  }
  std::ofstream out(argv[1], std::ios::binary);
  headunit::write_ppm(image, out);
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
