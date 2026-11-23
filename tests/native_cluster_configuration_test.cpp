// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster.hpp>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid native cluster configuration accepted");
}
}  // namespace
int main() try {
  using namespace headunit;
  const std::string valid =
      "version=1\nspeed_unit=mph\nspeed_maximum_kmh=160\n"
      "rpm_maximum=5000\nrpm_redline=4000\nshow_rpm=true\n";
  const auto config = parse_native_cluster_configuration(valid);
  check(config.miles_per_hour && config.speed_maximum_kmh == 160 && config.rpm_maximum == 5000 &&
            config.rpm_redline == 4000 && config.show_rpm &&
            config.distance_unit == NativeDistanceUnit::kilometers,
        "explicit native gauge parameters lost");
  check(config.temperature_unit == NativeTemperatureUnit::celsius,
        "old version-1 configuration lost default Celsius");
  for (const auto unit : {"celsius", "fahrenheit"}) {
    const auto optional = valid + "temperature_unit=" + unit + "\n";
    check(parse_native_cluster_configuration(optional).temperature_unit ==
              (std::string_view(unit) == "celsius" ? NativeTemperatureUnit::celsius
                                                   : NativeTemperatureUnit::fahrenheit),
          "explicit native temperature unit lost");
    check(parse_native_cluster_configuration(optional +
                                             "drive_mode_profiles_file=/private/profiles\n")
                  .drive_mode_profiles_file == "/private/profiles",
          "optional temperature unit displaced existing drive profiles");
  }
  for (const auto value : {"", "F", "C", "Fahrenheit", "kelvin", "fahrenheit ", "true", "1"})
    rejects([&] {
      (void)parse_native_cluster_configuration(valid + "temperature_unit=" + value + "\n");
    });
  rejects([&] {
    (void)parse_native_cluster_configuration(
        valid + "temperature_unit=celsius\ntemperature_unit=fahrenheit\n");
  });
  rejects([&] {
    (void)parse_native_cluster_configuration(
        valid +
        "temperature_unit=celsius\ndrive_mode_profiles_file=/private/profiles\nunknown=1\n");
  });
  for (const auto& [key, unit] : {std::pair{"kilometers", NativeDistanceUnit::kilometers},
                                  std::pair{"miles", NativeDistanceUnit::miles}}) {
    const auto configured = parse_native_cluster_configuration(
        valid + "distance_unit=" + key + "\ndrive_mode_profiles_file=/private/profiles.json\n");
    check(configured.miles_per_hour && configured.distance_unit == unit &&
              configured.drive_mode_profiles_file == "/private/profiles.json",
          "independent optional distance/profile configuration lost");
  }
  for (const auto& suffix :
       {"distance_unit=km\n", "distance_unit=\n", "distance_unit=Miles\n", "distance_unit=null\n",
        "distance_unit=miles\ndistance_unit=miles\n", "distance_unit=miles\nunknown=1\n"})
    rejects([&] { (void)parse_native_cluster_configuration(valid + suffix); });
  for (const auto field : {"version=1\n", "speed_unit=mph\n", "speed_maximum_kmh=160\n",
                           "rpm_maximum=5000\n", "rpm_redline=4000\n", "show_rpm=true\n"}) {
    auto missing = valid + "distance_unit=miles\n";
    missing.erase(missing.find(field), std::string_view(field).size());
    rejects([&] { (void)parse_native_cluster_configuration(missing); });
  }
  auto bad_enum = config;
  bad_enum.distance_unit = static_cast<NativeDistanceUnit>(99);
  check(!valid_native_cluster_configuration(bad_enum), "invalid distance enum accepted");
  for (const auto speed : {"kmh", "mph"}) {
    for (const auto temperature : {"", "celsius", "fahrenheit"}) {
      for (const auto distance : {"", "kilometers", "miles"}) {
        for (const bool profile : {false, true}) {
          auto combined = valid;
          combined.replace(combined.find("speed_unit=mph"), 14, std::string("speed_unit=") + speed);
          if (std::string_view(temperature) != "")
            combined += std::string("temperature_unit=") + temperature + "\n";
          if (std::string_view(distance) != "")
            combined += std::string("distance_unit=") + distance + "\n";
          if (profile) combined += "drive_mode_profiles_file=/private/profiles\n";
          const auto parsed = parse_native_cluster_configuration(combined);
          check(parsed.miles_per_hour == (std::string_view(speed) == "mph") &&
                    parsed.temperature_unit == (std::string_view(temperature) == "fahrenheit"
                                                    ? NativeTemperatureUnit::fahrenheit
                                                    : NativeTemperatureUnit::celsius) &&
                    parsed.distance_unit == (std::string_view(distance) == "miles"
                                                 ? NativeDistanceUnit::miles
                                                 : NativeDistanceUnit::kilometers) &&
                    parsed.drive_mode_profiles_file == (profile ? "/private/profiles" : ""),
                "combined optional fields changed independent configuration");
        }
      }
    }
  }
  const auto nine_fields = valid +
                           "temperature_unit=fahrenheit\ndistance_unit=miles\n"
                           "drive_mode_profiles_file=/private/profiles\n";
  for (const auto suffix : {"unknown=1\n", "distance_unit=miles\n", "temperature_unit=celsius\n",
                            "drive_mode_profiles_file=/private/other\n"})
    rejects([&] { (void)parse_native_cluster_configuration(nine_fields + suffix); });
  for (const auto field : {"version=1\n", "speed_unit=mph\n", "speed_maximum_kmh=160\n",
                           "rpm_maximum=5000\n", "rpm_redline=4000\n", "show_rpm=true\n"}) {
    auto missing = nine_fields;
    missing.erase(missing.find(field), std::string_view(field).size());
    rejects([&] { (void)parse_native_cluster_configuration(missing); });
  }
  check(
      parse_native_cluster_configuration(nine_fields + std::string(4096 - nine_fields.size(), '\n'))
              .distance_unit == NativeDistanceUnit::miles,
      "combined configuration at original bound rejected");
  rejects([&] {
    (void)parse_native_cluster_configuration(nine_fields +
                                             std::string(4097 - nine_fields.size(), '\n'));
  });
  {
    char directory[] = "/tmp/headunit-native-config-XXXXXX";
    check(mkdtemp(directory), "temporary native configuration directory failed");
    const std::filesystem::path root(directory), file = root / "native.conf";
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{root};
    const auto write = [&](const std::filesystem::path& path, const std::string& text) {
      std::ofstream stream(path, std::ios::binary);
      stream << text;
      stream.close();
      check(bool(stream), "native configuration publication failed");
    };
    write(file, valid);
    check(load_native_cluster_configuration(file).miles_per_hour,
          "bounded native configuration file was not admitted");
    write(root / "replacement", valid);
    std::filesystem::rename(root / "replacement", file);
    check(load_native_cluster_configuration(file).rpm_maximum == 5000,
          "complete atomic native configuration replacement was not admitted");
    const auto link = root / "link", oversized = root / "oversized", fifo = root / "fifo";
    std::filesystem::create_symlink(file, link);
    write(oversized, valid + std::string(4096, ' '));
    check(mkfifo(fifo.c_str(), 0600) == 0, "native configuration pipe fixture failed");
    for (const auto& path : {root, link, oversized}) {
      bool rejected = false;
      try {
        (void)load_native_cluster_configuration(path);
      } catch (const std::exception&) {
        rejected = true;
      }
      check(rejected, "nonregular, linked or oversized native configuration admitted");
    }
    // Bound the regression itself: an old blocking loader must fail this test
    // instead of hanging CI waiting for a nonexistent pipe writer.
    const auto child = fork();
    check(child >= 0, "native configuration reader fixture could not fork");
    if (!child) {
      try {
        (void)load_native_cluster_configuration(fifo);
      } catch (const std::exception&) {
        _exit(0);
      }
      _exit(1);
    }
    int status = 0;
    pid_t completed = 0;
    for (unsigned attempt = 0; attempt < 100 && !completed; ++attempt) {
      completed = waitpid(child, &status, WNOHANG);
      if (!completed) usleep(5000);
    }
    if (!completed) {
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
    }
    check(completed == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "native configuration pipe blocked startup or was admitted");
  }
  for (const auto& bad :
       {std::string{}, valid + "unknown=1\n", valid + "version=1\n", valid + std::string(4096, 'x'),
        valid + std::string(1, '\0'), std::string("version=1\n"), valid + "\r"})
    rejects([&] { (void)parse_native_cluster_configuration(bad); });
  for (const auto& [from, to] : {std::pair{"version=1", "version=2"},
                                 {"speed_unit=mph", "speed_unit=knots"},
                                 {"speed_maximum_kmh=160", "speed_maximum_kmh=0"},
                                 {"speed_maximum_kmh=160", "speed_maximum_kmh=nan"},
                                 {"speed_maximum_kmh=160", "speed_maximum_kmh=801"},
                                 {"rpm_maximum=5000", "rpm_maximum=100rpm"},
                                 {"rpm_maximum=5000", "rpm_maximum=3999"},
                                 {"rpm_redline=4000", "rpm_redline=-1"},
                                 {"show_rpm=true", "show_rpm=1"}}) {
    auto bad = valid;
    bad.replace(bad.find(from), std::string_view(from).size(), to);
    rejects([&] { (void)parse_native_cluster_configuration(bad); });
  }
  const auto now = Clock::time_point{std::chrono::milliseconds(10000)};
  SimulatedVehicle source(now);
  auto sample = source.sample(now);
  sample.speed_kmh = 80;
  sample.engine_rpm = 2500;
  const auto draw = [&](const NativeClusterConfiguration& gauge, const VehicleSample& value) {
    return render_native_cluster(value, now, 1920, 720, {}, {}, ClusterCenter::vehicle, false,
                                 gauge);
  };
  const auto baseline = draw({}, sample);
  const auto changed = draw(config, sample);
  check(baseline.pixels != changed.pixels, "configured gauges did not affect rendering");
  auto no_rpm = config;
  no_rpm.show_rpm = false;
  const auto hidden = draw(no_rpm, sample);
  std::size_t difference = 0;
  for (unsigned y = 0; y < hidden.height; ++y)
    for (unsigned x = 0; x < hidden.width; ++x) {
      const auto index = std::size_t(y) * hidden.width + x;
      if (hidden.pixels[index] == changed.pixels[index]) continue;
      ++difference;
      // Existing outer ticks extend slightly above the nominal instrument region.
      check(x >= 1380 && x < 1860 && y >= 80 && y < 566,
            "RPM visibility changed unrelated content");
    }
  check(difference > 1000, "hidden RPM gauge retained its artwork");
  sample.speed_kmh.reset();
  sample.engine_rpm.reset();
  const auto unavailable = draw(config, sample);
  sample.speed_kmh = 0;
  sample.engine_rpm = 0;
  check(unavailable.pixels != draw(config, sample).pixels,
        "configuration replaced missing readings with zero");
  auto invalid = config;
  invalid.rpm_maximum = std::numeric_limits<double>::infinity();
  rejects([&] { (void)draw(invalid, sample); });
  invalid = config;
  invalid.temperature_unit = static_cast<NativeTemperatureUnit>(99);
  rejects([&] { (void)draw(invalid, sample); });
  rejects([&] { (void)draw(bad_enum, sample); });
  std::cout << "Native gauge configuration, omission and unavailable rendering passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
