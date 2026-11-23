// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/night_mode.hpp"
#include "headunit/vehicle_facts.hpp"

#include <ctime>
#include <mutex>

namespace headunit {
namespace {

std::optional<unsigned> clock_minute(std::string_view text) {
  if (text.size() != 5 || text[2] != ':') return std::nullopt;
  for (const auto index : {0, 1, 3, 4})
    if (text[index] < '0' || text[index] > '9') return std::nullopt;
  const unsigned hours = static_cast<unsigned>((text[0] - '0') * 10 + (text[1] - '0'));
  const unsigned minutes = static_cast<unsigned>((text[3] - '0') * 10 + (text[4] - '0'));
  if (hours > 23 || minutes > 59) return std::nullopt;
  return hours * 60 + minutes;
}

std::mutex receiver_mutex;
std::optional<NightModePolicy> receiver_policy;

}  // namespace

std::optional<NightModePolicy> parse_night_mode_policy(std::string_view text) {
  if (text == "off") return NightModePolicy{NightModePolicy::Kind::off};
  if (text == "on") return NightModePolicy{NightModePolicy::Kind::on};
  if (text == "schedule") return NightModePolicy{NightModePolicy::Kind::schedule};
  if (text == "headlights") return NightModePolicy{NightModePolicy::Kind::headlights};
  constexpr std::string_view prefix = "schedule:";
  if (!text.starts_with(prefix) || text.size() != prefix.size() + 11 ||
      text[prefix.size() + 5] != '-')
    return std::nullopt;
  const auto start = clock_minute(text.substr(prefix.size(), 5));
  const auto end = clock_minute(text.substr(prefix.size() + 6, 5));
  if (!start || !end || *start == *end) return std::nullopt;
  return NightModePolicy{NightModePolicy::Kind::schedule, *start, *end};
}

bool night_mode_at(const NightModePolicy& policy, unsigned minute_of_day,
                   std::optional<bool> low_beam) {
  if (policy.kind == NightModePolicy::Kind::headlights && low_beam) return *low_beam;
  if (policy.kind == NightModePolicy::Kind::off) return false;
  if (policy.kind == NightModePolicy::Kind::on) return true;
  minute_of_day %= 24 * 60;
  if (policy.start_minute < policy.end_minute)
    return minute_of_day >= policy.start_minute && minute_of_day < policy.end_minute;
  return minute_of_day >= policy.start_minute || minute_of_day < policy.end_minute;
}

void set_receiver_night_mode(std::optional<NightModePolicy> policy) {
  std::lock_guard lock(receiver_mutex);
  receiver_policy = policy;
}

std::optional<bool> receiver_night_mode_now() {
  std::optional<NightModePolicy> policy;
  {
    std::lock_guard lock(receiver_mutex);
    policy = receiver_policy;
  }
  if (!policy) return std::nullopt;
  const auto now = std::time(nullptr);
  std::tm local{};
  if (!localtime_r(&now, &local)) return std::nullopt;
  std::optional<bool> low_beam;
  if (const auto facts = fresh_vehicle_facts()) low_beam = facts->low_beam;
  return night_mode_at(*policy, static_cast<unsigned>(local.tm_hour * 60 + local.tm_min), low_beam);
}

}  // namespace headunit
