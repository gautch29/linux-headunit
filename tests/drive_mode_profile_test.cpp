// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/drive_mode_profile.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/theme/drive_mode_control.hpp>
#include <headunit/native_cluster.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

using namespace headunit;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
const std::string layout = "8BC9ABB1-CB20-595E-92BF-D8F099BF3515";
std::string document(std::string selected = "sport", std::string accent = "99CCFF",
                     std::string wallpaper = "phone") {
  const auto row =
      "sport\tSport\t" + layout + "\tsport\tBlue\t" + accent + "\t" + wallpaper + "\ttrue\n";
  return "HUDP1\t44C9FA31EC:985\t1\t" + selected + "\nmode\t" + row + "base\t" + row;
}
void write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path);
  out << text;
  out.close();
  check(bool(out), "fixture write failed");
  check(chmod(path.c_str(), 0600) == 0, "fixture chmod failed");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 4 && std::string_view(argv[1]) == "--publish-status") {
      const auto acquired = Clock::now();
      DriveModeProfilesFile profiles(argv[2], "44C9FA31EC:985");
      VehicleSample sample;
      if (std::string_view(argv[3]) == "vehicle") {
        VehicleAgePolicy ages;
        ages.drive_mode = 5s;
        VehicleObservationCache cache(ages, parse_drive_mode_catalog_key("sport"));
        cache.begin_generation(1, acquired, acquired);
        const std::array batch{VehicleObservation{"drive_mode",
                                                  *parse_drive_mode_identifier("sport"), acquired,
                                                  VehicleObservationOrigin::simulation}};
        cache.accept(1, 1, batch, acquired);
        sample = cache.sample(acquired);
      }
      const auto mode = profiles.poll(sample, acquired);
      check(mode.profile.has_value(), "interop profile unavailable");
      publish_drive_mode_status(std::string(argv[2]) + ".ultra.status", profiles.scope(), mode,
                                "confirmed", "", acquired, std::string(32, 'a'), 7);
      return 0;
    }
    const auto config = parse_drive_mode_profiles(document());
    const auto now = Clock::now();
    auto mode = resolve_drive_mode(config, {}, now);
    check(mode.profile && mode.source == "user" && !mode.timing,
          "user display intent invented car state");
    VehicleAgePolicy ages;
    ages.drive_mode = 250ms;
    VehicleObservationCache car(ages, parse_drive_mode_catalog_key("sport,eco"));
    car.begin_generation(1, now, now);
    std::array observation{
        VehicleObservation{"drive_mode", *parse_drive_mode_identifier("sport"), now}};
    car.accept(1, 1, observation, now);
    const auto original = car.sample(now);
    mode = resolve_drive_mode(config, original, now + 249ms);
    check(mode.profile && mode.source == "vehicle" && mode.timing &&
              mode.timing->acquired_at == now && mode.vehicle_generation == 1,
          "vehicle priority/date/generation lost");
    mode = resolve_drive_mode(config, original, now + 250ms);
    check(mode.source == "user" && !mode.timing, "expiry renewed the vehicle observation");
    auto simulated = original;
    simulated.simulated = true;
    simulated.observation_timing->drive_mode.origin = VehicleObservationOrigin::simulation;
    const auto explicit_user = parse_drive_mode_profiles(document());
    check(resolve_drive_mode(explicit_user, simulated, now).source == "user",
          "bench simulation blocked the user's display profile");
    const auto no_user = parse_drive_mode_profiles(document("-"));
    check(resolve_drive_mode(no_user, simulated, now).source == "simulation",
          "fixture mode was misreported as a real car observation");
    check(!resolve_drive_mode(no_user, original, now + 250ms).profile,
          "missing mode became a default mode");
    observation[0] = {"drive_mode", *parse_drive_mode_identifier("eco"), now + 1ms};
    car.accept(1, 2, observation, now + 1ms);
    mode = resolve_drive_mode(config, car.sample(now + 1ms), now + 1ms);
    check(!mode.profile && mode.source == "unmapped",
          "unmapped fresh car mode fell back to conflicting user choice");
    for (const auto& malformed : {document().substr(1), document() + document(), document("absent"),
                                  document("sport", "red")}) {
      bool rejected = false;
      try {
        (void)parse_drive_mode_profiles(malformed);
      } catch (const std::exception&) {
        rejected = true;
      }
      check(rejected, "malformed profile admitted");
    }
    auto root = std::filesystem::temp_directory_path() /
                ("drive-mode-test-" + std::to_string(now.time_since_epoch().count()));
    std::filesystem::create_directory(root);
    root = std::filesystem::canonical(root);
    chmod(root.c_str(), 0700);
    struct Cleanup {
      std::filesystem::path root;
      ~Cleanup() { std::filesystem::remove_all(root); }
    } cleanup{root};
    const auto path = root / "profiles";
    write(path, document());
    DriveModeProfilesFile file(path, "44C9FA31EC:985");
    check(file.poll({}, now).source == "user", "private profile did not load");
    write(path, "corrupt");
    check(!file.poll({}, now + 250ms).profile && !file.error().empty(),
          "corrupt replacement retained intent");
    write(path, document());
    using namespace headunit::theme::script;
    LayoutSelection state;
    state.ready = state.idle = true;
    state.choices = {{layout, "DigiRing"}};
    state.palettes = {{"Blue", "Blue"}};
    state.active = "0D38B526-1271-575D-8A92-393A86B718F9";
    state.active_palette = "Blue";
    unsigned requests = 0;
    const std::string nonce(32, 'a');
    DriveModeControl control(path, "44C9FA31EC:985", {nonce, 7});
    const auto select = [&](const std::optional<ClusterSelectionRequest>& request) {
      if (request) {
        ++requests;
        check(request->layout == layout && request->palette == "Blue",
              "profile split into independent requests");
        state.result = LayoutRequest::requested;
      }
      return state;
    };
    control.poll({}, now, select);
    control.poll({}, now + 250ms, select);
    check(requests == 1, "pending request repeated");
    const auto status_text = [&] {
      std::ifstream status(path.string() + ".ultra.status");
      return std::string(std::istreambuf_iterator<char>(status), {});
    };
    check(status_text().find("\"nonce\":\"" + nonce + "\"") != std::string::npos &&
              status_text().find("\"connectionGeneration\":\"7\"") != std::string::npos,
          "status omitted the actual connection publisher");
    check(status_text().find("\"phase\":\"pending\"") != std::string::npos,
          "requested profile claimed confirmation");
    control.poll({}, now + 5250ms, select);
    check(status_text().find("\"phase\":\"error\"") != std::string::npos && requests == 1,
          "unconfirmed profile did not time out or retried");
    state.active = layout;
    control.poll({}, now + 5500ms, select);
    state.active_palette = "Red";
    control.poll({}, now + 6s, select);
    check(requests == 1, "profile fights phone Customize");
    // Actual native pixels: one buffer adopts accent/backdrop, and malformed
    // replacement returns to the established native frame without throwing.
    NativeClusterConfiguration native;
    native.drive_mode_profiles_file = path;
    const auto blue =
        render_native_cluster({}, now, 960, 360, {}, {}, ClusterCenter::vehicle, false, native);
    write(path, document("sport", "FF0000", "red"));
    const auto red = render_native_cluster({}, now + 250ms, 960, 360, {}, {},
                                           ClusterCenter::vehicle, false, native);
    check(blue.pixels != red.pixels, "native profile did not switch pixels");
    write(path, "corrupt");
    const auto withdrawn = render_native_cluster({}, now + 500ms, 960, 360, {}, {},
                                                 ClusterCenter::vehicle, false, native);
    const auto baseline =
        render_native_cluster({}, now + 500ms, 960, 360, {}, {}, ClusterCenter::vehicle);
    check(withdrawn.pixels == baseline.pixels, "retired profile leaked into native fallback");
    std::cout << "drive-mode profiles passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
