// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_hud.hpp>
#include <headunit/vehicle_observation.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F&& action, const char* why) {
  try {
    action();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(why);
}
void write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path);
  out << text;
  out.close();
  check(bool(out), "fixture write failed");
  chmod(path.c_str(), 0600);
}
std::vector<std::uint8_t> word(std::uint16_t v) {
  return {std::uint8_t(v >> 8), std::uint8_t(v)};
}
}  // namespace
namespace speed_limit_race {
using namespace headunit;
std::filesystem::path target;
std::string replacement;
bool injected = false, atomic_replace = false;
ssize_t partial_read(int fd, void* bytes, std::size_t length) {
  if (injected) return ::read(fd, bytes, length);
  struct stat before{};
  check(fstat(fd, &before) == 0, "race snapshot failed");
  const auto count = ::read(fd, bytes, std::min(length, std::size_t(32)));
  check(count == 32, "race fixture did not split the record");
  injected = true;
  // Real file mutation between reads; give the filesystem clock a distinct
  // ctime, then restore mtime so that the old check demonstrably accepts it.
  const timespec delay{0, 20000000};
  nanosleep(&delay, nullptr);
  if (atomic_replace) {
    write(target.parent_path() / "pending", replacement);
    std::filesystem::rename(target.parent_path() / "pending", target);
  } else {
    const int writer = open(target.c_str(), O_WRONLY | O_CLOEXEC);
    check(writer >= 0, "race writer failed");
    check(pwrite(writer, replacement.data(), replacement.size(), 0) == ssize_t(replacement.size()),
          "race rewrite failed");
#ifdef __APPLE__
    const timespec dates[]{before.st_atimespec, before.st_mtimespec};
#else
    const timespec dates[]{before.st_atim, before.st_mtim};
#endif
    check(futimens(writer, dates) == 0, "race mtime restore failed");
    close(writer);
  }
  struct stat after{};
  check(fstat(fd, &after) == 0, "race final stat failed");
#ifdef __APPLE__
  check(before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
            before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec,
        "race did not change ctime");
#else
  check(before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec,
        "race did not change ctime");
#endif
  return count;
}
}  // namespace speed_limit_race
// Compile the actual bounded reader with only its read syscall intercepted.
// The fixture uses real private inodes; no product test hook or fake stat data.
#define headunit speed_limit_race
#define read partial_read
#define fresh fixture_fresh
#include "../src/hud_speed_limit.cpp"
#undef fresh
#undef read
#undef headunit
int main() try {
  using namespace headunit;
  const auto now = Clock::time_point{std::chrono::seconds(10)};
  check(hud_distance_text(0, HudSpeedUnit::kilometres_per_hour) == "0 m" &&
            hud_distance_text(999, HudSpeedUnit::kilometres_per_hour) == "999 m" &&
            hud_distance_text(1000, HudSpeedUnit::kilometres_per_hour) == "1.0 km",
        "metric guidance distance boundary changed");
  check(hud_distance_text(0, HudSpeedUnit::miles_per_hour) == "0 ft" &&
            hud_distance_text(100, HudSpeedUnit::miles_per_hour) == "328 ft" &&
            hud_distance_text(160, HudSpeedUnit::miles_per_hour) == "525 ft" &&
            hud_distance_text(161, HudSpeedUnit::miles_per_hour) == "0.1 mi" &&
            hud_distance_text(1609, HudSpeedUnit::miles_per_hour) == "1.0 mi" &&
            hud_distance_text(std::numeric_limits<std::uint32_t>::max(),
                              HudSpeedUnit::miles_per_hour) == "2668768.9 mi",
        "mph guidance uses metric units or loses zero/bounded source distances");
  HudCalibration direct;
  direct.reflect_horizontal = false;
  NativeHudContent content;
  check(!native_hud_model(content, now).speed_kmh, "missing speed became zero");
  const auto absent = render_native_hud(content, now, 960, 360, direct);
  content.vehicle.speed_kmh = 0;
  content.vehicle.sampled_at = now;
  check(native_hud_model(content, now).speed_kmh == 0, "observed stationary speed lost");
  check(render_native_hud(content, now, 960, 360, direct).pixels != absent.pixels,
        "zero speed indistinguishable from unavailable");
  check(native_hud_model(content, now + std::chrono::milliseconds(500)).speed_kmh == 0,
        "legacy inclusive expiry changed");
  check(!native_hud_model(content, now + std::chrono::milliseconds(501)).speed_kmh,
        "expired legacy speed retained");
  content.vehicle.sampled_at = now + std::chrono::milliseconds(1);
  check(!native_hud_model(content, now).speed_kmh, "future vehicle date accepted");
  content.vehicle.sampled_at = now;
  content.vehicle.speed_kmh = std::numeric_limits<double>::quiet_NaN();
  check(!native_hud_model(content, now).speed_kmh, "invalid speed accepted");
  VehicleAgePolicy ages;
  ages.readings[0] = std::chrono::milliseconds(100);
  VehicleObservationCache cache(ages);
  cache.begin_generation(7, now, now);
  const std::array observations{VehicleObservation{"speed_kmh", 82., now},
                                VehicleObservation{"fuel_percent", 45., now}};
  cache.accept(7, 1, observations, now);
  content.vehicle = cache.sample(now);
  const auto measured = native_hud_model(content, now);
  check(measured.speed_kmh == 82 && measured.speed_timing &&
            measured.speed_timing->acquired_at == now &&
            measured.speed_timing->origin == VehicleObservationOrigin::measured &&
            !measured.simulated,
        "speed provenance was lost");
  check(!native_hud_model(content, now + std::chrono::milliseconds(100)).speed_kmh,
        "fresh unrelated signal renewed expired speed");
  const std::string record =
      "hud-speed-limit-v1 value=50 unit=km/h acquired_ns=10000000000 maximum_age_ns=1000000000 "
      "origin=measured source=vehicle-sign\n";
  const auto limit = parse_hud_speed_limit(record);
  check(limit && limit->kmh == 50 && limit->timing.acquired_at == now &&
            limit->source == "vehicle-sign",
        "explicit speed-limit record lost provenance");
  content.speed_limit = limit;
  check(native_hud_model(content, now).speed_limit.has_value(), "fresh limit rejected");
  check(!native_hud_model(content, now + std::chrono::seconds(1)).speed_limit,
        "limit survived exact expiry");
  check(!native_hud_model(content, now - std::chrono::nanoseconds(1)).speed_limit,
        "future limit accepted");
  auto simulation = *limit;
  simulation.timing.origin = VehicleObservationOrigin::simulation;
  content.speed_limit = simulation;
  check(native_hud_model(content, now).simulated, "simulated speed limit presented as measured");
  content.speed_limit = limit;
  for (const auto& bad :
       {std::string{}, record + "extra", std::string(513, 'x'),
        std::string("hud-speed-limit-v1 value=50 unit=mph acquired_ns=10000000000 "
                    "maximum_age_ns=1000000000 origin=measured source=vehicle-sign"),
        std::string("hud-speed-limit-v1 value=nan unit=km/h acquired_ns=10000000000 "
                    "maximum_age_ns=1000000000 origin=measured source=vehicle-sign"),
        std::string("hud-speed-limit-v1 value=0 unit=km/h acquired_ns=10000000000 maximum_age_ns=0 "
                    "origin=measured source=vehicle-sign")})
    check(!parse_hud_speed_limit(bad), "malformed/unsupported speed-limit record accepted");
  const std::string regional =
      "hud-speed-limit-v2 value=55 unit=mph acquired_ns=10000000000 maximum_age_ns=1000000000 "
      "origin=measured source=road-map country=US";
  const auto us_limit = parse_hud_speed_limit(regional);
  check(us_limit && std::abs(us_limit->kmh - 88.51392) < 1e-8 && us_limit->country == "US" &&
            us_limit->unit == HudSpeedUnit::miles_per_hour && us_limit->timing.acquired_at == now,
        "regional observation lost original units, country or acquisition date");
  content.country = "FR";
  content.speed_limit = us_limit;
  const auto us_model = native_hud_model(content, now);
  check(us_model.locale.sign == HudSpeedSign::us &&
            us_model.locale.unit == HudSpeedUnit::miles_per_hour &&
            std::abs(hud_speed_value(us_model.speed_limit->kmh, us_model.locale.unit) - 55) < 1e-8,
        "fresh provider did not override fallback with correct posted mph");
  const auto us_image = render_native_hud(content, now, 960, 360, direct);
  auto crossing = *us_limit;
  crossing.country = "CA";
  crossing.unit = HudSpeedUnit::kilometres_per_hour;
  crossing.kmh = 80;
  content.speed_limit = crossing;
  check(native_hud_model(content, now).locale.sign == HudSpeedSign::canada &&
            native_hud_model(content, now).locale.unit == HudSpeedUnit::kilometres_per_hour,
        "border observation did not change country and units together");
  check(render_native_hud(content, now, 960, 360, direct).pixels != us_image.pixels,
        "Canadian sign did not change presentation");
  check(!native_hud_model(content, now + std::chrono::seconds(1)).speed_limit &&
            native_hud_model(content, now + std::chrono::seconds(1)).locale.sign ==
                HudSpeedSign::red_ring,
        "expired provider retained country or sign instead of explicit fallback");
  content.speed_limit = limit;
  const auto france = render_native_hud(content, now, 960, 360, direct);
  check(france.at(447, 140) == Rgb{255, 255, 255} && france.at(447, 115).r > 200 &&
            france.at(447, 115).g < 100,
        "French sign lacks white face and red ring");
  content.country = "SE";
  const auto sweden = render_native_hud(content, now, 960, 360, direct);
  check(sweden.at(447, 140).r == 255 && sweden.at(447, 140).g > 190 && sweden.at(447, 140).b == 0,
        "Swedish sign did not use yellow face");
  for (const auto* country : {"FR", "DE", "BE", "ES", "CH", "NZ"})
    check(hud_road_locale(country).sign == HudSpeedSign::red_ring,
          "registered circular profile missing");
  check(hud_road_locale("GB").unit == HudSpeedUnit::miles_per_hour &&
            hud_road_locale("FI").sign == HudSpeedSign::yellow_red_ring,
        "UK/Finland profile incorrect");
  check(hud_road_locale("AU").sign == HudSpeedSign::australia, "Australian panel profile missing");
  crossing.country = "ZZ";
  crossing.unit = HudSpeedUnit::miles_per_hour;
  crossing.kmh = 55 * 1.609344;
  content.speed_limit = crossing;
  check(native_hud_model(content, now).locale.sign == HudSpeedSign::neutral &&
            native_hud_model(content, now).locale.unit == HudSpeedUnit::miles_per_hour,
        "unknown provider country guessed a sign or lost observed units");
  content.country.clear();
  content.speed_limit = limit;
  check(native_hud_model(content, now).locale.sign == HudSpeedSign::neutral,
        "missing country manufactured a regional sign");
  for (const auto& bad : {regional + " extra", regional.substr(0, regional.find(" country=")),
                          regional.substr(0, regional.find("country=")) + "country=us",
                          regional.substr(0, regional.find("country=")) + "country=USA",
                          regional.substr(0, regional.find("country=")) + "country=",
                          regional.substr(0, regional.find("country=")) + "country=US country=CA"})
    check(!parse_hud_speed_limit(bad), "malformed regional road observation accepted");
  auto invalid_region = *limit;
  invalid_region.country = "fr";
  check(!fresh(invalid_region, now), "direct invalid country bypassed record validation");
  const auto unknown =
      parse_hud_speed_limit(regional.substr(0, regional.find("country=")) + "country=unknown");
  check(unknown && unknown->country.empty(), "explicitly unknown country rejected");
  RouteGuidance route({42, "HUD fixture"});
  const std::array maneuver{Iap2Parameter{0, word(42)}, Iap2Parameter{1, word(7)},
                            Iap2Parameter{2, {'K', 'e', 'e', 'p', ' ', 'r', 'i', 'g', 'h', 't'}},
                            Iap2Parameter{3, {14}}};
  check(route.receive({0x5202, encode_iap2_parameters(maneuver)}, now),
        "maneuver fixture rejected");
  const std::array update{
      Iap2Parameter{0, word(42)}, Iap2Parameter{1, {1}}, Iap2Parameter{10, {0, 0, 0, 0}},
      Iap2Parameter{13, word(7)},
      Iap2Parameter{19, {'P', 'h', 'o', 'n', 'e', ' ', 'r', 'o', 'u', 't', 'e'}}};
  check(route.receive({0x5201, encode_iap2_parameters(update)}, now), "route fixture rejected");
  content.navigation = route.snapshot(now + std::chrono::minutes(20));
  auto navigation = native_hud_model(content, now).navigation;
  check(navigation.available && navigation.distance_m == 0 && navigation.maneuver &&
            navigation.maneuver->type == 14 && navigation.source == "Phone route" &&
            navigation.updated_at == now,
        "HUD invented heartbeat expiry or discarded real zero distance/provenance");
  const auto active = render_native_hud(content, now, 960, 360, direct);
  route.clear();
  content.navigation = route.snapshot(now);
  check(!native_hud_model(content, now).navigation.available, "retired session route retained");
  check(render_native_hud(content, now, 960, 360, direct).pixels != active.pixels,
        "HUD ignored current route withdrawal");
  content.navigation = navigation;
  content.navigation.route_state = 2;
  const auto arrived = native_hud_model(content, now).navigation;
  check(arrived.available && !arrived.distance_m && !arrived.maneuver,
        "arrival retained a next-turn instruction");
  content.navigation = navigation;
  content.navigation.available = false;
  check(!native_hud_model(content, now).navigation.maneuver,
        "unavailable route retained a maneuver");
  content.navigation = navigation;
  content.navigation.updated_at = now + std::chrono::milliseconds(1);
  check(!native_hud_model(content, now).navigation.available, "future route provenance accepted");
  content.navigation = navigation;
  content.navigation.source = std::string(10000, 'x');
  content.navigation.maneuver->instruction = std::string(255, 'x') + "\xc3\xa9";
  content.navigation.maneuver->road = "\xf0\x28\x8c\xbc";
  const auto bounded = native_hud_model(content, now).navigation;
  check(bounded.source.size() == 256 && bounded.maneuver->instruction.size() == 255 &&
            bounded.maneuver->road.empty(),
        "navigation strings unbounded or malformed UTF-8 retained");
  content.navigation = navigation;
  const auto image = render_native_hud(content, now, 960, 360, direct);
  for (unsigned y = 225; y < 245; ++y)
    for (unsigned x = 330; x < 380; ++x)
      check(image.at(x, y) == Rgb{0, 0, 0}, "speed-limit sign connected to a previous text path");
  const auto reflected = render_native_hud(content, now);
  for (unsigned y = 0; y < 360; ++y)
    for (unsigned x = 0; x < 960; ++x)
      check(image.at(x, y) == reflected.at(959 - x, y),
            "default reflection changed content or reversed wrong axis");
  HudCalibration vertical = direct;
  vertical.reflect_vertical = true;
  const auto flipped = render_native_hud(content, now, 960, 360, vertical);
  for (unsigned y = 0; y < 360; ++y)
    for (unsigned x = 0; x < 960; ++x)
      check(image.at(x, y) == flipped.at(x, 359 - y), "vertical reflection changed content");
  const auto portrait = render_native_hud(content, now, 360, 960, direct);
  HudCalibration rotated = direct;
  rotated.quarter_turns = 1;
  const auto turned = render_native_hud(content, now, 960, 360, rotated);
  for (unsigned y = 0; y < 360; ++y)
    for (unsigned x = 0; x < 960; ++x)
      check(turned.at(x, y) == portrait.at(y, 959 - x),
            "clockwise rotation distorted the fitted canvas");
  HudCalibration keystone = direct;
  keystone.corners = std::array<HudPoint, 4>{{{.2, .1}, {.8, .1}, {.95, .9}, {.05, .9}}};
  check(valid_hud_calibration(keystone), "valid measured convex quad rejected");
  const auto warped = render_native_hud(content, now, 960, 360, keystone);
  check(warped.at(0, 0) == Rgb{0, 0, 0} && warped.at(959, 359) == Rgb{0, 0, 0},
        "calibration leaked light outside measured quad");
  check(std::any_of(warped.pixels.begin(), warped.pixels.end(),
                    [](auto p) { return p != Rgb{0, 0, 0}; }),
        "projective calibration lost the HUD");
  check(render_native_hud(content, now, 960, 360, keystone).pixels == warped.pixels,
        "projective calibration nondeterministic");
  const auto grid = render_hud_calibration_grid(960, 360, direct);
  const auto mirrored_grid = render_hud_calibration_grid(960, 360);
  check(grid.pixels != image.pixels, "calibration grid contains vehicle claims");
  for (unsigned y = 0; y < 360; ++y)
    for (unsigned x = 0; x < 960; ++x)
      check(grid.at(x, y) == mirrored_grid.at(959 - x, y),
            "calibration grid uses a different reflection transform");
  HudCalibration positioned = direct;
  positioned.scale = .5;
  positioned.offset_x = .2;
  const auto placed = render_native_hud(content, now, 960, 360, positioned);
  check(placed.at(0, 180) == Rgb{0, 0, 0} && placed.pixels != image.pixels,
        "position/scale calibration ignored");
  std::array<HudCalibration, 4> bad_affine{};
  bad_affine[0].quarter_turns = 4;
  bad_affine[1].scale = 0;
  bad_affine[2].offset_x = 1;
  bad_affine[3].scale = std::numeric_limits<double>::quiet_NaN();
  for (const auto& bad : bad_affine)
    check(!valid_hud_calibration(bad), "invalid affine calibration accepted");
  auto crossed = direct;
  crossed.corners = std::array<HudPoint, 4>{{{0, 0}, {1, 1}, {1, 0}, {0, 1}}};
  check(!valid_hud_calibration(crossed), "crossed quad accepted");
  auto collapsed = direct;
  collapsed.corners = std::array<HudPoint, 4>{{{0, 0}, {.1, 0}, {.2, 0}, {.3, 0}}};
  check(!valid_hud_calibration(collapsed), "singular quad accepted");
  rejects([&] { render_native_hud(content, now, 960, 360, crossed); },
          "invalid quad reached drawing");
  rejects([&] { render_native_hud(content, now, 4097, 360); }, "oversized HUD allocated");
  std::vector<std::uint32_t> padded(964 * 360, 0x12345678);
  draw_native_hud(content, now, padded.data(), 960, 360, 964 * 4, direct);
  for (unsigned y = 0; y < 360; ++y)
    for (unsigned x = 960; x < 964; ++x)
      check(padded[y * 964 + x] == 0x12345678, "HUD wrote row padding");
  rejects([&] { draw_native_hud(content, now, padded.data(), 960, 360, 960 * 4 + 1); },
          "misaligned HUD stride accepted");
  char directory[] = "/tmp/headunit-hud-XXXXXX";
  check(mkdtemp(directory), "private fixture directory failed");
  const std::filesystem::path root = std::filesystem::canonical(directory);
  struct Remove {
    std::filesystem::path p;
    ~Remove() { std::filesystem::remove_all(p); }
  } cleanup{root};
  const auto file = root / "speed-limit";
  write(file, record);
  check(read_hud_speed_limit(file.string(), now).has_value(), "private dated limit file rejected");
  write(file, regional);
  const auto regional_file = read_hud_speed_limit(file.string(), now);
  check(regional_file && regional_file->country == "US" &&
            std::abs(regional_file->kmh - 88.51392) < 1e-8,
        "actual private regional provider file rejected");
  write(file, record);
  check(!read_hud_speed_limit(file.string(), now + std::chrono::seconds(1)),
        "reader renewed acquisition date");
  const auto link = root / "link";
  std::filesystem::create_symlink(file, link);
  check(!read_hud_speed_limit(link.string(), now), "speed-limit symlink accepted");
  const auto hard = root / "hard";
  std::filesystem::create_hard_link(file, hard);
  check(!read_hud_speed_limit(file.string(), now), "speed-limit hardlink accepted");
  std::filesystem::remove(hard);
  chmod(file.c_str(), 0644);
  check(!read_hud_speed_limit(file.string(), now), "public speed-limit file accepted");
  chmod(file.c_str(), 0600);
  chmod(root.c_str(), 0755);
  check(!read_hud_speed_limit(file.string(), now), "public provider directory accepted");
  chmod(root.c_str(), 0700);
  const auto fifo = root / "fifo";
  check(mkfifo(fifo.c_str(), 0600) == 0, "fifo fixture failed");
  check(!read_hud_speed_limit(fifo.string(), now), "blocking device accepted");
  write(root / "replacement", record);
  std::filesystem::rename(root / "replacement", file);
  check(read_hud_speed_limit(file.string(), now).has_value(),
        "atomic provider replacement rejected");
  const std::string expired =
      "hud-speed-limit-v1 value=50 unit=km/h acquired_ns=8000000000 maximum_age_ns=1000000000 "
      "origin=measured source=fixture\n";
  speed_limit_race::replacement =
      "hud-speed-limit-v1 value=80 unit=km/h acquired_ns=9999999999 maximum_age_ns=1000000000 "
      "origin=measured source=fixture\n";
  check(expired.size() == speed_limit_race::replacement.size(), "race records must share length");
  speed_limit_race::target = file;
  speed_limit_race::injected = false;
  write(file, expired);
  check(!speed_limit_race::read_hud_speed_limit(file.string(), now),
        "mixed old limit/new acquisition date accepted");
  const auto complete = read_hud_speed_limit(file.string(), now);
  check(complete && complete->kmh == 80, "next poll did not read complete replacement");
  const std::string fresh_old =
      "hud-speed-limit-v1 value=50 unit=km/h acquired_ns=9999999999 maximum_age_ns=1000000000 "
      "origin=measured source=fixture\n";
  speed_limit_race::injected = false;
  speed_limit_race::atomic_replace = true;
  write(file, fresh_old);
  check(!speed_limit_race::read_hud_speed_limit(file.string(), now),
        "retired inode mutation accepted during acquisition");
  const auto atomic_complete = read_hud_speed_limit(file.string(), now);
  check(atomic_complete && atomic_complete->kmh == 80, "next atomic replacement could not be read");
  write(file, std::string(513, 'x'));
  check(!read_hud_speed_limit(file.string(), now), "oversized limit file accepted");
  std::cout
      << "HUD freshness, provenance, route retirement, calibration and private records passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
