// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <chrono>
#include <cstdint>
#include <span>

namespace headunit {

// Synchronous socket write with one absolute deadline for the whole call,
// including partial progress and EINTR. The two-second maximum is a local
// receiver policy, not a phone wire limit. Socket/encryption ownership stays
// with the caller; failure requires retiring its partially written response.
void send_control_response(int socket, std::span<const std::uint8_t> response,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

}  // namespace headunit
