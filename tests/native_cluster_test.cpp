// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/backup_cluster.hpp>
#include <cairo.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
int main() try {
  using namespace headunit;
  namespace region = cluster_layout;
  for (auto [w, h] : {std::pair{1280u, 720u}, std::pair{1920u, 720u}, std::pair{800u, 600u},
                      std::pair{1920u, 1080u}}) {
    const auto r = native_cluster_map(w, h);
    check(r.x >= 0 && r.y >= 0 && r.x + r.width <= int(w) && r.y + r.height <= int(h),
          "map must stay inside the output");
    check(r.width == int(w) || r.height == int(h), "map must fill the fitted cluster canvas");
    check(std::abs(double(r.width) / r.height - 8. / 3) < .015, "map aspect ratio distorted");
  }
  const auto now = Clock::time_point{std::chrono::milliseconds(10000)};
  SimulatedVehicle source(now);
  auto sample = source.sample(now);
  const auto baseline = render_native_cluster(sample, now, 1920, 720);
  for (auto& lamp : sample.indicators)
    lamp = IndicatorState::off;
  // The gear badge is checked separately below; start these lamp checks without one.
  sample.transmission.reset();
  const auto all_off = render_native_cluster(sample, now, 1920, 720);
  // Every lamp has a readable glyph confined to its fixed slot. A second
  // active lamp cannot move or redraw the first, including flashing phases.
  for (const auto& definition : indicator_definitions) {
    if (definition.id == Indicator::hazard) continue;
    auto active = sample;
    active.indicator(definition.id) = IndicatorState::on;
    const auto lit = render_native_cluster(active, now, 1920, 720);
    const auto slot = region::indicator_frame(definition.id);
    std::size_t changed = 0;
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (lit.pixels[index] != all_off.pixels[index]) {
          check(slot.contains(x, y), "warning artwork escaped its fixed slot");
          const bool direction =
              definition.id == Indicator::left_turn || definition.id == Indicator::right_turn;
          check(direction ? y < unsigned(region::center.y) : region::telltales.contains(x, y),
                "warning or direction arrow obscured cluster content");
          ++changed;
        }
      }
    check(changed > 60, "warning glyph is empty or too small");
    active.indicators.fill(IndicatorState::on);
    const auto all_lit = render_native_cluster(active, now, 1920, 720);
    for (int y = slot.y; y < slot.y + slot.height; ++y)
      for (int x = slot.x; x < slot.x + slot.width; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        check(all_lit.pixels[index] == lit.pixels[index], "another lamp moved an active symbol");
      }
    active.indicators.fill(IndicatorState::off);
    active.indicator(definition.id).reset();
    const auto unknown = render_native_cluster(active, now, 1920, 720);
    const bool direction =
        definition.id == Indicator::left_turn || definition.id == Indicator::right_turn;
    check(direction ? unknown.pixels != all_off.pixels && unknown.pixels != lit.pixels
                    : unknown.pixels == all_off.pixels,
          "unknown direction was off/on or unknown warning gained a placeholder");
  }
  for (const auto format : {ClusterCenter::minimal, ClusterCenter::sport, ClusterCenter::gauges}) {
    const auto displayed = render_native_cluster(sample, now, 1920, 720, "", {}, format);
    auto missing = sample;
    missing.speed_kmh.reset();
    missing.engine_rpm.reset();
    const auto unavailable = render_native_cluster(missing, now, 1920, 720, "", {}, format);
    auto zero = missing;
    zero.speed_kmh = 0;
    zero.engine_rpm = 0;
    check(unavailable.pixels != render_native_cluster(zero, now, 1920, 720, "", {}, format).pixels,
          "format replaced unavailable speed/RPM with zero");
    check(displayed.pixels != all_off.pixels, "new format has no distinct presentation");
    NativeClusterConfiguration config;
    config.miles_per_hour = true;
    check(displayed.pixels !=
              render_native_cluster(sample, now, 1920, 720, "", {}, format, false, config).pixels,
          "format ignored configured speed units");
    config.miles_per_hour = false;
    config.show_rpm = false;
    check(displayed.pixels !=
              render_native_cluster(sample, now, 1920, 720, "", {}, format, false, config).pixels,
          "format ignored configured RPM visibility");
    auto active = sample;
    active.indicator(Indicator::left_turn) = IndicatorState::on;
    const auto lit = render_native_cluster(active, now, 1920, 720, "", {}, format);
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (displayed.pixels[index] != lit.pixels[index])
          check(region::indicator_frame(Indicator::left_turn).contains(x, y),
                "format moved turn signals or collided with content");
      }
    auto stale = sample;
    stale.sampled_at = now - std::chrono::seconds(1);
    auto empty = VehicleSample{};
    empty.sampled_at = stale.sampled_at;
    empty.simulated = stale.simulated;
    empty.drive_mode_catalog = stale.drive_mode_catalog;
    check(render_native_cluster(stale, now, 1920, 720, "", {}, format).pixels ==
              render_native_cluster(empty, now, 1920, 720, "", {}, format).pixels,
          "format kept expired gauges");
  }
  {
    auto hazard = sample;
    hazard.indicators.fill(std::nullopt);
    hazard.indicator(Indicator::hazard) = IndicatorState::on;
    const auto bright = render_native_cluster(hazard, now, 1920, 720);
    std::size_t left = 0, right = 0;
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (bright.pixels[index] == all_off.pixels[index]) continue;
        const bool in_left = region::indicator_frame(Indicator::left_turn).contains(x, y);
        const bool in_right = region::indicator_frame(Indicator::right_turn).contains(x, y);
        check(in_left || in_right, "hazards drew a separate lamp or escaped the top arrows");
        left += in_left;
        right += in_right;
      }
    check(left > 60 && right > 60, "hazards did not light both native direction arrows");
    hazard.sampled_at = now + std::chrono::milliseconds(400);
    check(render_native_cluster(hazard, hazard.sampled_at, 1920, 720).pixels == all_off.pixels,
          "native hazard arrows did not go dark together");
  }
  {
    auto gear_sample = sample;
    gear_sample.transmission = TransmissionState{GearboxType::manual, TransmissionSelector::manual,
                                                 EngagedGear{EngagedGearKind::forward, 3}};
    const auto with_gear = render_native_cluster(gear_sample, now, 1920, 720);
    std::size_t changed = 0;
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (with_gear.pixels[index] != all_off.pixels[index]) {
          ++changed;
          check(region::gear.contains(x, y),
                "gear input changed native pixels outside its instrument badge");
        }
      }
    check(changed > 0, "native gear badge did not display known engaged gear");
    gear_sample.transmission = TransmissionState{GearboxType::manual, {}, {}};
    check(render_native_cluster(gear_sample, now, 1920, 720).pixels == all_off.pixels,
          "gearbox type invented a native selected or engaged gear");
    VehicleAgePolicy ages;
    ages.transmission = std::chrono::milliseconds(100);
    VehicleObservationCache cache(ages);
    cache.begin_generation(1, now, now);
    const std::array observations{
        VehicleObservation{"speed_kmh", 40., now},
        VehicleObservation{"transmission",
                           TransmissionState{GearboxType::manual, TransmissionSelector::manual,
                                             EngagedGear{EngagedGearKind::forward, 3}},
                           now}};
    cache.accept(1, 1, observations, now);
    const auto expired = now + std::chrono::milliseconds(100);
    const auto held = cache.sample(expired);
    VehicleSample expected{40., {}, expired};
    check(render_native_cluster(held, expired, 1920, 720).pixels ==
              render_native_cluster(expected, expired, 1920, 720).pixels,
          "native cluster retained a stale gear beside current speed");
  }
  {
    const auto modes = parse_drive_mode_catalog_key("comfort,sport");
    const auto sport = parse_drive_mode_identifier("sport");
    check(modes && sport, "valid native drive-mode fixture rejected");
    auto configured = sample;
    configured.drive_mode_catalog = modes;
    configured.drive_mode.reset();
    const auto unavailable = render_native_cluster(configured, now, 1920, 720);
    configured.drive_mode = sport;
    const auto active = render_native_cluster(configured, now, 1920, 720);
    std::size_t changed = 0;
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (active.pixels[index] != unavailable.pixels[index]) {
          ++changed;
          check(region::drive_mode.contains(x, y),
                "active drive mode changed pixels outside its native badge");
        }
      }
    check(changed > 0, "native drive-mode badge did not display observed selection");
    VehicleAgePolicy ages;
    ages.drive_mode = std::chrono::milliseconds(100);
    VehicleObservationCache cache(ages, modes);
    cache.begin_generation(1, now, now);
    const std::array observations{VehicleObservation{"speed_kmh", 40., now},
                                  VehicleObservation{"drive_mode", *sport, now}};
    cache.accept(1, 1, observations, now);
    const auto expired = now + std::chrono::milliseconds(100);
    const auto held = cache.sample(expired);
    check(held.drive_mode_catalog == modes && !held.drive_mode,
          "drive-mode expiry lost static capabilities or retained active state");
    VehicleSample expected{40., {}, expired};
    expected.drive_mode_catalog = modes;
    check(render_native_cluster(held, expired, 1920, 720).pixels ==
              render_native_cluster(expected, expired, 1920, 720).pixels,
          "native cluster kept expired mode beside fresh speed");
    auto long_mode = sample;
    const std::string long_key(64, 'W');
    long_mode.drive_mode_catalog = parse_drive_mode_catalog_key(long_key);
    long_mode.drive_mode = parse_drive_mode_identifier(long_key);
    check(long_mode.drive_mode_catalog && long_mode.drive_mode,
          "maximum native mode fixture rejected");
    auto long_unknown = long_mode;
    long_unknown.drive_mode.reset();
    const auto long_active = render_native_cluster(long_mode, now, 1920, 720);
    const auto long_unavailable = render_native_cluster(long_unknown, now, 1920, 720);
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (long_active.pixels[index] != long_unavailable.pixels[index])
          check(region::drive_mode.contains(x, y), "long mode label escaped its native badge");
      }
  }
  {
    TripState trip;
    trip.identifier = *parse_trip_identifier("currentTrip");
    trip.reset_generation = 1;
    trip.distance_km = 65.;
    trip.elapsed_seconds = 3900.;
    trip.moving_seconds = 3600.;
    trip.average_speed_kmh = 60.;
    trip.average_speed_basis = TripAverageSpeedBasis::total;
    trip.fuel_efficiency_l_per_100km = 7.4;
    check(valid_trip_state(trip), "native trip fixture is not coherent");
    auto trip_sample = sample;
    const auto empty =
        render_native_cluster(trip_sample, now, 1920, 720, "", {}, ClusterCenter::trip);
    trip_sample.trip = trip;
    const auto supplied =
        render_native_cluster(trip_sample, now, 1920, 720, "", {}, ClusterCenter::trip);
    std::size_t changed = 0;
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (supplied.pixels[index] != empty.pixels[index]) {
          ++changed;
          check(region::center.contains(x, y),
                "trip statistics changed pixels outside native center");
        }
      }
    check(changed > 0, "observed trip statistics did not reach native page");
    {
      // A valid ten-year duration still needs to distinguish its final second;
      // the display must not clip the meaningful suffix of a supplied value.
      auto long_trip = trip_sample;
      long_trip.trip->elapsed_seconds = 315575990.;
      long_trip.trip->average_speed_kmh.reset();
      long_trip.trip->average_speed_basis.reset();
      const auto earlier =
          render_native_cluster(long_trip, now, 1920, 720, "", {}, ClusterCenter::trip);
      long_trip.trip->elapsed_seconds = 315575991.;
      const auto later =
          render_native_cluster(long_trip, now, 1920, 720, "", {}, ClusterCenter::trip);
      check(earlier.pixels != later.pixels,
            "valid long trip duration clipped its distinguishing last second");
    }
    const HeadingState heading{45., HeadingReference::true_north,
                               HeadingSource::vehicle_orientation};
    trip_sample.heading = heading;
    const auto with_heading =
        render_native_cluster(trip_sample, now, 1920, 720, "", {}, ClusterCenter::trip);
    changed = 0;
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (with_heading.pixels[index] != supplied.pixels[index]) {
          ++changed;
          check(region::center.contains(x, y) &&
                    x >= unsigned(region::center.x + region::center.width * 2 / 3) &&
                    y >= unsigned(region::center.y + region::center.height / 2),
                "heading changed pixels outside its native card");
        }
      }
    check(changed > 0, "qualified heading did not reach native page");
    VehicleAgePolicy ages;
    ages.trip = std::chrono::milliseconds(100);
    ages.heading = std::chrono::milliseconds(200);
    VehicleObservationCache cache(ages);
    cache.begin_generation(1, now, now);
    const std::array observations{VehicleObservation{"speed_kmh", 40., now},
                                  VehicleObservation{"trip", trip, now},
                                  VehicleObservation{"heading", heading, now}};
    cache.accept(1, 1, observations, now);
    for (const auto duration : {std::chrono::milliseconds(100), std::chrono::milliseconds(200)}) {
      const auto at = now + duration;
      const auto held = cache.sample(at);
      VehicleSample expected{40., {}, at};
      if (duration.count() < 200) expected.heading = heading;
      check(render_native_cluster(held, at, 1920, 720, "", {}, ClusterCenter::trip).pixels ==
                render_native_cluster(expected, at, 1920, 720, "", {}, ClusterCenter::trip).pixels,
            "native trip/heading expiry renewed another observation or retained stale data");
    }
  }
  auto unknown = sample;
  unknown.indicator(Indicator::check_engine).reset();
  check(render_native_cluster(unknown, now, 1920, 720).pixels == all_off.pixels,
        "unknown warning must stay hidden");
  auto warning = sample;
  warning.indicator(Indicator::check_engine) = IndicatorState::on;
  check(render_native_cluster(warning, now, 1920, 720).pixels != all_off.pixels,
        "active warning missing");
  {
    VehicleAgePolicy ages;
    ages.readings[3] = std::chrono::milliseconds(100);
    ages.indicators[static_cast<std::size_t>(Indicator::check_engine)] =
        std::chrono::milliseconds(100);
    VehicleObservationCache cache(ages);
    cache.begin_generation(1, now, now);
    const std::array initial{VehicleObservation{"speed_kmh", 40., now},
                             VehicleObservation{"coolant_c", 90., now},
                             VehicleObservation{"check_engine", IndicatorState::on, now}};
    cache.accept(1, 1, initial, now);
    const auto newer = now + std::chrono::milliseconds(99);
    const std::array speed{VehicleObservation{"speed_kmh", 50., newer}};
    cache.accept(1, 2, speed, newer);
    const auto held = cache.sample(newer);
    const auto expired = now + std::chrono::milliseconds(100);
    VehicleSample expected{50., {}, expired};  // Only speed remains genuinely current.
    check(render_native_cluster(held, expired, 1920, 720, "", {}, ClusterCenter::vehicle).pixels ==
              render_native_cluster(expected, expired, 1920, 720, "", {}, ClusterCenter::vehicle)
                  .pixels,
          "native gauges/vehicle page retained stale temperature or warning");
    check(render_backup_cluster(held, expired).pixels ==
              render_backup_cluster(expected, expired).pixels,
          "backup display retained stale temperature or warning");
    check(held.observation_timing->readings[3].acquired_at == now,
          "renderer mutated acquisition dates");
  }
  warning.indicator(Indicator::check_engine) = IndicatorState::flashing;
  auto later_phase = now + std::chrono::milliseconds(400);
  warning.sampled_at = later_phase;
  auto off_later = sample;
  off_later.sampled_at = later_phase;
  check(render_native_cluster(warning, later_phase, 1920, 720).pixels ==
            render_native_cluster(off_later, later_phase, 1920, 720).pixels,
        "flashing warning must disappear in its off phase");
  NavigationSnapshot navigation;
  navigation.available = true;
  navigation.route_state = 1;
  navigation.distance_m = 300;
  navigation.maneuver =
      NavigationManeuver{7, 2, "Turn right onto Rue des Lilas", "Rue des Lilas", 90};
  check(render_native_cluster(sample, now, 1920, 720, "", navigation).pixels != baseline.pixels,
        "navigation prompt not drawn");
  {
    const auto glyph = [](const Image& image) {
      std::vector<Rgb> pixels;
      for (unsigned y = region::guidance.y;
           y < unsigned(region::guidance.y + region::guidance.height); ++y)
        for (unsigned x = region::guidance.x; x < unsigned(region::guidance.x + 90); ++x)
          pixels.push_back(image.pixels[std::size_t(y) * image.width + x]);
      return pixels;
    };
    const auto icon = [&](unsigned type) {
      auto nav = navigation;
      nav.maneuver->type = std::uint8_t(type);
      return glyph(render_native_cluster(sample, now, 1920, 720, "", nav));
    };
    for (const auto types : {std::array{1u, 13u, 49u}, std::array{2u, 14u, 50u}}) {
      const auto turn = icon(types[0]), keep = icon(types[1]), slight = icon(types[2]);
      check(turn != keep && turn != slight && keep != slight,
            "native Keep/Slight guidance is indistinguishable from a full turn");
    }
    check(icon(0) == icon(3) && icon(3) == icon(5), "known Continue variants disagree");
    check(icon(4) != icon(255) && icon(11) != icon(0) && icon(10) == icon(12) &&
              icon(10) != icon(255),
          "known U-turn/start/arrival artwork missing or inconsistent");
    check(icon(6) == icon(7) && icon(6) == icon(255) && icon(254) == icon(255),
          "unqualified roundabout/extension geometry invented a direction");
    for (const auto state : {2u, 3u, 4u, 5u}) {
      auto nav = navigation;
      nav.route_state = std::uint8_t(state);
      const auto held = glyph(render_native_cluster(sample, now, 1920, 720, "", nav));
      nav.maneuver.reset();
      check(held == glyph(render_native_cluster(sample, now, 1920, 720, "", nav)),
            "nonactive route status retained a turn arrow");
    }
  }
  char folder[] = "/tmp/headunit-native-test-XXXXXX";
  check(mkdtemp(folder), "temporary directory failed");
  struct Cleanup {
    const char* p;
    ~Cleanup() { std::filesystem::remove_all(p); }
  } cleanup{folder};
  const auto path = std::filesystem::path(folder) / "media.json";
  const auto page_path = std::filesystem::path(folder) / "page.txt";
  check(selected_cluster_center(page_path, true) == ClusterCenter::navigation,
        "connected default page must be map");
  check(selected_cluster_center(page_path, false) == ClusterCenter::media,
        "phone-free default page must be media");
  {
    std::ofstream out(page_path);
    out << "map\n";
  }
  check(selected_cluster_center(page_path, false) == ClusterCenter::media,
        "map page must disappear without CarPlay");
  {
    std::ofstream out(page_path);
    out << "vehicle\n";
  }
  check(selected_cluster_center(page_path, false) == ClusterCenter::vehicle,
        "vehicle page must remain available");
  {
    std::ofstream out(page_path);
    out << "trip\n";
  }
  check(selected_cluster_center(page_path, false) == ClusterCenter::trip &&
            selected_cluster_center(page_path, true) == ClusterCenter::trip,
        "trip page must be available with or without CarPlay");
  for (const auto& [name, format] : std::array{std::pair{"minimal", ClusterCenter::minimal},
                                               std::pair{"sport", ClusterCenter::sport},
                                               std::pair{"gauges", ClusterCenter::gauges}}) {
    {
      std::ofstream output(page_path);
      output << name << '\n';
    }
    check(selected_cluster_center(page_path, false) == format &&
              selected_cluster_center(page_path, true) == format,
          "saved format did not survive phone availability change");
  }

  check(render_native_cluster(sample, now, 1920, 720, "", {}, ClusterCenter::vehicle).pixels !=
            baseline.pixels,
        "vehicle center must replace map");
  {
    std::ofstream out(path);
    out << R"({"available":true,"title":"Test title","artist":"Test artist","updated_monotonic_ms":10000})";
  }
  check(render_native_cluster(sample, now, 1920, 720, path).pixels != baseline.pixels,
        "media not drawn");
  check(render_native_cluster(sample, now, 1920, 720, path, {}, ClusterCenter::media).pixels !=
            baseline.pixels,
        "large media page must replace map");
  const auto later = now + std::chrono::seconds(4);
  sample.sampled_at = later;
  check(render_native_cluster(sample, later, 1920, 720, path).pixels ==
            render_native_cluster(sample, later, 1920, 720).pixels,
        "expired media still visible");
  {
    std::ofstream out(path);
    out << "bad json";
  }
  check(render_native_cluster(sample, later + std::chrono::milliseconds(300), 1920, 720, path)
                .pixels ==
            render_native_cluster(sample, later + std::chrono::milliseconds(300), 1920, 720).pixels,
        "bad media not cleared");
  {
    // Bad shared-state endpoints must not block useful native instruments or
    // turn a linked/oversized snapshot into media or a selected cluster page.
    const auto at = later + std::chrono::milliseconds(600);
    sample.sampled_at = at;
    const auto expected = render_native_cluster(sample, at, 1920, 720).pixels;
    const auto root = std::filesystem::path(folder);
    const auto pipe = root / "state-pipe";
    check(mkfifo(pipe.c_str(), 0600) == 0, "state pipe fixture failed");
    const auto directory = root / "state-directory";
    std::filesystem::create_directory(directory);
    const auto target = root / "valid-media.json";
    {
      std::ofstream out(target);
      out << R"({"available":true,"title":"Linked title","updated_monotonic_ms":14600})";
    }
    check(render_native_cluster(sample, at, 1920, 720, target).pixels != expected,
          "valid media fixture not rendered");
    const auto link = root / "state-link";
    std::filesystem::create_symlink(target, link);
    const auto oversized = root / "oversized-media.json";
    {
      std::ofstream out(oversized);
      out << R"({"available":true,"title":"Oversized title","updated_monotonic_ms":14600})"
          << std::string(32768, ' ');
    }
    for (const auto& invalid : {pipe, directory, link, oversized}) {
      check(render_native_cluster(sample, at, 1920, 720, invalid).pixels == expected,
            "nonregular, linked or oversized media snapshot reached native instruments");
      check(selected_cluster_center(invalid, true) == ClusterCenter::navigation &&
                selected_cluster_center(invalid, false) == ClusterCenter::media,
            "invalid page state did not retain the connected/phone-free default");
    }
    const auto page_target = root / "valid-page";
    {
      std::ofstream out(page_target);
      out << "vehicle\n";
    }
    const auto page_link = root / "page-link";
    std::filesystem::create_symlink(page_target, page_link);
    check(selected_cluster_center(page_target, false) == ClusterCenter::vehicle &&
              selected_cluster_center(page_link, false) == ClusterCenter::media,
          "linked page state was followed");
  }
  {
    // A selected media source can expose playback/progress before track text.
    // Empty metadata is unknown text, not a disconnected/unavailable source.
    const auto root = std::filesystem::path(folder);
    const auto empty = root / "empty-title.json", unknown = root / "unknown-title.json",
               missing_stamp = root / "missing-stamp.json",
               stopped = root / "unavailable-with-title.json";
    const auto at = Clock::time_point{std::chrono::milliseconds(15000)};
    auto media_sample = source.sample(at);
    const auto publish = [&](const std::filesystem::path& file, const std::string& title) {
      std::ofstream out(file);
      out << R"({"available":true,"title":")" << title
          << R"(","artist":"","album":"Fixture album","status":"playing","duration_ms":600000,"position_ms":120000,"updated_monotonic_ms":15000})";
    };
    publish(empty, "");
    publish(unknown, "Unknown title");
    {
      std::ofstream out(stopped);
      out << R"({"available":false,"title":"Previous title","artist":"Previous artist","status":"playing","updated_monotonic_ms":15000})";
    }
    for (const auto center : {ClusterCenter::media, ClusterCenter::navigation})
      for (const bool connected : {false, true}) {
        const auto actual =
            render_native_cluster(media_sample, at, 1920, 720, empty, {}, center, connected);
        const auto expected =
            render_native_cluster(media_sample, at, 1920, 720, unknown, {}, center, connected);
        const auto unavailable =
            render_native_cluster(media_sample, at, 1920, 720, "", {}, center, connected);
        check(actual.pixels == expected.pixels,
              "empty track text discarded a current available media source");
        std::size_t changed = 0;
        for (unsigned y = 0; y < 720; ++y)
          for (unsigned x = 0; x < 1920; ++x) {
            const auto index = std::size_t(y) * 1920 + x;
            if (actual.pixels[index] == unavailable.pixels[index]) continue;
            ++changed;
            const bool inside = center == ClusterCenter::media ? region::center.contains(x, y)
                                                               : region::media.contains(x, y);
            if (!inside)
              throw std::runtime_error(
                  "media availability escaped center=" + std::to_string(int(center)) + " at " +
                  std::to_string(x) + "," + std::to_string(y));
          }
        check(changed > 0, "current available media with unknown track text was redacted");
        check(render_native_cluster(media_sample, at, 1920, 720, stopped, {}, center, connected)
                      .pixels == unavailable.pixels,
              "unavailable media retained previous text or playback status");
        for (const auto date :
             {at - std::chrono::milliseconds(1), at + std::chrono::milliseconds(3001)}) {
          const auto expired =
              render_native_cluster(media_sample, date, 1920, 720, empty, {}, center, connected);
          check(expired.pixels ==
                    render_native_cluster(media_sample, date, 1920, 720, "", {}, center, connected)
                        .pixels,
                "future or expired unknown track metadata stayed visible");
        }
      }
    {
      std::ofstream out(missing_stamp);
      out << R"({"available":true,"title":"Unknown title","status":"playing"})";
    }
    const auto startup = Clock::time_point{std::chrono::milliseconds(500)};
    const auto startup_sample = SimulatedVehicle(startup).sample(startup);
    check(render_native_cluster(startup_sample, startup, 1920, 720, missing_stamp, {},
                                ClusterCenter::media)
                  .pixels == render_native_cluster(startup_sample, startup, 1920, 720, "", {},
                                                   ClusterCenter::media)
                                 .pixels,
          "missing acquisition date became fresh media at startup");
  }
  {
    // libplist accepts escaped NULs but its allocating C-string accessor
    // aborts on them. Malformed fields must leave the gauges and the remaining
    // current media intact, with no prefix posing as a validated filename.
    const auto at = Clock::time_point{std::chrono::milliseconds(15000)};
    const auto observed = source.sample(at);
    const auto root = std::filesystem::path(folder);
    const auto expected_path = root / "sanitized.json";
    {
      std::ofstream out(expected_path);
      out << R"({"available":true,"title":"Unknown title","artist":"","album":"","status":"","updated_monotonic_ms":15000})";
    }
    const auto expected =
        render_native_cluster(observed, at, 1920, 720, expected_path, {}, ClusterCenter::media);
    unsigned index = 0;
    for (const std::string& bad : {std::string(R"(bad\u0000hidden)"), std::string(R"(bad\ud800)"),
                                   std::string(R"(bad\u0009text)"), std::string("bad\xfftext")}) {
      const auto file = root / ("malformed-" + std::to_string(index++) + ".json");
      {
        std::ofstream out(file);
        out << R"({"available":true,"title":")" << bad << R"(","artist":")" << bad
            << R"(","album":")" << bad << R"(","status":")" << bad << R"(","artwork":")" << bad
            << R"(","artwork_id":")" << bad << R"(","updated_monotonic_ms":15000})";
      }
      check(render_native_cluster(observed, at, 1920, 720, file, {}, ClusterCenter::media).pixels ==
                expected.pixels,
            "malformed media string aborted, poisoned Cairo or exposed its valid prefix");
    }
  }
  {
    // Media typography must preserve ordinary long titles, bound UTF-8 at the
    // limit, and leave the map/gauges/warnings outside its own text allocation.
    const auto root = std::filesystem::path(folder);
    const auto at = Clock::time_point{std::chrono::milliseconds(15000)};
    auto observed = source.sample(at);
    const auto publish = [&](const char* name, const std::string& title,
                             const std::string& artist = "", const std::string& album = "") {
      const auto file = root / name;
      std::ofstream out(file);
      out << R"({"available":true,"title":")" << title << R"(","artist":")" << artist
          << R"(","album":")" << album
          << R"(","status":"playing","duration_ms":600000,"position_ms":120000,"updated_monotonic_ms":15000})";
      return file;
    };
    const auto plain = publish("short-title.json", "A short title");
    const auto long_title =
        publish("long-title.json", "Piano Concerto No. 2 in C Minor, Op. 18: II. Adagio sostenuto");
    std::string maximum;
    for (unsigned i = 0; i < 1024; ++i)
      maximum += "é";
    const auto limit = publish("maximum-title.json", maximum);
    const auto short_image =
        render_native_cluster(observed, at, 1920, 720, plain, {}, ClusterCenter::media);
    const auto long_image =
        render_native_cluster(observed, at, 1920, 720, long_title, {}, ClusterCenter::media);
    const auto maximum_image =
        render_native_cluster(observed, at, 1920, 720, limit, {}, ClusterCenter::media);
    // The text column sits right of the artwork; two title lines, then artist/album.
    const cluster_layout::Region title{region::center.x + 356, region::center.y + 100,
                                       region::center.width - 356, 112},
        details{region::center.x + 356, region::center.y + 212, region::center.width - 356, 112};
    std::size_t second_line = 0;
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (long_image.pixels[index] != short_image.pixels[index]) {
          check(title.contains(x, y), "long media title escaped its two-line allocation");
          if (y >= unsigned(title.y + title.height / 2)) ++second_line;
        }
        if (maximum_image.pixels[index] != short_image.pixels[index])
          check(title.contains(x, y), "maximum UTF-8 title escaped or poisoned native drawing");
      }
    check(second_line > 100, "long media title lost its distinguishing second line");
    const auto full = publish("full-metadata.json",
                              "Piano Concerto No. 2 in C Minor, Op. 18: II. Adagio sostenuto",
                              "The Philharmonic Orchestra with the Festival Chamber Players",
                              "Rachmaninoff: Piano Concertos — The Complete Concert Recordings");
    const auto map =
        render_native_cluster(observed, at, 1920, 720, full, {}, ClusterCenter::navigation);
    const auto media =
        render_native_cluster(observed, at, 1920, 720, full, {}, ClusterCenter::media);
    const auto vehicle =
        render_native_cluster(observed, at, 1920, 720, full, {}, ClusterCenter::vehicle);
    const auto trip = render_native_cluster(observed, at, 1920, 720, full, {}, ClusterCenter::trip);
    for (unsigned y = 0; y < 720; ++y)
      for (unsigned x = 0; x < 1920; ++x) {
        const auto index = std::size_t(y) * 1920 + x;
        if (media.pixels[index] != long_image.pixels[index])
          check(details.contains(x, y), "artist/album escaped their bounded media allocation");
        if (region::speed.contains(x, y) || region::power.contains(x, y))
          check(media.pixels[index] == vehicle.pixels[index] &&
                    media.pixels[index] == trip.pixels[index],
                "center page changed an instrument pixel");
      }
    // Map is a transparent layer over the phone's full-width map: its middle
    // stays clear with full metadata, while every other page is opaque.
    std::vector<std::uint32_t> layer(1920 * 720);
    draw_native_cluster({observed, full, {}, ClusterCenter::navigation, true}, at, layer.data(),
                        1920, 720, 1920 * 4);
    for (unsigned y = 200; y < 500; ++y)
      for (unsigned x = 760; x < 1160; ++x)
        check((layer[std::size_t(y) * 1920 + x] >> 24) == 0,
              "native drawing covered the map's center");
    check((layer[std::size_t(region::bottom_bar.y + 50) * 1920 + 960] >> 24) > 200,
          "map page lost its readable bottom bar");
    for (const auto page : {ClusterCenter::media, ClusterCenter::vehicle, ClusterCenter::trip}) {
      draw_native_cluster({observed, full, {}, page, true}, at, layer.data(), 1920, 720, 1920 * 4);
      check(
          std::all_of(layer.begin(), layer.end(), [](std::uint32_t p) { return (p >> 24) == 255; }),
          "a native center page let the hidden map show through");
    }
    if (const auto destination = std::getenv("HEADUNIT_NATIVE_CLUSTER_ARTIFACTS")) {
      const auto save = [&](const char* name, const Image& image) {
        std::ofstream out(std::filesystem::path(destination) / (std::string(name) + ".ppm"),
                          std::ios::binary);
        check(bool(out), "could not write native visual fixture");
        write_ppm(image, out);
      };
      save("media", media);
      save("vehicle", vehicle);
      TripState supplied;
      supplied.identifier = *parse_trip_identifier("currentTrip");
      supplied.reset_generation = 1;
      supplied.distance_km = 65.;
      supplied.elapsed_seconds = 3900.;
      supplied.moving_seconds = 3600.;
      supplied.average_speed_kmh = 60.;
      supplied.average_speed_basis = TripAverageSpeedBasis::total;
      supplied.fuel_efficiency_l_per_100km = 7.4;
      observed.trip = supplied;
      observed.heading =
          HeadingState{45., HeadingReference::true_north, HeadingSource::vehicle_orientation};
      save("trip", render_native_cluster(observed, at, 1920, 720, "", {}, ClusterCenter::trip));
      const VehicleSample missing{{}, {}, at};
      for (const auto& [name, center] : {std::pair{"media-unavailable", ClusterCenter::media},
                                         std::pair{"vehicle-unavailable", ClusterCenter::vehicle},
                                         std::pair{"trip-unavailable", ClusterCenter::trip}})
        save(name, render_native_cluster(missing, at, 1920, 720, "", {}, center));
    }
  }
  {
    // Cover replacement can precede metadata publication. A new cover must
    // never appear under the previous track's hash, including after a decode
    // was cached; a later coherent publication must recover without restart.
    const auto root = std::filesystem::path(folder);
    const auto metadata = root / "art-media.json", plain = root / "plain-media.json",
               cover = root / "cover.png";
    auto at = later + std::chrono::seconds(1);
    sample.sampled_at = at;
    const auto base =
        R"({"available":true,"title":"Artwork title","artist":"Artwork artist","updated_monotonic_ms":15000)";
    {
      std::ofstream out(plain);
      out << base << "}";
    }
    const auto picture = [](double red, double blue) {
      auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
      auto* context = cairo_create(surface);
      cairo_set_source_rgb(context, red, 0, blue);
      cairo_paint(context);
      std::string bytes;
      const auto write = [](void* closure, const unsigned char* data,
                            unsigned length) -> cairo_status_t {
        static_cast<std::string*>(closure)->append(reinterpret_cast<const char*>(data), length);
        return CAIRO_STATUS_SUCCESS;
      };
      const auto status = cairo_surface_write_to_png_stream(surface, write, &bytes);
      cairo_destroy(context);
      cairo_surface_destroy(surface);
      check(status == CAIRO_STATUS_SUCCESS, "artwork PNG fixture failed");
      return bytes;
    };
    const auto digest = [](const std::string& bytes) {
      std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
      SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), hash.data());
      std::string result;
      for (const auto b : hash) {
        result += "0123456789abcdef"[b >> 4];
        result += "0123456789abcdef"[b & 15];
      }
      return result;
    };
    const auto save = [&](const std::string& bytes) {
      std::ofstream out(cover, std::ios::binary);
      out.write(bytes.data(), bytes.size());
    };
    const auto publish = [&](const std::string& name, const std::string& hash) {
      std::ofstream out(metadata);
      out << base << R"(,"artwork":")" << name << R"(","artwork_id":")" << hash << "\"}";
    };
    const auto render = [&](const std::filesystem::path& file) {
      return render_native_cluster(sample, at, 1920, 720, file, {}, ClusterCenter::media).pixels;
    };
    const auto red = picture(1, 0), blue = picture(0, 1);
    save(red);
    publish(cover.filename(), digest(red));
    const auto absent = render(plain), first = render(metadata);
    check(first != absent, "verified artwork was not rendered");
    save(blue);
    at += std::chrono::milliseconds(300);
    const auto stale = render(metadata);
    check(stale == render(plain), "replacement cover appeared under previous track hash");
    publish(cover.filename(), digest(blue));
    at += std::chrono::milliseconds(300);
    const auto recovered = render(metadata);
    check(recovered != render(plain) && recovered != first,
          "coherent artwork publication did not recover");
    std::filesystem::remove(cover);
    at += std::chrono::milliseconds(300);
    const auto missing = render(metadata);
    check(missing == render(plain), "cached artwork survived removal of its verified source");
    save(blue);
    at += std::chrono::milliseconds(300);
    check(render(metadata) == recovered, "matching cover did not recover after source returned");
    publish(cover.filename(), std::string(64, '0'));
    at += std::chrono::milliseconds(300);
    check(render(metadata) == render(plain), "cached artwork survived a mismatching digest");
    const auto link = root / "cover-link.png";
    std::filesystem::create_symlink(cover, link);
    const auto pipe = root / "cover-pipe.png";
    check(mkfifo(pipe.c_str(), 0600) == 0, "cover pipe fixture failed");
    const auto directory = root / "cover-directory";
    std::filesystem::create_directory(directory);
    for (const auto& invalid : {link, pipe, directory}) {
      publish(invalid.filename(), digest(blue));
      at += std::chrono::milliseconds(300);
      check(render(metadata) == render(plain), "linked/nonregular artwork reached the PNG decoder");
    }
    const auto oversized = blue + std::string(1048576, ' ');
    save(oversized);
    publish(cover.filename(), digest(oversized));
    at += std::chrono::milliseconds(300);
    check(render(metadata) == render(plain), "oversized artwork reached the PNG decoder");
  }
  std::cout << "Native cluster map bounds, media and expiry passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
