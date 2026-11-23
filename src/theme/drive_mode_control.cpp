// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/drive_mode_control.hpp>
#include <algorithm>

namespace headunit::theme::script {
DriveModeControl::DriveModeControl(std::string path, std::string scope,
                                   LayoutControl::Publisher publisher)
    : profiles_(std::move(path), std::move(scope)),
      phase_("unavailable"),
      publisher_(std::move(publisher)) {}
void DriveModeControl::poll(const VehicleSample& sample, Clock::time_point now,
                            const LayoutControl::ClusterSelection& select) {
  if (now < next_poll_) return;
  next_poll_ = now + std::chrono::milliseconds(250);
  const auto mode = profiles_.poll(sample, now);
  if (mode.profile != target_) {
    target_ = mode.profile;
    phase_ = target_ ? "waiting" : "unavailable";
    error_.clear();
    requested_at_ = {};
  }
  if (!profiles_.error().empty()) error_ = profiles_.error();
  if (target_) {
    auto state = select({});
    const auto& profile = *target_;
    const auto declared =
        std::any_of(state.choices.begin(), state.choices.end(),
                    [&](const auto& c) { return c.identifier == profile.layout; });
    const auto palette_declared =
        profile.palette.empty() ||
        std::any_of(state.palettes.begin(), state.palettes.end(),
                    [&](const auto& c) { return c.identifier == profile.palette; });
    const bool idle = state.ready && state.idle && !state.transitioning;
    const bool matches = idle && state.active == profile.layout &&
                         (profile.palette.empty() || state.active_palette == profile.palette);
    if (profile.layout.empty()) {
      phase_ = "unsupported";
      error_ = "Profile has no signed layout mapping";
    } else if (state.ready && (!declared || !palette_declared)) {
      phase_ = "unsupported";
      error_ = "Profile layout or palette is absent from this pack";
    } else if (matches) {
      phase_ = profile.wallpaper == "phone" ? "confirmed" : "partial";
      error_ = profile.wallpaper == "phone" ? "" : "Signed wallpaper has no qualified command";
    } else if (phase_ == "confirmed" || phase_ == "partial" || phase_ == "changed-on-phone") {
      phase_ = "changed-on-phone";
      error_.clear();
    } else if (phase_ != "error" && phase_ != "unsupported") {
      if (requested_at_ != Clock::time_point{} && now - requested_at_ >= std::chrono::seconds(5)) {
        phase_ = "error";
        error_ = "Phone did not confirm the complete layout and palette profile";
      } else if (idle && requested_at_ == Clock::time_point{}) {
        ClusterSelectionRequest request;
        request.layout = profile.layout;
        request.palette = profile.palette;
        state = select(request);
        if (state.result == LayoutRequest::unknown || state.result == LayoutRequest::unavailable) {
          phase_ = "error";
          error_ = "Signed scene rejected the profile request";
        } else {
          requested_at_ = now;
          phase_ = "pending";
        }
      }
    }
  }
  publish_drive_mode_status(profiles_.path() + ".ultra.status", profiles_.scope(), mode, phase_,
                            error_, now, publisher_.nonce, publisher_.connection_generation);
}
}  // namespace headunit::theme::script
