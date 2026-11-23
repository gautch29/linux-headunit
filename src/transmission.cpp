// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/transmission.hpp>

#include <array>
#include <charconv>
#include <utility>

namespace headunit {
namespace {
constexpr std::array gearbox_keys{
    std::pair{GearboxType::manual, std::string_view{"manual"}},
    std::pair{GearboxType::automatic, std::string_view{"automatic"}},
    std::pair{GearboxType::automated_manual, std::string_view{"automated_manual"}},
    std::pair{GearboxType::continuously_variable, std::string_view{"continuously_variable"}}};
constexpr std::array selector_keys{
    std::pair{TransmissionSelector::park, std::string_view{"park"}},
    std::pair{TransmissionSelector::reverse, std::string_view{"reverse"}},
    std::pair{TransmissionSelector::neutral, std::string_view{"neutral"}},
    std::pair{TransmissionSelector::drive, std::string_view{"drive"}},
    std::pair{TransmissionSelector::low, std::string_view{"low"}},
    std::pair{TransmissionSelector::manual, std::string_view{"manual"}},
    std::pair{TransmissionSelector::sport, std::string_view{"sport"}}};
template <class T, std::size_t N>
std::string_view key(T value, const std::array<std::pair<T, std::string_view>, N>& keys) {
  for (const auto& entry : keys)
    if (entry.first == value) return entry.second;
  return {};
}
template <class T, std::size_t N>
std::optional<T> parse(std::string_view value,
                       const std::array<std::pair<T, std::string_view>, N>& keys) {
  for (const auto& entry : keys)
    if (entry.second == value) return entry.first;
  return std::nullopt;
}
bool valid_gear(const EngagedGear& gear) {
  switch (gear.kind) {
    case EngagedGearKind::neutral:
    case EngagedGearKind::reverse:
      return gear.forward_number == 0;
    case EngagedGearKind::forward:
      return gear.forward_number >= 1 && gear.forward_number <= 32;
  }
  return false;
}
}  // namespace
std::string_view gearbox_type_key(GearboxType value) {
  return key(value, gearbox_keys);
}
std::string_view transmission_selector_key(TransmissionSelector value) {
  return key(value, selector_keys);
}
std::optional<GearboxType> parse_gearbox_type_key(std::string_view value) {
  return parse(value, gearbox_keys);
}
std::optional<TransmissionSelector> parse_transmission_selector_key(std::string_view value) {
  return parse(value, selector_keys);
}
std::string engaged_gear_key(const EngagedGear& gear) {
  if (!valid_gear(gear)) return {};
  switch (gear.kind) {
    case EngagedGearKind::neutral:
      return "neutral";
    case EngagedGearKind::reverse:
      return "reverse";
    case EngagedGearKind::forward:
      return std::to_string(gear.forward_number);
  }
  return {};
}
std::optional<EngagedGear> parse_engaged_gear_key(std::string_view value) {
  if (value == "neutral") return EngagedGear{EngagedGearKind::neutral, 0};
  if (value == "reverse") return EngagedGear{EngagedGearKind::reverse, 0};
  if (value.empty() || value.size() > 2 || value.front() < '1' || value.front() > '9')
    return std::nullopt;
  unsigned number = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number < 1 ||
      number > 32)
    return std::nullopt;
  return EngagedGear{EngagedGearKind::forward, static_cast<std::uint8_t>(number)};
}
bool valid_transmission_state(const TransmissionState& state) {
  if (!state.gearbox_type && !state.selector && !state.engaged_gear) return false;
  if (state.gearbox_type && gearbox_type_key(*state.gearbox_type).empty()) return false;
  if (state.selector && transmission_selector_key(*state.selector).empty()) return false;
  if (state.engaged_gear && !valid_gear(*state.engaged_gear)) return false;
  if (!state.selector || !state.engaged_gear) return true;
  const auto kind = state.engaged_gear->kind;
  switch (*state.selector) {
    case TransmissionSelector::park:
    case TransmissionSelector::neutral:
      return kind == EngagedGearKind::neutral;
    case TransmissionSelector::reverse:
      return kind == EngagedGearKind::reverse;
    case TransmissionSelector::drive:
    case TransmissionSelector::low:
    case TransmissionSelector::manual:
    case TransmissionSelector::sport:
      return kind == EngagedGearKind::neutral || kind == EngagedGearKind::forward;
  }
  return false;
}
std::string transmission_state_key(const TransmissionState& state) {
  if (!valid_transmission_state(state)) return {};
  std::string result =
      state.gearbox_type ? std::string(gearbox_type_key(*state.gearbox_type)) : "-";
  result += '/';
  result += state.selector ? transmission_selector_key(*state.selector) : std::string_view{"-"};
  result += '/';
  result += state.engaged_gear ? engaged_gear_key(*state.engaged_gear) : "-";
  return result;
}
std::optional<TransmissionState> parse_transmission_state_key(std::string_view value) {
  constexpr auto maximum_key_size =
      std::string_view{"continuously_variable/reverse/reverse"}.size();
  if (value.size() > maximum_key_size) return std::nullopt;
  const auto first = value.find('/');
  if (first == std::string_view::npos) return std::nullopt;
  const auto second = value.find('/', first + 1);
  if (second == std::string_view::npos || value.find('/', second + 1) != std::string_view::npos)
    return std::nullopt;
  TransmissionState result;
  const auto type = value.substr(0, first), selector = value.substr(first + 1, second - first - 1),
             gear = value.substr(second + 1);
  if (type != "-") {
    result.gearbox_type = parse_gearbox_type_key(type);
    if (!result.gearbox_type) return std::nullopt;
  }
  if (selector != "-") {
    result.selector = parse_transmission_selector_key(selector);
    if (!result.selector) return std::nullopt;
  }
  if (gear != "-") {
    result.engaged_gear = parse_engaged_gear_key(gear);
    if (!result.engaged_gear) return std::nullopt;
  }
  if (!valid_transmission_state(result)) return std::nullopt;
  return result;
}
std::string transmission_display_label(const std::optional<TransmissionState>& state) {
  if (!state || !valid_transmission_state(*state)) return "--";
  if (state->selector == TransmissionSelector::park) return "P";
  if (state->engaged_gear) {
    switch (state->engaged_gear->kind) {
      case EngagedGearKind::neutral:
        return "N";
      case EngagedGearKind::reverse:
        return "R";
      case EngagedGearKind::forward:
        return std::to_string(state->engaged_gear->forward_number);
    }
  }
  if (state->selector) {
    switch (*state->selector) {
      case TransmissionSelector::park:
        return "P";
      case TransmissionSelector::reverse:
        return "R";
      case TransmissionSelector::neutral:
        return "N";
      case TransmissionSelector::drive:
        return "D";
      case TransmissionSelector::low:
        return "L";
      case TransmissionSelector::manual:
        return "M";
      case TransmissionSelector::sport:
        return "S";
    }
  }
  return "--";
}
}  // namespace headunit
