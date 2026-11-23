// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/ambient_light.hpp>
#include <array>
#include <span>

namespace headunit::ambient {
// Private local software-policy IPC. No provider admission or actuator API.
inline constexpr std::size_t software_settings_size = 2368;
inline constexpr std::size_t software_packet_size = 192 + software_settings_size;
inline constexpr std::size_t software_action_size = 64 + software_settings_size;
using SoftwareNonce = std::array<std::byte, 32>;
using SoftwareSettings = std::array<std::byte, software_settings_size>;
using SoftwarePacket = std::array<std::byte, software_packet_size>;
using SoftwareAction = std::array<std::byte, software_action_size>;
SoftwareSettings software_settings(const Settings&);
SoftwareAction software_action(const Settings&, const SoftwareNonce&, std::uint64_t revision,
                               std::uint64_t identifier, Clock::time_point sent_at);
class SoftwareModel {
 public:
  SoftwareModel();
  // False retires the IPC protocol. Policy errors are reported without mutation.
  bool change(std::span<const std::byte>, const SoftwareNonce&, Clock::time_point);
  SoftwarePacket publish(const VehicleSample&, const SoftwareNonce&, std::uint64_t sequence,
                         Clock::time_point, std::optional<unsigned> local_minute);
  void retire(Clock::time_point);

 private:
  Controller controller_;
  std::uint64_t revision_ = 1;
  std::uint64_t action_ = 0;
  std::uint64_t result_ = 0;
  bool retired_ = false;
};
}  // namespace headunit::ambient
