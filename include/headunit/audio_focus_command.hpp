// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>

namespace headunit {
struct AudioFocusCommand {
  double gain = 1., duration_ms = 500.;
  bool operator==(const AudioFocusCommand&) const = default;
};
// Local ramp policy, not an advertised Apple protocol limit.
inline constexpr double audio_focus_max_duration_ms = 60000.;
inline void validate_audio_focus_command(const AudioFocusCommand& command) {
  if (!std::isfinite(command.gain) || command.gain < 0 || command.gain > 1 ||
      !std::isfinite(command.duration_ms) || command.duration_ms < 0 ||
      command.duration_ms > audio_focus_max_duration_ms)
    throw std::invalid_argument("invalid audio focus command");
}
// Exact authenticated /command duckAudio/unduckAudio plist. Unknown command
// types return no command; malformed/oversized inputs throw without mutation.
std::optional<AudioFocusCommand> decode_audio_focus_command(std::span<const std::uint8_t>);
}  // namespace headunit
