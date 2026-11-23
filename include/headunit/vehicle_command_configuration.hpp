// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/vehicle_commands.hpp>
#include <filesystem>
#include <string_view>
#include <vector>

namespace headunit::vehicle_control {

// Configuration declares target policies only. Loading it creates no provider,
// command, confirmed value or permission to dispatch a physical operation.
// Invalid schema/content throws invalid_argument; file I/O throws runtime_error.
std::vector<TargetPolicy> parse_configuration(std::string_view json);
std::vector<TargetPolicy> load_configuration(const std::filesystem::path& path);

}  // namespace headunit::vehicle_control
