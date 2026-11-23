// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_configuration.hpp>
#include <headunit/theme/content_control.hpp>
#include <headunit/theme/vehicle_transport.hpp>
#include <headunit/theme/local_vehicle_profile.hpp>
#include <headunit/native_cluster_configuration.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>

namespace {
void native_summary(const headunit::NativeClusterConfiguration& config) {
  if (!headunit::valid_native_cluster_configuration(config))
    throw std::invalid_argument("invalid native instrument summary");
  std::cout.imbue(std::locale::classic());
  std::cout
      << std::setprecision(std::numeric_limits<double>::max_digits10) << "{\"speedUnit\":\""
      << (config.miles_per_hour ? "mph" : "kmh") << "\",\"temperatureUnit\":\""
      << (config.temperature_unit == headunit::NativeTemperatureUnit::fahrenheit ? "fahrenheit"
                                                                                 : "celsius")
      << "\",\"distanceUnit\":\""
      << (config.distance_unit == headunit::NativeDistanceUnit::miles ? "miles" : "kilometers")
      << "\",\"fuelConsumptionUnit\":\""
      << (config.fuel_consumption_unit == headunit::NativeFuelConsumptionUnit::us_mpg ? "us_mpg"
          : config.fuel_consumption_unit == headunit::NativeFuelConsumptionUnit::imperial_mpg
              ? "imperial_mpg"
              : "liters_per_100km")
      << "\",\"speedPrecision\":\""
      << (config.speed_precision == headunit::NativeSpeedPrecision::one_decimal ? "one_decimal"
                                                                                : "whole")
      << "\",\"temperaturePrecision\":\""
      << (config.temperature_precision == headunit::NativeTemperaturePrecision::one_decimal
              ? "one_decimal"
              : "whole")
      << "\",\"pressureUnit\":\""
      << (config.pressure_unit == headunit::NativePressureUnit::bar   ? "bar"
          : config.pressure_unit == headunit::NativePressureUnit::psi ? "psi"
                                                                      : "kpa")
      << "\",\"speedMaximumKmh\":" << config.speed_maximum_kmh
      << ",\"rpmMaximum\":" << config.rpm_maximum << ",\"rpmRedline\":" << config.rpm_redline
      << ",\"showRpm\":" << (config.show_rpm ? "true" : "false") << "}\n";
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc == 2 && std::string_view(argv[1]) == "--native-summary-defaults") {
    native_summary({});
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--native-summary") {
    // The manager passes its already admitted original bytes, rather than a
    // pathname which could select a different file between hash and parsing.
    std::string input(4097, '\0');
    std::cin.read(input.data(), input.size());
    input.resize(std::cin.gcount());
    if (std::cin.bad() || input.size() > 4096)
      throw std::invalid_argument("invalid native instrument summary input");
    native_summary(headunit::parse_native_cluster_configuration(input));
    return 0;
  }
  if (argc < 3 || argc > 8 || (argc > 4 && argc % 2 != 0))
    throw std::invalid_argument(
        "usage: headunit-vehicle-config-check CLIMATE.json|- SETTINGS.json|- [CAMERA.json|-] "
        "[--local-profile PROFILE.plist] [--native-cluster CONFIG.conf]");
  const auto path = [](const char* s) -> std::optional<std::filesystem::path> {
    return std::string_view(s) == "-" ? std::nullopt : std::optional<std::filesystem::path>{s};
  };
  auto config = headunit::theme::vehicle::load_configuration(
      path(argv[1]), path(argv[2]), argc >= 4 ? path(argv[3]) : std::nullopt);
  if (config.content_request)
    headunit::theme::vehicle::validate_content_request_endpoint(
        config.content_request->control_file);
  bool seen_local = false, seen_native = false;
  for (int index = 4; index < argc; index += 2) {
    if (std::string_view(argv[index]) == "--native-cluster" && !seen_native) {
      (void)headunit::load_native_cluster_configuration(argv[index + 1]);
      seen_native = true;
      continue;
    }
    if (std::string_view(argv[index]) != "--local-profile" || seen_local)
      throw std::invalid_argument("unknown or duplicate vehicle checker option");
    seen_local = true;
    std::ifstream stream(argv[index + 1], std::ios::binary);
    if (!stream) throw std::invalid_argument("local profile cannot be opened");
    std::vector<std::uint8_t> bytes(65537);
    stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    bytes.resize(stream.gcount());
    if (bytes.size() > 65536 || stream.bad())
      throw std::invalid_argument("local profile exceeds release bound or could not be read");
    const auto local = headunit::theme::vehicle::decode_local_profile(bytes);
    if (config.content_request) {
      auto state =
          std::make_shared<headunit::theme::vehicle::ContentStateSnapshot>(*config.content_request);
      (void)headunit::theme::vehicle::ContentLocalInput(local, std::move(state));
    }
    std::cout << "Local vehicle profile valid: " << local.bindings.size() << " bindings"
              << (local.identity ? "; configured vehicle identity" : "") << '\n';
  }
  const auto provider = headunit::theme::vehicle::simulated_provider(config);
  headunit::theme::vehicle::ConfiguredModel model(config, provider);
  model.refresh(0);
  (void)model.service().configuration_snapshot(0);
  const auto info =
      headunit::encode_binary_plist(headunit::PlistValue{headunit::PlistValue::Dictionary{}});
  (void)headunit::theme::vehicle::add_server_info(info, model.service(), 0);
  std::cout << "Vehicle capability files valid: " << config.signals.size() << " dynamic bindings, "
            << config.fixed.size() << " constants"
            << (config.experimental ? "; experimental capabilities" : "") << '\n';
  for (const auto& signal : config.signals)
    std::cout << signal.policy.iid << ' ' << signal.key
              << (signal.writable ? " read/write" : " read") << '\n';
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
