// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/local_cluster.hpp>
#include <headunit/theme/local_cluster_selection.hpp>
#include <headunit/theme/live_ui.hpp>
#include <headunit/theme/gauge_input.hpp>
#include <headunit/drive_mode_profile.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <headunit/theme/scene_lifecycle.hpp>
#include <headunit/theme/greeting_overlay.hpp>
#include <headunit/theme/local_presentation_diagnostics.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>

namespace headunit::theme::script {
namespace {
class BorrowedVehicle final : public headunit::VehicleSource {
 public:
  explicit BorrowedVehicle(headunit::VehicleSource& source) : source_(source) {}
  headunit::VehicleSample sample(headunit::Clock::time_point at) override {
    return source_.sample(at);
  }

 private:
  headunit::VehicleSource& source_;
};
std::uint64_t milliseconds(headunit::Clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}
}  // namespace
void run_local_cluster(const std::string& path, headunit::VehicleSource& source, unsigned width,
                       unsigned height, const std::string& output, GreetingSequenceFile* greeting,
                       const std::function<bool()>& stopping) {
  if (width != 1920 || height != 720 || output.empty())
    throw std::invalid_argument("local signed gauges require the authored 1920x720 cluster");
  const auto began = headunit::Clock::now();
  graphics::LocalPresentationDiagnostics diagnostic;
  diagnostic.entered_ns = graphics::local_diagnostic_clock();
  std::uint64_t token = 0, presented = 0;
  struct Retirement {
    headunit::Clock::time_point began;
    const std::uint64_t& presented;
    const graphics::LocalPresentationDiagnostics& diagnostic;
    ~Retirement() {
      std::clog << "Local Ultra gauges retired; actualPresentations=" << presented
                << " phoneFramesOffered=0 retiredMonotonicNs="
                << std::chrono::duration_cast<std::chrono::nanoseconds>(
                       headunit::Clock::now().time_since_epoch())
                       .count()
                << " wallMs="
                << std::chrono::duration_cast<std::chrono::milliseconds>(headunit::Clock::now() -
                                                                         began)
                       .count()
                << '\n';
      graphics::write_local_presentation_diagnostics(std::clog, diagnostic);
    }
  } retirement{began, presented, diagnostic};
  auto resources = InstalledUi::local_cluster_resources(path);
  auto profile = vehicle::decode_local_profile(resources.vehicle_profile);
  vehicle::GaugeInput gauges(profile, std::make_unique<BorrowedVehicle>(source));
  std::optional<std::string> saved;
  if (!resources.preference_path.empty()) {
    PreferenceFile preferences(resources.preference_path);
    preferences.wait();  // Disk owner preloads before entry-point/task startup.
    saved = preferences.snapshot().data;
  }
  std::unique_ptr<headunit::DriveModeProfilesFile> drive_modes;
  if (!resources.drive_mode_profiles_file.empty())
    drive_modes = std::make_unique<headunit::DriveModeProfilesFile>(
        resources.drive_mode_profiles_file, resources.drive_mode_scope);
  auto now = headunit::Clock::now();
  const auto origin = now;
  gauges.pump(now);  // One source read; profile and gauges share original acquisition dates.
  auto mode = drive_modes ? drive_modes->poll(gauges.sample(), now) : DriveModePresentation{};
  struct LocalScene {
    ~LocalScene() {
      if (empty_remote) empty_remote->close();
    }
    std::unique_ptr<Runtime> runtime;
    std::shared_ptr<video::Stream> empty_remote;
    std::unique_ptr<SceneLifecycle> lifecycle;
  };
  const auto prepare = [&](const std::optional<DriveModeProfile>& requested) {
    auto selection =
        local_cluster_selection(resources.asset, resources.startup_layout,
                                saved ? std::optional<std::string_view>(*saved) : std::nullopt);
    if (requested) {
      if (requested->layout.empty())
        throw std::invalid_argument("Profile has no signed layout mapping");
      const auto record =
          requested->layout + "\n" + (requested->palette.empty() ? "" : requested->palette + "\n");
      selection = local_cluster_selection(resources.asset, resources.startup_layout, record);
      if (selection.layout != requested->layout || selection.palette != requested->palette)
        throw std::invalid_argument("Profile layout or palette is absent from this pack");
    }
    auto result = std::make_unique<LocalScene>();
    result->runtime =
        std::make_unique<Runtime>(resources.asset, Display{"Cluster_Display", 1920, 720});
    auto& runtime = *result->runtime;
    runtime.bind_vehicle(profile.source, profile.bindings, [&] { return milliseconds(now); });
    runtime.enable_scene_observation();
    runtime.bind_verification_reporter([](const auto&) {});
    result->empty_remote = runtime.add_video_input({"iPhoneVideo", 1920, 720});
    if (!selection.layout.empty()) runtime.select_startup_layout(selection.layout);
    if (!selection.palette.empty()) {
      runtime.evaluate("require('State').setPaletteSelections({['" + selection.layout + "']='" +
                           selection.palette + "'})",
                       "local-palette-startup");
    }
    result->lifecycle = std::make_unique<SceneLifecycle>(profile, SceneLifecycle::Launch::resume);
    result->lifecycle->pump(milliseconds(now), Runtime::ScenePhase::unavailable, false);
    runtime.tick(std::chrono::duration<double>(now - origin).count());
    runtime.evaluate("require 'main'", "local-cluster-startup");
    runtime.set_foregrounded(true);
    return result;
  };
  // One compositor remains alive. Prepare the successor VM off the control
  // thread, retain current pixels until its complete DRIVE scene is available.
  std::string profile_error;
  std::unique_ptr<LocalScene> active;
  try {
    active = prepare(mode.profile);
  } catch (const std::exception& e) {
    profile_error = e.what();
    active = prepare({});
  }
  std::unique_ptr<LocalScene> candidate;
  auto attempted_profile = mode.profile;
  auto candidate_started = now;
  auto next_status = now;
  graphics::WaylandOptions window;
  window.title = "Local Ultra gauges";
  window.output_name = output;
  window.app_id = "headunit.backup-cluster";
  window.background_layer = true;
  window.opaque_background = true;
  window.local_diagnostics = &diagnostic;
  if (stopping()) return;
  graphics::GpuCompositor gpu(1920, 720, window);
  std::unique_ptr<graphics::WarningOverlay> warnings;
  if (resources.warning_overlay && resources.warning_overlay->enabled)
    warnings = std::make_unique<graphics::WarningOverlay>(*resources.warning_overlay);
  auto next = headunit::Clock::now();
  std::clog << "Local Ultra gauges started; phoneConnected=false animationClock=local-monotonic\n";
  while (!stopping()) {
    while (auto receipt = gpu.poll_presentation()) {
      if (receipt->presented) {
        if (!presented)
          std::clog << "Local start-to-first-presentation us="
                    << std::chrono::duration_cast<std::chrono::microseconds>(
                           headunit::Clock::now() - began)
                           .count()
                    << '\n';
        ++presented;
      }
    }
    now = headunit::Clock::now();
    if (now >= next && gpu.presentation_slot_available()) {
      if (!active->empty_remote->unused())
        throw std::runtime_error("disconnected local gauges received unexpected phone frames");
      gauges.pump(now);
      if (drive_modes) {
        mode = drive_modes->poll(gauges.sample(), now);
        if (mode.profile != attempted_profile) {
          attempted_profile = mode.profile;
          candidate.reset();
          profile_error.clear();
          try {
            candidate = prepare(mode.profile);
            candidate_started = now;
          } catch (const std::exception& e) {
            profile_error = e.what();
          }
        }
      }
      active->runtime->tick(std::chrono::duration<double>(now - origin).count());
      active->lifecycle->pump(milliseconds(now), active->runtime->scene_phase(), presented > 0);
      if (candidate) {
        try {
          candidate->runtime->tick(std::chrono::duration<double>(now - origin).count());
          candidate->lifecycle->pump(milliseconds(now), candidate->runtime->scene_phase(), false);
          if (candidate->runtime->scene_phase() == Runtime::ScenePhase::drive &&
              candidate->runtime->scene_root()) {
            active->empty_remote->close();
            active = std::move(candidate);
          } else if (now - candidate_started >= std::chrono::seconds(5)) {
            candidate.reset();
            profile_error = "Local profile did not produce a complete DRIVE scene";
          }
        } catch (const std::exception& e) {
          candidate.reset();
          profile_error = e.what();
        }
      }
      if (drive_modes && now >= next_status) {
        next_status = now + std::chrono::milliseconds(250);
        const auto phase = candidate                ? "pending"
                           : !profile_error.empty() ? "error"
                           : mode.profile           ? "local-profile"
                                                    : "unavailable";
        publish_drive_mode_status(
            resources.drive_mode_profiles_file + ".native.status", resources.drive_mode_scope, mode,
            phase, !profile_error.empty() ? profile_error : drive_modes->error(), now);
      }
      auto root = std::const_pointer_cast<graphics::Layer>(active->runtime->scene_root());
      if (root && greeting)
        root = std::const_pointer_cast<graphics::Layer>(graphics::with_scene_overlay(
            root, graphics::greeting_overlay(greeting->frame(now), {1920, 720})));
      if (root && warnings) {
        auto composite = std::make_shared<graphics::Layer>();
        composite->bounds.size = {1920, 720};
        composite->anchor = {0, 0};
        composite->children = {root, std::const_pointer_cast<graphics::Layer>(warnings->scene(
                                         gauges.sample(), now, {1920, 720}, false))};
        root = std::move(composite);
      }
      if (!root) throw std::runtime_error("local signed gauges have no scene");
      gpu.compose_scene(root);
      gpu.submit(++token);
      next = now + std::chrono::nanoseconds(33333333);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  active->empty_remote->close();
}
}  // namespace headunit::theme::script
