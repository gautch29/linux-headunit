// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace headunit {
enum class HeadingReference { unknown, true_north, magnetic_north };
enum class HeadingSource { unknown, vehicle_orientation, course_over_ground };
struct HeadingState {
  double degrees = 0;  // Clockwise from the explicit north reference, [0,360).
  HeadingReference reference = HeadingReference::unknown;
  HeadingSource source = HeadingSource::unknown;
  bool operator==(const HeadingState&) const = default;
};
bool valid_heading_state(const HeadingState&) noexcept;
// Canonical slash order: degrees/reference/source. Reference is true_north or
// magnetic_north; source is vehicle_orientation or course_over_ground. Missing
// heading is an absent optional, never an unknown enum or invented zero north.
std::string heading_state_key(const HeadingState&);
std::optional<HeadingState> parse_heading_state_key(std::string_view) noexcept;
}  // namespace headunit
