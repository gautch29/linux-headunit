// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/scene_lifecycle.hpp>
#include <stdexcept>

namespace headunit::theme::script {
SceneLifecycle::SceneLifecycle(const vehicle::LocalProfile& profile, Launch launch)
    : source_(profile.source),
      ids_(profile.scene_inputs.value_or(vehicle::LocalProfile::SceneInputs{0, 0})),
      launch_(launch),
      driving_(launch == Launch::resume) {
  if (!source_ || !profile.scene_inputs)
    throw std::invalid_argument("scene lifecycle needs a session profile");
  const auto state = source_->policy(ids_.state), options = source_->policy(ids_.options);
  if (!state || !options || !source_->timed(ids_.state) || !source_->timed(ids_.options) ||
      state->kind != vehicle::ValueKind::integer || options->kind != vehicle::ValueKind::array)
    throw std::invalid_argument("scene lifecycle needs timed state/options inputs");
}
void SceneLifecycle::pump(std::uint64_t now, Runtime::ScenePhase phase, bool visible) {
  if (retired_) throw std::logic_error("retired scene lifecycle");
  if (last_publication_ && now < *last_publication_)
    throw std::invalid_argument("reversed scene owner clock");
  if (!driving_ && visible) {
    if (phase == Runtime::ScenePhase::logos) {
      saw_logos_ = true;
      if (!animation_started_) animation_started_ = now;
    }
    if (saw_logos_ && (phase == Runtime::ScenePhase::startup ||
                       (launch_ == Launch::first_pairing && phase == Runtime::ScenePhase::drive)))
      driving_ = true;
    // A stuck signed animation retires the worker; expiry never asserts DRIVE.
    if (!driving_ && animation_started_ && now - *animation_started_ >= 15000)
      throw std::runtime_error("signed startup animation did not complete");
  }
  // Ordered source publications have millisecond resolution. Retry a same-ms
  // decision on the next owner turn, without inventing a timestamp.
  if (last_publication_ == now) return;
  const auto state = std::int64_t(driving_ || launch_ == Launch::first_pairing ? 2 : 1);
  opack::Array options;
  if (!driving_ && launch_ == Launch::first_pairing)
    options.push_back(opack::Value{std::string("AllowPairingAnimation")});
  source_->publish_batch({{ids_.state, opack::Value{state}, now},
                          {ids_.options, opack::Value{std::move(options)}, now}},
                         now);
  last_publication_ = now;
}
void SceneLifecycle::retire(std::uint64_t now) {
  if (retired_) return;
  source_->publish_batch({{ids_.state, {}, {}}, {ids_.options, {}, {}}}, now);
  retired_ = true;
}
}  // namespace headunit::theme::script
