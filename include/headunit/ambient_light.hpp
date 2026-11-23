// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/night_mode.hpp>
#include <headunit/vehicle_commands.hpp>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace headunit::ambient {
using Colour = std::array<std::uint8_t, 4>;
using Value = vehicle_control::AmbientLightValue;
enum class Selection { automatic, screen, drive_mode, fixed };
enum class Source { screen, drive_mode, fixed, disabled };
enum class NightSource { policy, headlights, schedule };
struct ModeColour {
  DriveModeIdentifier mode{};
  Colour rgbw{};
};
struct Settings {
  bool enabled = false;
  Selection selection = Selection::automatic;
  Colour fixed_rgbw{32, 96, 255, 0};
  unsigned day_brightness_permille = 300;
  unsigned night_brightness_permille = 60;
  unsigned transition_ms = 800;
  NightModePolicy night_policy{NightModePolicy::Kind::headlights};
  std::array<ModeColour, 32> mode_colours{};
  std::size_t mode_count = 0;
};
bool valid_settings(const Settings&) noexcept;
Settings parse_settings(std::string_view json);
// Screen authority is a currently observed/confirmed palette, never a button
// request or remembered phone choice. Keep original publisher generation/date.
struct Accent {
  Colour rgbw{};
  std::uint64_t generation = 0;
  Clock::time_point started_at{};
  VehicleSignalTiming timing{};
};
struct Inputs {
  std::optional<Accent> screen{};
  VehicleSample vehicle{};
  // Real local wall clock supplied by the owner. Missing means unavailable
  // schedule; it never defaults to midnight.
  std::optional<unsigned> local_minute{};
};
struct Evidence {
  std::uint64_t generation = 0;
  VehicleSignalTiming timing{};
};
struct Target {
  Clock::time_point derived_at{}, valid_until{};
  Value value{};
  Source source = Source::disabled;
  std::optional<Evidence> colour_observation{};
  std::optional<Evidence> headlights_observation{};
  bool night = false;
  NightSource night_source = NightSource::policy;
};
std::optional<Target> derive(const Settings&, const Inputs&, Clock::time_point now);
// Dedicated ambient target/coordinator on one owner thread. One in-flight
// command, latest target coalesced by recomputation, no physical I/O or retry.
class Controller {
 public:
  Controller(Settings, vehicle_control::TargetPolicy);
  void begin_generation(std::uint64_t, Clock::time_point started_at, Clock::time_point now);
  void retire(Clock::time_point now);
  void settings(Settings);
  std::optional<vehicle_control::Request> update(const Inputs&, Clock::time_point now,
                                                 bool permitted = false);
  vehicle_control::Snapshot acknowledge(const vehicle_control::Acknowledgement&, Clock::time_point);
  void observe(const vehicle_control::ConfirmedState&, Clock::time_point);
  std::optional<vehicle_control::ConfirmedState> current(Clock::time_point);
  std::optional<vehicle_control::Snapshot> command(Clock::time_point);
  const std::optional<Target>& requested() const { return target_; }
  const Settings& settings() const { return settings_; }
  Clock::duration maximum_age() const { return policy_.maximum_age; }

 private:
  Settings settings_;
  vehicle_control::TargetPolicy policy_;
  vehicle_control::Coordinator coordinator_;
  std::optional<Target> target_{};
  std::optional<Value> issued_{};
  std::optional<Accent> screen_frontier_{};
  bool screen_withdrawn_ = false;
  std::uint64_t generation_ = 0;
  std::uint64_t identifier_ = 0;
  bool connected_ = false;
};
// Simulator only. Smoothstep interpolation of emitted channel intensities;
// current intermediate readings still require an explicit simulation receipt.
Value interpolate(const Value& from, const Value& to, double fraction);
std::string snapshot_json(Controller&, Clock::time_point now);
}  // namespace headunit::ambient
