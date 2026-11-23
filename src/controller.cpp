// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/controller.hpp>

#include <stdexcept>

namespace headunit {

namespace {
bool recent(const std::optional<Clock::time_point>& frame, Clock::time_point now,
            Clock::duration age) {
  return frame && *frame <= now && now - *frame <= age;
}
}  // namespace

Controller::Controller(Clock::duration maximum_frame_age) : maximum_frame_age_(maximum_frame_age) {
  if (maximum_frame_age_ <= Clock::duration::zero()) {
    throw std::invalid_argument("frame age must be positive");
  }
}

DisplayDecision Controller::update(const PhonePresentation& phone, Clock::time_point now) {
  current_ = {};
  if (!phone.authenticated || !recent(phone.main_presented_at, now, maximum_frame_age_)) {
    return current_;
  }
  if (phone.mode == PhoneMode::standard) {
    current_.main = MainSurface::carplay;
  } else if (phone.mode == PhoneMode::ultra && phone.theme_verified) {
    current_.main = MainSurface::carplay;
    if (recent(phone.cluster_presented_at, now, maximum_frame_age_))
      current_.cluster = ClusterSurface::carplay;
  }
  return current_;
}

}  // namespace headunit
