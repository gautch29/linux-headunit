// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/camera_visibility.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace headunit::theme::vehicle {
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::invalid_argument(why);
}
constexpr std::array<std::string_view, 3> uid_path{"automakerInputStreams", "criticalInputStream",
                                                   "inputStreamUID"};
constexpr std::array<std::string_view, 3> on_path{"automakerInputStreams", "criticalInputStream",
                                                  "on"};
bool path_is(const ScriptBinding& binding, const std::array<std::string_view, 3>& path) {
  return binding.path.size() == path.size() &&
         std::equal(binding.path.begin(), binding.path.end(), path.begin());
}
bool mapped_input(const LocalProfile& profile, std::int64_t iid) {
  return std::any_of(profile.vehicle_inputs.begin(), profile.vehicle_inputs.end(),
                     [iid](const auto& input) { return input.iid == iid; });
}
}  // namespace
CameraVisibility::CameraVisibility(const LocalProfile& profile, std::string_view identifier,
                                   std::uint64_t age_ms, std::uint64_t now_ns)
    : source_(profile.source), last_now_ns_(now_ns) {
  check(source_ && !identifier.empty() && identifier.size() <= 64 &&
            identifier.find('\0') == identifier.npos,
        "invalid local camera visibility identity");
  check(age_ms > 0 && age_ms <= 1000, "invalid local camera visibility age");
  maximum_age_ns_ = age_ms * 1000000;
  std::map<std::string, const ScriptBinding*> uids, ons;
  std::set<std::int64_t> fields;
  for (const auto& binding : profile.bindings) {
    if (!path_is(binding, uid_path) && !path_is(binding, on_path)) continue;
    check(binding.instance && !binding.instance->empty(),
          "camera visibility requires a repeated service instance");
    check(fields.insert(binding.iid).second, "duplicate camera visibility IID");
    auto& entries = path_is(binding, uid_path) ? uids : ons;
    check(entries.emplace(*binding.instance, &binding).second,
          "duplicate camera visibility instance field");
  }
  check(!uids.empty(), "camera visibility UID binding is missing");
  std::vector<std::int64_t> uid_ids;
  uid_ids.reserve(uids.size());
  for (const auto& [instance, binding] : uids) {
    (void)instance;
    const auto policy = source_->policy(binding->iid);
    check(policy && policy->kind == ValueKind::string && !source_->timed(binding->iid) &&
              !mapped_input(profile, binding->iid),
          "camera visibility UID must be fixed local configuration");
    uid_ids.push_back(binding->iid);
  }
  const auto now_ms = now_ns / 1000000;
  const auto readings = source_->snapshot(uid_ids, now_ms);
  std::optional<std::string> match;
  for (const auto& [instance, binding] : uids) {
    const auto& reading = readings.at(binding->iid);
    check(reading.value && std::holds_alternative<std::string>(reading.value->data),
          "camera visibility UID is unavailable");
    if (std::get<std::string>(reading.value->data) == identifier) {
      check(!match, "ambiguous camera visibility UID");
      match = instance;
    }
  }
  check(match && ons.contains(*match), "camera visibility on binding is missing");
  const auto* on = ons.at(*match);
  const auto policy = source_->policy(on->iid);
  check(on->iid != uids.at(*match)->iid && policy && policy->kind == ValueKind::boolean &&
            source_->timed(on->iid) && !mapped_input(profile, on->iid),
        "camera visibility on must be an owner-controlled timed Boolean");
  // Camera freshness includes the last nanosecond at its age limit; Service
  // expires at a whole-ms exclusive boundary. The extra millisecond prevents
  // the script from observing an invalid sentinel while a frame is still fresh.
  check(policy->max_age_ms >= age_ms + 1,
        "camera visibility signal expires before source freshness");
  on_iid_ = on->iid;
  source_->deactivate_boolean(on_iid_, now_ms);
  last_published_ms_ = now_ms;
  off_cutoff_ns_ = now_ns;
}
void CameraVisibility::observe(std::uint64_t now_ns) {
  check(now_ns >= last_now_ns_, "camera visibility owner clock reversed");
  last_now_ns_ = now_ns;
}
void CameraVisibility::withdraw(std::uint64_t now_ns) {
  observe(now_ns);
  source_->deactivate_boolean(on_iid_, now_ns / 1000000);
  if (visible_) off_cutoff_ns_ = now_ns;
  visible_ = false;
}
bool CameraVisibility::refresh(bool active, std::uint64_t capture_ns, std::uint64_t now_ns) {
  observe(now_ns);
  if (!active || capture_ns > now_ns || now_ns - capture_ns > maximum_age_ns_ ||
      (last_capture_ns_ && capture_ns < *last_capture_ns_)) {
    withdraw(now_ns);
    return false;
  }
  if (last_capture_ns_ && capture_ns == *last_capture_ns_) return false;
  const auto capture_ms = capture_ns / 1000000;
  // Keep the service's real acquisition frontier strict. A first capture in
  // the startup OFF millisecond is skipped until a genuinely newer capture;
  // do not invent an extra millisecond or relabel it with polling time.
  if (capture_ns <= off_cutoff_ns_ || capture_ms <= last_published_ms_) return false;
  source_->publish_visibility_capture(on_iid_, capture_ms, now_ns / 1000000);
  last_capture_ns_ = capture_ns;
  last_published_ms_ = capture_ms;
  visible_ = true;
  return true;
}
}  // namespace headunit::theme::vehicle
