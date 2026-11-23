// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace headunit {

enum class GearboxType { manual, automatic, automated_manual, continuously_variable };
// This is the observed effective selector/operating mode, never a shift request.
enum class TransmissionSelector { park, reverse, neutral, drive, low, manual, sport };
enum class EngagedGearKind { neutral, reverse, forward };
struct EngagedGear {
  EngagedGearKind kind = EngagedGearKind::neutral;
  std::uint8_t forward_number = 0;
  bool operator==(const EngagedGear&) const = default;
};
struct TransmissionState {
  std::optional<GearboxType> gearbox_type{};
  std::optional<TransmissionSelector> selector{};
  std::optional<EngagedGear> engaged_gear{};
  bool operator==(const TransmissionState&) const = default;
};

// At least one component must be known. Known components form one coherent
// observation; absent components are not inferred from speed, RPM or gearbox type.
bool valid_transmission_state(const TransmissionState&);
std::string_view gearbox_type_key(GearboxType);
std::string_view transmission_selector_key(TransmissionSelector);
std::string engaged_gear_key(const EngagedGear&);
std::optional<GearboxType> parse_gearbox_type_key(std::string_view);
std::optional<TransmissionSelector> parse_transmission_selector_key(std::string_view);
std::optional<EngagedGear> parse_engaged_gear_key(std::string_view);
// Three canonical component keys, separated by '/', with '-' for an unknown
// component. The fully unknown state is unavailable, not a valid observation.
std::string transmission_state_key(const TransmissionState&);
std::optional<TransmissionState> parse_transmission_state_key(std::string_view);
// Effective park is P; otherwise prefer actual R/N/forward gear. A selector
// label is only a fallback when no actual gear is known; unavailable is "--".
std::string transmission_display_label(const std::optional<TransmissionState>&);

}  // namespace headunit
