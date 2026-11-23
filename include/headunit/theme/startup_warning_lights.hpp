// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace headunit::theme {
// A presentation preference only: never changes vehicle observations or lamps
// in the driving scene. Invalid/missing updates retain the last valid choice.
std::optional<bool> parse_startup_warning_lights(std::string_view);
class StartupWarningLightsFile {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto poll_interval = std::chrono::milliseconds(500);
  explicit StartupWarningLightsFile(std::string path);
  // Returns hide/show only when a valid choice differs from the last one returned.
  std::optional<bool> poll(Clock::time_point now);

 private:
  std::string path_;
  Clock::time_point next_poll_{};
  std::optional<bool> current_;
};
}  // namespace headunit::theme
