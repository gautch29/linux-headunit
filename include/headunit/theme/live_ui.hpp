// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/asset_bundle.hpp>
#include <headunit/theme/gpu_display.hpp>
#include <headunit/theme/ui_session.hpp>
#include <mutex>
#include <headunit/touch_mailbox.hpp>
#include <array>
#include <headunit/display_configuration.hpp>
#include <headunit/theme/receiver_video.hpp>
#include <headunit/theme/warning_overlay.hpp>
#include <headunit/camera.hpp>
#include <headunit/presentation_status.hpp>
#include <headunit/theme/camera_state.hpp>
#include <headunit/theme/content_control.hpp>
#include <headunit/theme/layout_control.hpp>

namespace headunit::theme::script {
// Copied clock anchors only. Worker callbacks never touch receiver sockets.
class UiClockSnapshot {
 public:
  void update(std::optional<AirplayClockSample> value) {
    std::lock_guard lock(mutex_);
    value_ = value;
  }
  std::optional<AirplayClockSample> sample() const {
    std::lock_guard lock(mutex_);
    return value_;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<AirplayClockSample> value_;
};
// Validated before the receiver listens. Captures immutable verified resources;
// each service generation gets its own caches, VMs and GPU owners.
class InstalledUi {
 public:
  // Verify the same configuration, signed assets, profiles and local endpoints
  // as open(), without creating or locking a camera surface lease. This does
  // not construct a worker, execute scripts or establish a phone connection.
  static AssetBundleInfo validate_configuration(const std::string& local_config);
  static std::shared_ptr<InstalledUi> open(const std::string& local_config);
  struct LocalClusterResources {
    ResourceReader asset;
    Bytes vehicle_profile;
    std::string startup_layout, preference_path;
    std::string drive_mode_profiles_file, drive_mode_scope;
    std::optional<graphics::WarningOverlayConfiguration> warning_overlay;
  };
  // Read-only verification: no camera lease, phone clock, connection or VM.
  static LocalClusterResources local_cluster_resources(const std::string& local_config);
  const AssetBundleInfo& asset_info() const { return bundle_->info(); }
  bool diagnostics_enabled() const noexcept { return diagnostics_; }
  std::string cluster_input_file() const {
    return cluster_layout_file_.empty() ? std::string{} : cluster_layout_file_ + ".input";
  }
  const std::optional<camera::Configuration>& camera_configuration() const noexcept {
    return camera_;
  }
  const std::string& camera_status_file() const noexcept { return camera_status_file_; }
  void validate_content_request(const vehicle::ContentRequestCapability& capability) const;
  UiSession::Factory factory(
      std::string main_uuid, std::string cluster_uuid, std::shared_ptr<UiClockSnapshot>,
      std::shared_ptr<video::ReceiverVideo> video = {},
      std::shared_ptr<headunit::TouchMailbox> touches = {},
      ReceiverDisplays displays = default_receiver_displays(),
      std::shared_ptr<MainPresentationSnapshot> main_presentation = {},
      std::shared_ptr<vehicle::CameraStateSnapshot> camera_state = {},
      LayoutControl::Publisher layout_publisher = {}, bool newly_paired = false,
      std::shared_ptr<vehicle::ContentStateSnapshot> content_state = {}) const;

 private:
  static std::shared_ptr<InstalledUi> open_configuration(const std::string& local_config,
                                                         bool read_only);
  std::shared_ptr<const AssetBundle> bundle_;
  std::vector<std::uint8_t> local_profile_;
  std::array<std::string, 2> wayland_displays_;
  bool diagnostics_ = false, simulate_gauges_ = false, overlap_script_tick_ = false;
  bool defer_scene_fence_ = false;
  bool opaque_windows_ = false;
  bool isolate_cluster_output_faults_ = false;
  bool reveal_cluster_underlay_while_waiting_ = false;
  std::array<unsigned, 2> presentation_queue_depths_{1, 1};
  unsigned feedback_poll_ms_ = 0;
  unsigned video_presentation_advance_ms_ = 0;
  unsigned diagnostic_reports_ = 12;
  unsigned script_log_lines_ = 0;
  std::string vehicle_socket_, drive_mode_profiles_file_;
  std::string
      cluster_startup_layout_;  // Optional bench selection from the verified pack's catalogue.
  std::string
      cluster_layout_file_;  // Local buttons request changes through the existing UI-Sync session.
  std::string display_response_file_;        // Optional native-page touch/smoothness preference.
  std::string startup_warning_lights_file_;  // Native Settings startup overlay preference.
  bool persist_cluster_layout_ = false, first_pairing_animation_ = false;
  bool bench_assistance_view_ = false;
  bool brand_greeting_ = false;
  std::string greeting_sequence_file_;
  std::optional<graphics::WarningOverlayConfiguration> warning_overlay_;
  std::optional<camera::Configuration> camera_;
  std::string camera_control_file_, camera_status_file_;
  std::string native_camera_control_file_, native_camera_lease_file_;
  std::string native_page_control_file_, native_page_lease_file_;
};
}  // namespace headunit::theme::script
