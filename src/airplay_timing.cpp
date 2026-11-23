// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_timing.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace headunit {
namespace {
constexpr std::uint64_t ntp_unix_offset = 2208988800ULL;

void write_be64(AirplayTiming::Packet& packet, std::size_t offset, std::uint64_t number) {
  for (std::size_t index = 0; index < 8; ++index) {
    packet[offset + index] = static_cast<std::uint8_t>(number >> (56 - index * 8));
  }
}

std::uint64_t read_be64(std::span<const std::uint8_t> packet, std::size_t offset) {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    result = (result << 8) | packet[offset + index];
  }
  return result;
}
}  // namespace

std::optional<AirplayHostClockMonitor::Correction> AirplayHostClockMonitor::observe(
    Reading reading) {
  constexpr std::uint64_t second = 1000000000;
  constexpr std::uint64_t max_read_bracket = 1000000;
  constexpr std::uint64_t max_observation_gap = 10 * second;
  if (!reading.ntp || reading.after_ns < reading.before_ns ||
      reading.after_ns - reading.before_ns > max_read_bracket) {
    previous_.reset();
    return {};
  }
  const auto previous = previous_;
  previous_ = reading;
  if (!previous) return {};
  if (reading.before_ns <= previous->after_ns) {
    previous_.reset();
    return {};
  }
  const auto previous_bracket = previous->after_ns - previous->before_ns;
  const auto bracket = reading.after_ns - reading.before_ns;
  const auto age = reading.before_ns + bracket / 2 - (previous->before_ns + previous_bracket / 2);
  if (age > max_observation_gap) return {};
  const auto ticks = ((age / second) << 32) + ((age % second) << 32) / second;
  // NTP-era wrap is valid. Express only the relative correction, never origins.
  const auto difference = reading.ntp - (previous->ntp + ticks);
  const bool backwards = difference > (std::uint64_t{1} << 63);
  const auto magnitude = backwards ? std::uint64_t{0} - difference : difference;
  const auto nanoseconds =
      (magnitude >> 32) * second + (((magnitude & 0xffffffffULL) * second + (1ULL << 31)) >> 32);
  // Diagnostic threshold only: actual read brackets, conversion tolerance and
  // conservative 500 ppm host drift, plus 20 ms. No clock policy depends on it.
  const auto bound = 20000000 + (previous_bracket + 1) / 2 + (bracket + 1) / 2 + 2000 +
                     (age * 500 + 999999) / 1000000;
  if (nanoseconds <= bound || reports_ >= 8) return {};
  ++reports_;
  const auto signed_delta = static_cast<std::int64_t>(nanoseconds);
  return Correction{backwards ? -signed_delta : signed_delta, age, previous_bracket, bracket};
}

std::uint64_t AirplayTiming::ntp_now() {
  using namespace std::chrono;
  const auto duration = system_clock::now().time_since_epoch();
  const auto seconds = duration_cast<std::chrono::seconds>(duration);
  const auto fraction = duration_cast<nanoseconds>(duration - seconds).count();
  return ((static_cast<std::uint64_t>(seconds.count()) + ntp_unix_offset) << 32) |
         ((static_cast<std::uint64_t>(fraction) << 32) / 1000000000ULL);
}

std::uint64_t AirplayTiming::ntp_at(std::uint64_t anchor_ntp, std::uint64_t anchor_monotonic_ns,
                                    std::uint64_t monotonic_ns) {
  if (monotonic_ns < anchor_monotonic_ns)
    throw std::invalid_argument("monotonic time precedes the timing anchor");
  const std::uint64_t elapsed = monotonic_ns - anchor_monotonic_ns;
  const std::uint64_t seconds = elapsed / 1000000000ULL;
  const std::uint64_t nanoseconds = elapsed % 1000000000ULL;
  const std::uint64_t ticks = (seconds << 32) + (nanoseconds << 32) / 1000000000ULL;
  return anchor_ntp + ticks;  // Unsigned arithmetic keeps NTP-era wrap.
}

AirplayTiming::Packet AirplayTiming::request(std::uint64_t transmit) {
  Packet packet{};
  packet[0] = 0x80;
  packet[1] = 0xd2;
  write_be64(packet, 24, transmit);
  return packet;
}

AirplayTiming::Packet AirplayTiming::reply(std::span<const std::uint8_t> request_packet,
                                           std::uint64_t receive, std::uint64_t transmit) {
  if (request_packet.size() != 32 || request_packet[0] != 0x80 || request_packet[1] != 0xd2) {
    throw std::invalid_argument("invalid AirPlay timing request");
  }
  Packet packet{};
  packet[0] = 0x80;
  packet[1] = 0xd3;
  packet[2] = request_packet[2];
  packet[3] = request_packet[3];
  std::copy_n(request_packet.begin() + 24, 8, packet.begin() + 8);
  write_be64(packet, 16, receive);
  write_be64(packet, 24, transmit);
  return packet;
}

bool AirplayTiming::valid_reply(std::span<const std::uint8_t> packet,
                                std::uint64_t expected_origin) {
  return packet.size() == 32 && packet[0] == 0x80 && packet[1] == 0xd3 && expected_origin != 0 &&
         read_be64(packet, 8) == expected_origin && read_be64(packet, 16) != 0 &&
         read_be64(packet, 24) != 0;
}
}  // namespace headunit
