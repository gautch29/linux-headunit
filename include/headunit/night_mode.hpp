// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <optional>
#include <string_view>

namespace headunit {

// Day/night appearance offered to the phone (`/info` nightMode and the
// setNightMode command). A schedule uses local wall-clock minutes and may wrap
// midnight. `headlights` follows a fresh low-beam reading and falls back to the
// schedule while the lamp state is unknown.
struct NightModePolicy {
  enum class Kind { off, on, schedule, headlights };
  Kind kind = Kind::off;
  unsigned start_minute = 20 * 60;
  unsigned end_minute = 7 * 60;
  bool operator==(const NightModePolicy&) const = default;
};

// "off", "on", "schedule" (20:00-07:00), "schedule:HH:MM-HH:MM" or
// "headlights" (low beam, else the default schedule).
std::optional<NightModePolicy> parse_night_mode_policy(std::string_view text);
bool night_mode_at(const NightModePolicy& policy, unsigned minute_of_day,
                   std::optional<bool> low_beam = std::nullopt);

// Receiver-wide setting from HEADUNIT_NIGHT_MODE, configured once at startup.
// Absent means the receiver does not declare night-mode support at all.
void set_receiver_night_mode(std::optional<NightModePolicy> policy);
std::optional<bool> receiver_night_mode_now();

}  // namespace headunit
