// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/theme/script_runtime.hpp>

namespace headunit::theme::bench {
// Verified external scripts, real host acquisition dates and an explicit
// simulated vehicle/UI peer. No physical display, phone or actuator owner.
void drive_mode_probe(const script::ResourceReader&, std::span<const std::uint8_t> profile);
}  // namespace headunit::theme::bench
