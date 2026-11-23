// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <string>

namespace headunit {
inline constexpr std::size_t maximum_vehicle_observation_datagram = 8192;
enum class VehicleObservationWireVersion { hvo1, hvo2, hvo3, hvo4, hvo6 };
struct VehicleObservationPacket {
  VehicleSample sample;
  std::uint64_t sequence = 0;
  VehicleObservationWireVersion version = VehicleObservationWireVersion::hvo1;
};
// Validates decoded snapshots too: exact representable positive dates/ages,
// coherent value/availability/provenance, and age consistency at publication.
// Receipt is only an upper bound on publication; no dates are inferred.
bool valid_vehicle_observation_packet(const VehicleObservationPacket&,
                                      Clock::time_point received_at);
// HVO1/HVO2 <generation> <sequence> <started_ns> <published_ns>. HVO1
// contains exactly 34 original reading/indicator records; HVO2 adds the complete
// transmission=type/selector/gear,acquired_ns-or-,max_age_ns,origin,availability
// record. HVO3 adds a sixth header token drive_modes=<comma-list-or-> and a
// 36th drive_mode=<identifier-or-> record. HVO4 keeps that header and adds
// coherent trip and qualified heading records, exactly 38 records total.
// Each observation has the same acquisition/age/provenance/availability timing.
// The unknown transmission value is "-"; unknown components use "-".
// Single spaces separate tokens; all fields occur exactly once.
// Origins: m/c/s. Availability: a/u/s/d. Complete snapshots, not delta batches.
// Every date is an original positive nanosecond date in this host's steady
// clock domain. Parsing does not infer receipt dates or enforce replay order.
std::optional<VehicleObservationPacket> parse_vehicle_observation_datagram(
    std::string_view, Clock::time_point received_at);
// Metadata is mandatory. Reject inconsistent/expired-at-publication available
// data; never silently expire values or replace their original dates. HVO1 is
// the default and rejects transmission state or non-default timing it cannot
// preserve. HVO1/HVO2 reject drive-mode catalog/state/non-default timing.
// HVO1/HVO2/HVO3 also reject trip/heading values or non-default timing.
// Producers explicitly select HVO2 for transmission, HVO3 for drive modes, or
// HVO4 for current trip and qualified heading.
std::string encode_vehicle_observation_datagram(const VehicleObservationPacket&);
}  // namespace headunit
