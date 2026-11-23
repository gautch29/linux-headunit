// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace headunit {

// Observes actual bracketed host wall/monotonic readings for diagnostics only.
// It supplies no media anchor and never qualifies or retires a phone clock.
class AirplayHostClockMonitor {
 public:
  struct Reading {
    std::uint64_t ntp;
    std::uint64_t before_ns;
    std::uint64_t after_ns;
  };
  struct Correction {
    std::int64_t delta_ns;
    std::uint64_t age_ns;
    std::uint64_t previous_read_bracket_ns;
    std::uint64_t read_bracket_ns;
  };
  std::optional<Correction> observe(Reading reading);

 private:
  std::optional<Reading> previous_;
  unsigned reports_ = 0;
};

// AirPlay's 32-byte RTP-style timing exchange. Packet construction is separate
// from sockets so the wire format can be checked without a phone or MFi chip.
class AirplayTiming {
 public:
  using Packet = std::array<std::uint8_t, 32>;
  static std::uint64_t ntp_now();
  // The NTP instant monotonic_ns on a timeline that equals anchor_ntp at
  // anchor_monotonic_ns and then advances only with the monotonic clock.
  // System time corrections (NTP steps or slews) cannot move this timeline,
  // so the phone, which synchronizes to our timing replies, never sees them.
  // monotonic_ns must not precede the anchor. NTP-era wrap is preserved.
  static std::uint64_t ntp_at(std::uint64_t anchor_ntp, std::uint64_t anchor_monotonic_ns,
                              std::uint64_t monotonic_ns);
  static Packet request(std::uint64_t transmit);
  static Packet reply(std::span<const std::uint8_t> request, std::uint64_t receive,
                      std::uint64_t transmit);
  static bool valid_reply(std::span<const std::uint8_t> reply, std::uint64_t expected_origin);
};

}  // namespace headunit
