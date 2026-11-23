// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/ambient_light.hpp>
#include "theme/vehicle_json.hpp"
#include <algorithm>
#include <stdexcept>

namespace headunit::ambient {
namespace {
using Json = theme::opack::Value;
using Object = theme::opack::Dictionary;
void require(bool valid, const char* message) {
  if (!valid) throw std::invalid_argument(message);
}
const Object& object(const Json& value, std::initializer_list<std::string_view> keys) {
  const auto* result = std::get_if<Object>(&value.data);
  require(result != nullptr, "ambient configuration requires an object");
  for (const auto& [key, child] : *result) {
    (void)child;
    const auto* name = std::get_if<std::string>(&key.data);
    require(name && std::find(keys.begin(), keys.end(), *name) != keys.end(),
            "unknown ambient configuration key");
  }
  return *result;
}
const Json& field(const Object& value, std::string_view name) {
  for (const auto& [key, child] : value)
    if (std::get<std::string>(key.data) == name) return child;
  throw std::invalid_argument("missing ambient configuration key");
}
std::string_view string(const Json& value) {
  const auto* text = std::get_if<std::string>(&value.data);
  require(text != nullptr, "ambient configuration requires a string");
  return *text;
}
unsigned number(const Json& value, unsigned maximum) {
  const auto* result = std::get_if<std::int64_t>(&value.data);
  require(result && *result >= 0 && *result <= maximum, "ambient integer outside bounds");
  return static_cast<unsigned>(*result);
}
Colour colour(const Json& value) {
  const auto* array = std::get_if<theme::opack::Array>(&value.data);
  require(array && array->size() == 4, "ambient colour requires four RGBW channels");
  Colour result;
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = static_cast<std::uint8_t>(number((*array)[i], 255));
  return result;
}
}  // namespace
Settings parse_settings(std::string_view text) {
  require(!text.empty() && text.size() <= 16384 && text.find('\0') == text.npos,
          "ambient settings must be 1..16384 bytes");
  const auto json = theme::vehicle::detail::Json(text).parse();
  const auto& root =
      object(json, {"version", "enabled", "selection", "fixed_rgbw", "day_brightness_permille",
                    "night_brightness_permille", "transition_ms", "night_policy", "mode_colours"});
  require(number(field(root, "version"), 1) == 1, "unsupported ambient settings version");
  Settings result;
  const auto* enabled = std::get_if<bool>(&field(root, "enabled").data);
  require(enabled != nullptr, "ambient enabled requires a Boolean");
  result.enabled = *enabled;
  const auto selection = string(field(root, "selection"));
  if (selection == "automatic")
    result.selection = Selection::automatic;
  else if (selection == "screen")
    result.selection = Selection::screen;
  else if (selection == "drive_mode")
    result.selection = Selection::drive_mode;
  else if (selection == "fixed")
    result.selection = Selection::fixed;
  else
    throw std::invalid_argument("unsupported ambient selection");
  result.fixed_rgbw = colour(field(root, "fixed_rgbw"));
  result.day_brightness_permille = number(field(root, "day_brightness_permille"), 1000);
  result.night_brightness_permille = number(field(root, "night_brightness_permille"), 1000);
  result.transition_ms =
      number(field(root, "transition_ms"), vehicle_control::maximum_ambient_transition_ms);
  const auto night = parse_night_mode_policy(string(field(root, "night_policy")));
  require(night.has_value(), "unsupported ambient night policy");
  result.night_policy = *night;
  const auto* modes = std::get_if<theme::opack::Array>(&field(root, "mode_colours").data);
  require(modes && modes->size() <= result.mode_colours.size(),
          "ambient mode colours exceed 32 entries");
  for (const auto& mode : *modes) {
    const auto& entry = object(mode, {"id", "rgbw"});
    const auto identifier = parse_drive_mode_identifier(string(field(entry, "id")));
    require(identifier.has_value(), "invalid ambient drive mode identifier");
    result.mode_colours[result.mode_count++] =
        ModeColour{*identifier, colour(field(entry, "rgbw"))};
  }
  require(valid_settings(result), "invalid ambient settings or duplicate modes");
  return result;
}
}  // namespace headunit::ambient
