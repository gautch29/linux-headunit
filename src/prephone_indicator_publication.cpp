// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/prephone_indicator_publication.hpp>
#include <cstring>

namespace headunit {
namespace {
std::uint64_t ns(Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}
void integer(PrephoneIndicatorPacket& packet, std::size_t offset, std::uint64_t value,
             unsigned size = 8) {
  for (unsigned i = 0; i < size; ++i)
    packet[offset + i] = std::byte(value >> (i * 8));
}
}  // namespace
PrephoneIndicatorPacket prephone_indicator_packet(const PrephoneIndicatorSummary& model,
                                                  const PrephoneNonce& nonce,
                                                  std::uint64_t sequence,
                                                  Clock::time_point copied_at) {
  static_assert(prephone_indicator_summary_row_limit == 24);
  PrephoneIndicatorPacket packet{};
  std::memcpy(packet.data(), "HUPI001", 7);
  std::memcpy(packet.data() + 8, nonce.data(), nonce.size());
  integer(packet, 40, sequence);
  integer(packet, 48, ns(copied_at));
  integer(packet, 56, static_cast<std::uint64_t>(model.state));
  integer(packet, 64, model.source_generation.value_or(0));
  integer(packet, 72, model.source_started_at ? ns(*model.source_started_at) : 0);
  integer(packet, 80, model.source_sampled_at ? ns(*model.source_sampled_at) : 0);
  integer(packet, 88, 1, 2);
  integer(packet, 90, model.rows.size(), 2);
  integer(packet, 92, packet.size(), 4);
  packet[96] =
      model.clock_domain == PrephoneIndicatorClockDomain::host_steady ? std::byte{1} : std::byte{0};
  for (std::size_t i = 0; i < model.rows.size(); ++i) {
    const auto& row = model.rows[i];
    const auto at = 104 + i * 32;
    packet[at] = std::byte(row.availability);
    packet[at + 1] = row.origin ? std::byte(*row.origin) : std::byte{255};
    packet[at + 2] = row.value ? std::byte(*row.value) : std::byte{255};
    packet[at + 3] = std::byte(i);
    integer(packet, at + 16, row.acquired_at ? ns(*row.acquired_at) : 0);
    integer(packet, at + 24,
            row.maximum_age
                ? std::chrono::duration_cast<std::chrono::nanoseconds>(*row.maximum_age).count()
                : 0);
  }
  return packet;
}
}  // namespace headunit
