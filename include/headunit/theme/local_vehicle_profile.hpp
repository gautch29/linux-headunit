// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/vehicle_script_view.hpp>

namespace headunit::theme::vehicle {
// Host-supplied local script bindings. This format has no protocol type IDs and
// must not be advertised as a phone-facing VDP accessory configuration. Timed
// signals start unavailable; fixed values are explicit immutable configuration.
// Fixed arrays contain at most 32 primitive items, strings up to 128 bytes,
// and at most 1 KiB encoded. Only managed uiSceneOptions may be a timed array;
// its session owner supplies a bounded option list. Other timed/nested/data
// arrays are rejected.
struct VehicleInputBinding {
  std::int64_t iid;
  std::string field;
  ValueKind kind;
  double scale = 1, offset = 0;
  std::optional<double> minimum, maximum;
  std::vector<opack::Value> states;
  // Explicit semantic state keys -> asset-specific script scalars.
  // Unknown components and omitted keys stay unavailable; there is no default.
  std::map<std::string, opack::Value> semantic_states{};
};
struct LocalProfile {
  std::shared_ptr<ReadOnlyService> source;
  std::vector<ScriptBinding> bindings;
  std::vector<VehicleInputBinding> vehicle_inputs{};
  // Local configuration metadata, never a phone-facing VehicleInformation service
  // or an override of the head unit's pairing/device identity.
  struct Identity {
    std::string make, model;
    std::uint16_t year;
  };
  std::optional<Identity> identity{};
  // Explicit local session-owner bindings, distinct from car observations.
  // Both are timed; no saved profile may assert startup or pairing as fixed data.
  struct SceneInputs {
    std::int64_t state, options;
  };
  std::optional<SceneInputs> scene_inputs{};
};
LocalProfile decode_local_profile(std::span<const std::uint8_t> plist);
// Also validates directly constructed profiles. Observed currentIndex requires
// an explicit ordered fixed authored identifiers array and unique in-range
// indices; the source's semantic mode catalogue remains independent of it.
// Trip consumption is a timed numeric unit conversion from canonical L/100km.
void validate_local_vehicle_inputs(const LocalProfile&);
}  // namespace headunit::theme::vehicle
