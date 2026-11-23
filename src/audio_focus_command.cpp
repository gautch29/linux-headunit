// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_focus_command.hpp>
#include <headunit/binary_plist.hpp>
#include <cmath>
#include <stdexcept>
#include <string>

namespace headunit {
namespace {
void check(bool valid) {
  if (!valid) throw std::invalid_argument("invalid audio focus command");
}
double numeric(const PlistValue& value) {
  if (const auto* real = std::get_if<double>(&value.value)) return *real;
  if (const auto* integer = std::get_if<std::int64_t>(&value.value)) return double(*integer);
  throw std::invalid_argument("audio focus parameter must be numeric");
}
}  // namespace
std::optional<AudioFocusCommand> decode_audio_focus_command(std::span<const std::uint8_t> bytes) {
  // Preserve the existing diagnostic route for other bounded command shapes.
  preflight_binary_plist(bytes, {65536, 512, 131072, 16});
  const auto value = decode_binary_plist(bytes);
  const auto* root = std::get_if<PlistValue::Dictionary>(&value.value);
  check(root != nullptr);
  const auto type = root->find("type");
  check(type != root->end());
  const auto* name = std::get_if<std::string>(&type->second.value);
  check(name != nullptr);
  if (*name != "duckAudio" && *name != "unduckAudio") return std::nullopt;
  preflight_binary_plist(bytes, {8192, 32, 16384, 4});
  check(root->size() <= 2);
  for (const auto& [key, parameter] : *root) {
    (void)parameter;
    check(key == "type" || key == "params");
  }
  const auto parameters = root->find("params");
  const PlistValue::Dictionary empty;
  const auto* params = parameters == root->end()
                           ? &empty
                           : std::get_if<PlistValue::Dictionary>(&parameters->second.value);
  check(params && params->size() <= 2);
  AudioFocusCommand command{*name == "duckAudio" ? .2 : 1., 500.};
  for (const auto& [key, parameter] : *params) {
    if (key == "volume" && *name == "unduckAudio") continue;  // Release does not read volume.
    const auto number = numeric(parameter);
    check(std::isfinite(number));
    if (key == "durationMs")
      command.duration_ms = number;
    else if (key == "volume")
      command.gain = number <= -144 ? 0. : number >= 0 ? 1. : std::pow(10., number / 20.);
    else
      check(false);
  }
  validate_audio_focus_command(command);
  return command;
}
}  // namespace headunit
