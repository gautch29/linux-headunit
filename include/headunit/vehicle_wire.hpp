// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/vehicle.hpp>

#include <optional>
#include <string_view>
#include <string>

namespace headunit {

inline constexpr std::size_t maximum_vehicle_datagram = 2048;
inline constexpr std::size_t maximum_vehicle_transport_datagram = maximum_vehicle_datagram + 32;
// Local adapter protocol: one complete snapshot per datagram; missing keys are
// unavailable, never retained from a previous snapshot.
// HUV2 key=value ...; numeric values use documented units, indicators use
// off/on/flash, and '-' explicitly marks an unavailable input.
// Legacy:
// HUV1 <speed_kmh or -> <engine_rpm or ->, without spaces inside a field.
// Direct HUV packets acquire the receiver's monotonic timestamp on receipt.
// The local fan-out broker wraps one HUV packet as HUF1 <monotonic_ns> <HUV...>.
// This preserves Pi ingress time across consumer queues; future/nested envelopes
// are rejected. It is not a hardware sensor timestamp or an authenticated bus.
std::optional<VehicleSample> parse_vehicle_datagram(std::string_view text,
                                                    Clock::time_point received_at);

// Legacy encoding refuses observation_timing: this protocol cannot preserve
// per-signal dates, origins or generations. A versioned transport is pending.
std::string encode_vehicle_datagram(const VehicleSample&);

}  // namespace headunit
