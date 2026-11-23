// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/prephone_indicator_summary.hpp>
#include <headunit/prephone_publication.hpp>

namespace headunit {
// Separate fixed binary projection. The HUPN001/eight-row API is unchanged.
inline constexpr std::size_t prephone_indicator_packet_size = 872;
using PrephoneIndicatorPacket = std::array<std::byte, prephone_indicator_packet_size>;
PrephoneIndicatorPacket prephone_indicator_packet(const PrephoneIndicatorSummary&,
                                                  const PrephoneNonce&, std::uint64_t sequence,
                                                  Clock::time_point copied_at);
}  // namespace headunit
