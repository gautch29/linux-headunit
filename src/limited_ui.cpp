// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/limited_ui.hpp"

#include <cmath>
#include <mutex>

namespace headunit {
namespace {
std::mutex receiver_mutex;
std::optional<LimitedUiPolicy> receiver_policy;
}  // namespace

std::optional<LimitedUiPolicy> parse_limited_ui_policy(std::string_view text) {
  if (text == "speed") return LimitedUiPolicy{};
  return std::nullopt;
}

bool limited_ui_next(const LimitedUiPolicy& policy, bool limited, std::optional<double> speed_kmh) {
  if (!speed_kmh || !std::isfinite(*speed_kmh) || *speed_kmh < 0) return limited;
  if (!limited) return *speed_kmh > policy.enter_kmh;
  return *speed_kmh >= policy.exit_kmh;
}

void set_receiver_limited_ui(std::optional<LimitedUiPolicy> policy) {
  std::lock_guard lock(receiver_mutex);
  receiver_policy = policy;
}

std::optional<LimitedUiPolicy> receiver_limited_ui() {
  std::lock_guard lock(receiver_mutex);
  return receiver_policy;
}

}  // namespace headunit
