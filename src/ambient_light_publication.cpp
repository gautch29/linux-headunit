// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/ambient_light_publication.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace headunit::ambient {
namespace {
void put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value,
         std::size_t size = 8) {
  for (std::size_t i = 0; i < size; ++i)
    bytes[offset + i] = std::byte(value >> (8 * i));
}
std::uint64_t get(std::span<const std::byte> bytes, std::size_t offset, std::size_t size = 8) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i)
    value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
  return value;
}
std::uint64_t ns(Clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}
vehicle_control::TargetPolicy policy() {
  vehicle_control::TargetPolicy value;
  value.key = *vehicle_control::parse_key("cabin.ambient.rgbw");
  value.kind = vehicle_control::ValueKind::ambient_light;
  return value;  // Not writable, no provider generation is ever begun.
}
std::optional<Settings> decode(std::span<const std::byte> bytes) {
  if (bytes.size() != software_settings_size || get(bytes, 0, 1) > 1 || get(bytes, 1, 1) > 3 ||
      get(bytes, 2, 1) > 3 || get(bytes, 3, 1) > 32)
    return {};
  Settings value;
  value.enabled = get(bytes, 0, 1);
  value.selection = static_cast<Selection>(get(bytes, 1, 1));
  value.night_policy.kind = static_cast<NightModePolicy::Kind>(get(bytes, 2, 1));
  value.mode_count = get(bytes, 3, 1);
  for (std::size_t i = 0; i < 4; ++i)
    value.fixed_rgbw[i] = get(bytes, 4 + i, 1);
  value.day_brightness_permille = get(bytes, 8, 2);
  value.night_brightness_permille = get(bytes, 10, 2);
  value.transition_ms = get(bytes, 12, 2);
  value.night_policy.start_minute = get(bytes, 14, 2);
  value.night_policy.end_minute = get(bytes, 16, 2);
  for (std::size_t i = 0; i < value.mode_count; ++i) {
    const auto at = 64 + i * 72;
    auto& mode = value.mode_colours[i];
    mode.mode.length = get(bytes, at, 1);
    std::memcpy(mode.mode.bytes.data(), bytes.data() + at + 4, mode.mode.bytes.size());
    for (std::size_t c = 0; c < 4; ++c)
      mode.rgbw[c] = get(bytes, at + 68 + c, 1);
  }
  if (!valid_settings(value)) return {};
  // Canonical roundtrip rejects reserved/trailing/unused mode bytes atomically.
  const auto canonical = software_settings(value);
  if (!std::equal(bytes.begin(), bytes.end(), canonical.begin())) return {};
  return value;
}
void evidence(std::span<std::byte> bytes, std::size_t at, const std::optional<Evidence>& value) {
  if (!value) return;
  put(bytes, at, value->generation);
  put(bytes, at + 8, ns(*value->timing.acquired_at));
  put(bytes, at + 16,
      std::chrono::duration_cast<std::chrono::nanoseconds>(value->timing.maximum_age).count());
  put(bytes, at + 24, static_cast<unsigned>(value->timing.origin));
}
}  // namespace
SoftwareSettings software_settings(const Settings& value) {
  if (!valid_settings(value)) throw std::invalid_argument("invalid ambient software settings");
  SoftwareSettings bytes{};
  put(bytes, 0, value.enabled, 1);
  put(bytes, 1, static_cast<unsigned>(value.selection), 1);
  put(bytes, 2, static_cast<unsigned>(value.night_policy.kind), 1);
  put(bytes, 3, value.mode_count, 1);
  for (std::size_t i = 0; i < 4; ++i)
    put(bytes, 4 + i, value.fixed_rgbw[i], 1);
  put(bytes, 8, value.day_brightness_permille, 2);
  put(bytes, 10, value.night_brightness_permille, 2);
  put(bytes, 12, value.transition_ms, 2);
  put(bytes, 14, value.night_policy.start_minute, 2);
  put(bytes, 16, value.night_policy.end_minute, 2);
  for (std::size_t i = 0; i < value.mode_count; ++i) {
    const auto at = 64 + i * 72;
    const auto& mode = value.mode_colours[i];
    put(bytes, at, mode.mode.length, 1);
    std::memcpy(bytes.data() + at + 4, mode.mode.bytes.data(), mode.mode.length);
    for (std::size_t c = 0; c < 4; ++c)
      put(bytes, at + 68 + c, mode.rgbw[c], 1);
  }
  return bytes;
}
SoftwareAction software_action(const Settings& settings, const SoftwareNonce& nonce,
                               std::uint64_t revision, std::uint64_t identifier,
                               Clock::time_point sent_at) {
  SoftwareAction bytes{};
  std::memcpy(bytes.data(), "HUALS01", 7);
  std::memcpy(bytes.data() + 8, nonce.data(), nonce.size());
  put(bytes, 40, revision);
  put(bytes, 48, identifier);
  put(bytes, 56, ns(sent_at));
  const auto encoded = software_settings(settings);
  std::copy(encoded.begin(), encoded.end(), bytes.begin() + 64);
  return bytes;
}
SoftwareModel::SoftwareModel() : controller_(Settings{}, policy()) {}
bool SoftwareModel::change(std::span<const std::byte> bytes, const SoftwareNonce& nonce,
                           Clock::time_point now) {
  if (retired_ || bytes.size() != software_action_size ||
      std::memcmp(bytes.data(), "HUALS01\0", 8) != 0 ||
      !std::equal(nonce.begin(), nonce.end(), bytes.begin() + 8) || get(bytes, 48) <= action_)
    return false;
  action_ = get(bytes, 48);
  const auto sent = get(bytes, 56);
  if (!sent || sent > ns(now) || ns(now) - sent >= 1'000'000'000) {
    result_ = 4;  // Existing one-second model validity also bounds this action.
    return true;
  }
  if (get(bytes, 40) != revision_ || revision_ == std::numeric_limits<std::uint64_t>::max()) {
    result_ = 3;
    return true;
  }
  const auto settings = decode(bytes.subspan(64));
  if (!settings) {
    result_ = 2;
    return true;
  }
  controller_.settings(*settings);
  ++revision_;
  result_ = 1;  // Software policy only. No physical applied-state acknowledgement.
  return true;
}
SoftwarePacket SoftwareModel::publish(const VehicleSample& sample, const SoftwareNonce& nonce,
                                      std::uint64_t sequence, Clock::time_point now,
                                      std::optional<unsigned> local_minute) {
  if (retired_ || !sequence || now <= Clock::time_point{})
    throw std::invalid_argument("ambient software owner retired/invalid publication");
  Inputs inputs;
  inputs.vehicle = sample;
  inputs.local_minute = local_minute;
  controller_.update(inputs, now, false);
  SoftwarePacket bytes{};
  std::memcpy(bytes.data(), "HUAL001", 7);
  std::memcpy(bytes.data() + 8, nonce.data(), nonce.size());
  put(bytes, 40, sequence);
  put(bytes, 48, ns(now));
  put(bytes, 56, revision_);
  put(bytes, 64, action_);
  put(bytes, 72, result_);
  if (const auto& target = controller_.requested()) {
    put(bytes, 80, 1);
    put(bytes, 88, ns(target->derived_at));
    put(bytes, 96, ns(target->valid_until));
    put(bytes, 104, static_cast<unsigned>(target->source), 1);
    put(bytes, 105, target->night, 1);
    put(bytes, 106, static_cast<unsigned>(target->night_source), 1);
    const auto value = vehicle_control::encode_ambient_light_value(target->value);
    for (std::size_t i = 0; i < value.size(); ++i)
      bytes[112 + i] = std::byte(value[i]);
    evidence(bytes, 128, target->colour_observation);
    evidence(bytes, 160, target->headlights_observation);
  }
  const auto settings = software_settings(controller_.settings());
  std::copy(settings.begin(), settings.end(), bytes.begin() + 192);
  return bytes;
}
void SoftwareModel::retire(Clock::time_point now) {
  controller_.retire(now);
  retired_ = true;
}
}  // namespace headunit::ambient
