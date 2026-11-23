// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <optional>
#include <string_view>

namespace headunit {

// Driving restrictions offered to the phone (`/info` limitedUI and
// limitedUIElements, then setLimitedUI). The phone restricts keyboards and long
// lists while limited. Hysteresis avoids toggling around walking pace; an
// unknown speed keeps the current state rather than inventing "parked".
struct LimitedUiPolicy {
  double enter_kmh = 5;
  double exit_kmh = 2;
};

// "speed" (enter above 5 km/h, leave below 2 km/h). Anything else is invalid.
std::optional<LimitedUiPolicy> parse_limited_ui_policy(std::string_view text);
bool limited_ui_next(const LimitedUiPolicy& policy, bool limited, std::optional<double> speed_kmh);

// Receiver-wide setting from HEADUNIT_LIMITED_UI, configured once at startup.
void set_receiver_limited_ui(std::optional<LimitedUiPolicy> policy);
std::optional<LimitedUiPolicy> receiver_limited_ui();

}  // namespace headunit
