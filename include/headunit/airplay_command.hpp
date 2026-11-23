// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/binary_plist.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace headunit {

// AirPlay resource/app modes are independent of vehicle drive modes. Unknown
// ownership remains absent; it never means that the accessory owns a resource.
struct AirplayResourceMode {
  std::uint32_t entity;
  std::optional<std::uint32_t> permanent_entity;
  bool operator==(const AirplayResourceMode&) const = default;
};
// Recognized current owners only. A permanent owner is not a current grant.
enum class AirplayResourceOwner { unobserved, controller, accessory, unknown };
AirplayResourceOwner airplay_resource_owner(const std::optional<AirplayResourceMode>&);

struct AirplayAppMode {
  std::uint32_t entity;
  // The phone uses -1 when no speech mode is specified. Preserve that sentinel;
  // it is not a resource grant or an active speech state.
  std::optional<std::int64_t> speech_mode;
  bool operator==(const AirplayAppMode&) const = default;
};
struct AirplayModesUpdate {
  std::array<std::optional<AirplayResourceMode>, 2> resources;
  std::array<std::optional<AirplayAppMode>, 3> apps;
};
struct AirplayModesState : AirplayModesUpdate {
  // Each entry retains its own receive date across unrelated/empty deltas.
  std::array<std::optional<std::chrono::steady_clock::time_point>, 2> resource_observed_at;
  std::array<std::optional<std::chrono::steady_clock::time_point>, 3> app_observed_at;
  std::optional<std::chrono::steady_clock::time_point> observed_at;
  std::uint64_t revision = 0;
  void apply(const AirplayModesUpdate&, std::chrono::steady_clock::time_point);
};
enum class AirplayCommandKind {
  modes_changed,
  duck_audio,
  unduck_audio,
  suggest_ui,
  disable_bluetooth,
  unsupported,
  count
};
struct AirplayCommand {
  AirplayCommandKind kind = AirplayCommandKind::unsupported;
  std::optional<AirplayModesUpdate> modes;
  std::optional<std::uint64_t> unsupported_fingerprint;
};
// Bounded plist/type validation shared by main and event control. Unsupported
// types are classified without retaining or logging their private parameters.
AirplayCommand decode_airplay_command(std::span<const std::uint8_t>);
const char* airplay_command_name(AirplayCommandKind);
// Fixed-format numeric ownership summary for diagnostics, e.g.
// "mainScreen=1/- mainAudio=1/2 speech=0 phoneCall=0 turnByTurn=0". Each
// resource is current/permanent entity; "-" means unobserved. iOS resolves the
// cluster Now Playing page to CarRadio while it believes the car owns mainAudio,
// so the permanent owner is needed to diagnose a missing Music page.
std::string airplay_modes_summary(const AirplayModesUpdate&);

}  // namespace headunit
