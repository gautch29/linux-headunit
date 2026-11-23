// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/input_presentation_report.hpp>
#include <headunit/theme/display_report.hpp>
#include <string_view>
#include <cstdlib>
#include <atomic>
#include <headunit/theme/live_ui.hpp>
#include <headunit/theme/local_vehicle_profile.hpp>
#include <headunit/theme/scene_lifecycle.hpp>
#include <headunit/theme/brand_greeting.hpp>
#include <headunit/theme/greeting_overlay.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <headunit/theme/gauge_input.hpp>
#include <headunit/theme/layout_control.hpp>
#include <headunit/theme/drive_mode_control.hpp>
#include <headunit/theme/display_response.hpp>
#include <headunit/theme/startup_warning_lights.hpp>
#include <headunit/theme/camera_input.hpp>
#include <headunit/theme/vehicle_view.hpp>
#include <headunit/theme/camera_visibility.hpp>
#include <headunit/camera_control.hpp>
#include <headunit/camera_controller_io.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <headunit/main_surface_control.hpp>
#include <headunit/camera_display.hpp>
#include <headunit/socket_vehicle.hpp>
#include <charconv>
#include <chrono>
#include <iostream>
#include <syncstream>
#include <array>
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit::theme::script {
namespace {
// Opt-in pack log for live layout diagnosis. The installed-UI setting
// diagnosticScriptLog=N (1..2000) forwards at most N lines per panel from the
// signed layout, workspace and transition-metadata modules, truncated to 400
// bytes, into the private receiver log. Absent means nothing is forwarded.
void bind_bounded_script_log(Runtime& runtime, std::size_t panel, unsigned limit) {
  if (!limit) return;
  auto remaining = std::make_shared<std::atomic<unsigned>>(limit);
  runtime.bind_log_sink([panel, remaining](int level, std::string_view tag,
                                           std::string_view message) {
    const bool wanted = tag.ends_with("Metadata") || tag.ends_with("LayoutController") ||
                        tag.ends_with("Workspace");
    if (!wanted) return;
    unsigned left = remaining->load();
    while (left && !remaining->compare_exchange_weak(left, left - 1)) {
    }
    if (!left) return;
    std::osyncstream(std::cerr) << "Ultra script log panel=" << panel << " level=" << level
                                << " tag=" << tag << " message=" << message.substr(0, 400) << '\n';
  });
}
// Owner-thread only, fixed storage. Counts cover every threshold event, while
// the retained maxima explain one cold and one warm offender per interval.
struct DisplayDeadlineWindow {
  std::uint64_t slow_draws = 0, warm_slow_draws = 0, long_gaps = 0, warm_long_gaps = 0;
  graphics::GpuSlowFrameTrace slowest{}, warm_slowest{};
  graphics::GpuPresentationGapTrace longest{}, warm_longest{};
  void observe(const graphics::GpuSlowFrameTrace& trace) {
    ++slow_draws;
    if (trace.compose_ns > slowest.compose_ns) slowest = trace;
    if (trace.ordinal > 64) {
      ++warm_slow_draws;
      if (trace.compose_ns > warm_slowest.compose_ns) warm_slowest = trace;
    }
  }
  void observe(const graphics::GpuPresentationGapTrace& trace) {
    ++long_gaps;
    if (trace.gap_ns > longest.gap_ns) longest = trace;
    if (trace.draw_ordinal > 64) {
      ++warm_long_gaps;
      if (trace.gap_ns > warm_longest.gap_ns) warm_longest = trace;
    }
  }
  void write(std::ostream& log, unsigned panel) const {
    log << "Ultra deadline diagnostic panel=" << panel << " slowDraws16ms=" << slow_draws
        << " warmSlowDraws16ms=" << warm_slow_draws << " longGaps20ms=" << long_gaps
        << " warmLongGaps20ms=" << warm_long_gaps << '\n';
    const auto draw = [&](const auto& t, const char* category) {
      if (!t.ordinal) return;
      log << "Ultra slow draw panel=" << panel << " category=" << category << " draw=" << t.ordinal
          << " composeMs=" << t.compose_ns / 1e6 << " validationMs=" << t.validation_ns / 1e6
          << " preparationMs=" << t.preparation_ns / 1e6 << " traversalMs=" << t.traversal_ns / 1e6
          << " flushMs=" << t.flush_ns / 1e6 << " completionWaitMs=" << t.completion_wait_ns / 1e6
          << " importMs=" << t.import_ns / 1e6 << " retainedBuilds=" << t.retained_builds
          << " strokeBuilds=" << t.stroke_builds << " strokeRasterMs=" << t.stroke_raster_ns / 1e6
          << " gpuCacheBytes=" << t.gpu_cache_bytes << '\n';
    };
    const auto gap = [&](const auto& t, const char* category) {
      if (!t.draw_ordinal) return;
      log << "Ultra long gap panel=" << panel << " category=" << category
          << " draw=" << t.draw_ordinal << " scene=" << t.scene_ordinal
          << " gapMs=" << t.gap_ns / 1e6 << " submitToPresentMs=" << t.submit_to_present_ns / 1e6
          << " presentToPollMs=" << t.present_to_poll_ns / 1e6
          << " refreshMs=" << t.refresh_ns / 1e6 << " queuedBehind=" << t.queued_behind << '\n';
    };
    draw(slowest, "interval");
    if (warm_slowest.ordinal != slowest.ordinal) draw(warm_slowest, "warm");
    gap(longest, "interval");
    if (warm_longest.draw_ordinal != longest.draw_ordinal) gap(warm_longest, "warm");
  }
};
std::vector<std::uint8_t> read_local(const std::string& path, std::size_t limit) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) throw std::runtime_error("cannot open local Ultra configuration");
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } close_fd{fd};
  struct stat st{};
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_uid != getuid() && st.st_uid != 0) ||
      (st.st_mode & 0022) || st.st_size <= 0 || std::uint64_t(st.st_size) > limit)
    throw std::runtime_error("invalid local Ultra configuration file");
  std::vector<std::uint8_t> data(std::size_t(st.st_size));
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto n = read(fd, data.data() + offset, data.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("short local Ultra configuration read");
    offset += std::size_t(n);
  }
  return data;
}
}  // namespace
std::shared_ptr<InstalledUi> InstalledUi::open(const std::string& path) {
  return open_configuration(path, false);
}
AssetBundleInfo InstalledUi::validate_configuration(const std::string& path) {
  return open_configuration(path, true)->asset_info();
}
InstalledUi::LocalClusterResources InstalledUi::local_cluster_resources(const std::string& path) {
  auto installed = open_configuration(path, true);
  const auto bundle = installed->bundle_;
  auto reader = cache_resources([bundle](std::string_view name) {
    const auto bytes = bundle->resource(std::string(name));
    return std::string(bytes.begin(), bytes.end());
  });
  const auto& asset = bundle->info();
  return {std::move(reader),
          installed->local_profile_,
          installed->cluster_startup_layout_,
          installed->persist_cluster_layout_
              ? installed->cluster_layout_file_ + ".confirmed." + asset.identifier + ":" +
                    std::to_string(asset.version)
              : std::string{},
          installed->drive_mode_profiles_file_,
          asset.identifier + ":" + std::to_string(asset.version),
          installed->warning_overlay_};
}
std::shared_ptr<InstalledUi> InstalledUi::open_configuration(const std::string& path,
                                                             bool read_only) {
  const auto bytes = read_local(path, 8192);
  const std::string text(bytes.begin(), bytes.end());
  std::map<std::string, std::string> fields;
  std::size_t at = 0;
  while (at < text.size()) {
    auto end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const auto line = text.substr(at, end - at);
    at = end + 1;
    const auto equal = line.find('=');
    if (equal == std::string::npos || equal == 0 || equal + 1 == line.size() ||
        line.find_first_of("\r\0", 0, 2) != std::string::npos ||
        !fields.emplace(line.substr(0, equal), line.substr(equal + 1)).second)
      throw std::invalid_argument("invalid local Ultra profile entry");
  }
  const std::array keys{"assetDirectory", "assetIdentifier", "assetVersion", "vehicleProfile"};
  auto certificate_time_policy = AssetCertificateTimePolicy::strict;
  if (auto entry = fields.find("allowExpiredAssetCertificate"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid asset certificate expiry flag");
    if (entry->second == "true")
      certificate_time_policy = AssetCertificateTimePolicy::allow_expired_leaf;
    fields.erase(entry);
  }
  std::optional<AcceptedAssetReference> acceptance_reference;
  if (auto entry = fields.find("acceptanceReference"); entry != fields.end()) {
    if (!std::filesystem::path(entry->second).is_absolute())
      throw std::invalid_argument("Ultra acceptance reference path must be absolute");
    acceptance_reference = read_asset_reference(entry->second);
    fields.erase(entry);
  }
  std::array<std::string, 2> display_names;
  const std::array display_keys{"centerWaylandDisplay", "clusterWaylandDisplay"};
  for (std::size_t i = 0; i < display_keys.size(); ++i) {
    if (auto entry = fields.find(display_keys[i]); entry != fields.end()) {
      const auto& name = entry->second;
      if (name.size() > 100 ||
          name.find_first_not_of(
              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != name.npos)
        throw std::invalid_argument("invalid local Wayland socket name");
      display_names[i] = name;
      fields.erase(entry);
    }
  }
  bool brand_greeting = false;
  if (auto entry = fields.find("clusterGreetingBrand"); entry != fields.end()) {
    if (entry->second != "asset" && entry->second != "brand")
      throw std::invalid_argument("invalid cluster greeting brand");
    brand_greeting = entry->second == "brand";
    fields.erase(entry);
  }
  bool first_pairing_animation = false;
  if (auto entry = fields.find("firstPairingAnimation"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid Ultra first-pairing animation flag");
    first_pairing_animation = entry->second == "true";
    fields.erase(entry);
  }
  bool diagnostics = false;
  if (auto entry = fields.find("diagnostics"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid Ultra diagnostics flag");
    diagnostics = entry->second == "true";
    fields.erase(entry);
  }
  bool overlap_script_tick = false;
  unsigned diagnostic_reports = 12;
  if (auto entry = fields.find("diagnosticReports"); entry != fields.end()) {
    const auto& value = entry->second;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), diagnostic_reports);
    if (error != std::errc{} || end != value.data() + value.size() || diagnostic_reports < 1 ||
        diagnostic_reports > 120)
      throw std::invalid_argument("invalid Ultra diagnostic report count");
    fields.erase(entry);
  }
  unsigned script_log_lines = 0;
  if (auto entry = fields.find("diagnosticScriptLog"); entry != fields.end()) {
    const auto& value = entry->second;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), script_log_lines);
    if (error != std::errc{} || end != value.data() + value.size() || script_log_lines < 1 ||
        script_log_lines > 2000)
      throw std::invalid_argument("invalid Ultra diagnostic script log line count");
    fields.erase(entry);
  }
  if (auto entry = fields.find("overlapScriptTick"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid Ultra overlap flag");
    overlap_script_tick = entry->second == "true";
    fields.erase(entry);
  }
  bool defer_scene_fence = false;
  if (auto entry = fields.find("deferSceneFence"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid Ultra GPU fence policy");
    defer_scene_fence = entry->second == "true";
    fields.erase(entry);
  }
  bool opaque_windows = false;
  if (auto entry = fields.find("opaqueWindows"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid Ultra window opacity policy");
    opaque_windows = entry->second == "true";
    fields.erase(entry);
  }
  bool isolate_cluster_output_faults = false;
  if (auto entry = fields.find("isolateClusterOutputFaults"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid Ultra cluster output-fault isolation policy");
    isolate_cluster_output_faults = entry->second == "true";
    fields.erase(entry);
  }
  bool reveal_cluster_underlay_while_waiting = false;
  if (auto entry = fields.find("revealClusterUnderlayWhileWaiting"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid Ultra cluster waiting-underlay policy");
    reveal_cluster_underlay_while_waiting = entry->second == "true";
    fields.erase(entry);
  }
  std::array<unsigned, 2> presentation_queue_depths{1, 1};
  if (auto entry = fields.find("presentationQueueDepth"); entry != fields.end()) {
    const auto& value = entry->second;
    unsigned depth = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), depth);
    if (error != std::errc{} || end != value.data() + value.size() || (depth != 1 && depth != 2))
      throw std::invalid_argument("invalid Ultra presentation queue depth");
    presentation_queue_depths = {depth, depth};
    fields.erase(entry);
  }
  for (unsigned index = 0; index < 2; ++index) {
    const char* key = index == 0 ? "mainPresentationQueueDepth" : "clusterPresentationQueueDepth";
    if (auto entry = fields.find(key); entry != fields.end()) {
      const auto& value = entry->second;
      unsigned depth = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), depth);
      if (error != std::errc{} || end != value.data() + value.size() || (depth != 1 && depth != 2))
        throw std::invalid_argument("invalid per-panel presentation queue depth");
      presentation_queue_depths[index] = depth;
      fields.erase(entry);
    }
  }
  unsigned feedback_poll_ms = 0;
  if (auto entry = fields.find("feedbackPollMs"); entry != fields.end()) {
    const auto& value = entry->second;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), feedback_poll_ms);
    if (error != std::errc{} || end != value.data() + value.size() || feedback_poll_ms > 4)
      throw std::invalid_argument("invalid Ultra presentation feedback polling interval");
    fields.erase(entry);
  }
  unsigned video_presentation_advance_ms = 0;
  if (auto entry = fields.find("videoPresentationAdvanceMs"); entry != fields.end()) {
    const auto& value = entry->second;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), video_presentation_advance_ms);
    if (error != std::errc{} || end != value.data() + value.size() ||
        std::uint64_t(video_presentation_advance_ms) * 1000000 >
            video::NativeInput::max_presentation_advance_ns)
      throw std::invalid_argument("invalid Ultra video presentation advance");
    fields.erase(entry);
  }
  bool simulate_gauges = false;
  if (auto entry = fields.find("simulateGauges"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid gauge simulation flag");
    simulate_gauges = entry->second == "true";
    fields.erase(entry);
  }
  std::string cluster_startup_layout;
  if (auto entry = fields.find("clusterStartupLayout"); entry != fields.end()) {
    const auto& value = entry->second;
    if (value.size() != 36)
      throw std::invalid_argument("invalid cluster startup layout identifier");
    for (std::size_t i = 0; i < value.size(); ++i) {
      const auto c = value[i];
      if ((i == 8 || i == 13 || i == 18 || i == 23)
              ? c != '-'
              : !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F'))
        throw std::invalid_argument("invalid cluster startup layout identifier");
    }
    cluster_startup_layout = value;
    fields.erase(entry);
  }
  std::string vehicle_socket;
  std::string cluster_layout_file;
  std::string drive_mode_profiles_file;
  if (auto entry = fields.find("driveModeProfilesFile"); entry != fields.end()) {
    drive_mode_profiles_file = entry->second;
    fields.erase(entry);
    (void)headunit::DriveModeProfilesFile(drive_mode_profiles_file);
  }
  if (auto entry = fields.find("clusterLayoutFile"); entry != fields.end()) {
    cluster_layout_file = entry->second;
    fields.erase(entry);
    // Validate the path and its private directory before the receiver listens.
    (void)LayoutControl(cluster_layout_file);
  }
  std::string display_response_file;
  if (auto entry = fields.find("displayResponseFile"); entry != fields.end()) {
    display_response_file = entry->second;
    fields.erase(entry);
    (void)DisplayResponseFile(display_response_file);  // Absolute-path check only.
  }
  std::string greeting_sequence_file;
  if (auto entry = fields.find("greetingSequenceFile"); entry != fields.end()) {
    greeting_sequence_file = entry->second;
    if (!std::filesystem::path(greeting_sequence_file).is_absolute())
      throw std::invalid_argument("greeting preference must be absolute");
    fields.erase(entry);
  }
  std::string startup_warning_lights_file;
  if (auto entry = fields.find("startupWarningLightsFile"); entry != fields.end()) {
    startup_warning_lights_file = entry->second;
    fields.erase(entry);
    (void)StartupWarningLightsFile(startup_warning_lights_file);
  }
  bool persist_cluster_layout = false;
  if (auto entry = fields.find("persistClusterLayout"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("invalid cluster preference flag");
    persist_cluster_layout = entry->second == "true";
    fields.erase(entry);
  }
  if (persist_cluster_layout && cluster_layout_file.empty())
    throw std::invalid_argument("cluster preference needs a layout control file");
  if (auto entry = fields.find("vehicleSocket"); entry != fields.end()) {
    vehicle_socket = entry->second;
    fields.erase(entry);
    if (!std::filesystem::path(vehicle_socket).is_absolute() || vehicle_socket.size() > 107 ||
        vehicle_socket.find('\0') != vehicle_socket.npos)
      throw std::invalid_argument("invalid local vehicle socket path");
  }
  if (simulate_gauges && !vehicle_socket.empty())
    throw std::invalid_argument("simulation and sensor input are mutually exclusive");
  std::optional<graphics::WarningOverlayConfiguration> warning_overlay;
  if (auto entry = fields.find("warningOverlayProfile"); entry != fields.end()) {
    if (!std::filesystem::path(entry->second).is_absolute())
      throw std::invalid_argument("warning overlay profile path must be absolute");
    const auto data = read_local(entry->second, 65536);
    warning_overlay = graphics::decode_warning_overlay_configuration(
        {reinterpret_cast<const char*>(data.data()), data.size()});
    if (warning_overlay->enabled && !simulate_gauges && vehicle_socket.empty())
      throw std::invalid_argument("warning overlay needs a configured vehicle source");
    fields.erase(entry);
  }
  bool bench_assistance_view = false;
  if (auto entry = fields.find("benchAssistanceView"); entry != fields.end()) {
    if (entry->second != "true" && entry->second != "false")
      throw std::invalid_argument("bench assistance flag must be boolean");
    bench_assistance_view = entry->second == "true";
    fields.erase(entry);
  }
  if (bench_assistance_view && (!simulate_gauges || !vehicle_socket.empty()))
    throw std::invalid_argument("bench assistance requires explicit simulated vehicle inputs");
  std::optional<camera::Configuration> camera_configuration;
  std::string camera_control_file;
  if (auto entry = fields.find("cameraProfile"); entry != fields.end()) {
    if (!std::filesystem::path(entry->second).is_absolute())
      throw std::invalid_argument("camera profile path must be absolute");
    const auto data = read_local(entry->second, 8192);
    camera_configuration =
        camera::decode_configuration({reinterpret_cast<const char*>(data.data()), data.size()});
    fields.erase(entry);
  }
  if (auto entry = fields.find("cameraControlFile"); entry != fields.end()) {
    camera_control_file = entry->second;
    (void)camera::ActivationControl(camera_control_file);
    fields.erase(entry);
    if (!camera_configuration) throw std::invalid_argument("camera control needs a camera profile");
  }
  std::string camera_status_file;
  if (auto entry = fields.find("cameraStatusFile"); entry != fields.end()) {
    const std::filesystem::path path(entry->second);
    if (!path.is_absolute() || path.lexically_normal() != path || path.filename().empty() ||
        entry->second == camera_control_file || !camera_configuration ||
        !camera_configuration->enabled)
      throw std::invalid_argument(
          "camera status needs a distinct absolute path and enabled camera");
    camera_status_file = entry->second;
    fields.erase(entry);
  }
  if (camera_configuration && camera_configuration->enabled) {
    const auto& c = *camera_configuration;
    if (!c.manual_activation || camera_control_file.empty())
      throw std::invalid_argument("Ultra camera needs explicit manual control");
    if (c.panel != camera::Panel::main || c.stream_identifier != "camera" || c.width != 1650 ||
        c.height != 720 || c.canvas_width != 1920 || c.canvas_height != 720 ||
        c.destination.x != 135 || c.destination.y != 0 || c.destination.width != 1650 ||
        c.destination.height != 720)
      throw std::invalid_argument("camera profile does not match the inspected Ultra slot");
    if (c.rotation_degrees || c.mirror_horizontal)
      throw std::invalid_argument("Ultra camera orientation needs a capture conversion adapter");
    if (defer_scene_fence)
      throw std::invalid_argument("Ultra camera currently requires synchronous GPU completion");
  }
  std::string native_camera_control_file, native_camera_lease_file;
  for (auto* key : {"nativeCameraControlFile", "nativeCameraLeaseFile"}) {
    if (auto entry = fields.find(key); entry != fields.end()) {
      (std::string_view(key) == "nativeCameraControlFile" ? native_camera_control_file
                                                          : native_camera_lease_file) =
          entry->second;
      fields.erase(entry);
    }
  }
  if (native_camera_control_file.empty() != native_camera_lease_file.empty())
    throw std::invalid_argument("native camera handoff needs both control and surface lease files");
  if (!native_camera_control_file.empty()) {
    if (native_camera_control_file == native_camera_lease_file)
      throw std::invalid_argument("native camera control and surface lease must differ");
    if (camera_configuration && camera_configuration->enabled)
      throw std::invalid_argument(
          "select native camera handoff or the signed camera input, not both");
    (void)camera::ActivationControl(native_camera_control_file);
    (void)camera::ActivationControl(native_camera_lease_file);
    // Distinct strings may name the same missing endpoint through an ancestor
    // alias. Do not let an atomic request replacement replace the lease inode.
    const std::filesystem::path request(native_camera_control_file),
        lease(native_camera_lease_file);
    struct stat request_parent{}, lease_parent{};
    if (::stat(request.parent_path().c_str(), &request_parent) ||
        ::stat(lease.parent_path().c_str(), &lease_parent))
      throw std::invalid_argument("native camera endpoint parent unavailable");
    if (request.filename() == lease.filename() && request_parent.st_dev == lease_parent.st_dev &&
        request_parent.st_ino == lease_parent.st_ino)
      throw std::invalid_argument("native camera request aliases the surface lease");
    if (read_only)
      camera::CameraSurfaceLease::validate(native_camera_lease_file);
    else
      (void)camera::CameraSurfaceLease(native_camera_lease_file);
  }
  std::string native_page_control_file, native_page_lease_file;
  for (auto* key : {"nativePageControlFile", "nativePageLeaseFile"}) {
    if (auto entry = fields.find(key); entry != fields.end()) {
      (std::string_view(key) == "nativePageControlFile" ? native_page_control_file
                                                        : native_page_lease_file) = entry->second;
      fields.erase(entry);
    }
  }
  if (native_page_control_file.empty() != native_page_lease_file.empty())
    throw std::invalid_argument("native page handoff needs both control and surface lease files");
  if (!native_page_control_file.empty()) {
    if (!native_camera_control_file.empty())
      throw std::invalid_argument("native page and camera ownership cannot be combined");
    MainSurfaceControl::validate(native_page_control_file, native_page_lease_file);
  }
  if (fields.size() != keys.size()) throw std::invalid_argument("incomplete local Ultra profile");
  for (const auto* key : keys)
    if (!fields.contains(key)) throw std::invalid_argument("unknown local Ultra profile field");
  for (const auto* key : {"assetDirectory", "vehicleProfile"})
    if (!std::filesystem::path(fields.at(key)).is_absolute())
      throw std::invalid_argument("Ultra paths must be absolute");
  const auto& version = fields.at("assetVersion");
  std::uint64_t number = 0;
  const auto [end, error] =
      std::from_chars(version.data(), version.data() + version.size(), number);
  if (error != std::errc{} || end != version.data() + version.size() || !number)
    throw std::invalid_argument("invalid Ultra asset version");
  AssetBundlePolicy policy{fields.at("assetIdentifier"), number, acceptance_reference,
                           certificate_time_policy};
  const std::filesystem::path directory(fields.at("assetDirectory"));
  auto result = std::shared_ptr<InstalledUi>(new InstalledUi);
  result->diagnostics_ = diagnostics;
  result->diagnostic_reports_ = diagnostic_reports;
  result->script_log_lines_ = script_log_lines;
  result->overlap_script_tick_ = overlap_script_tick;
  result->defer_scene_fence_ = defer_scene_fence;
  result->opaque_windows_ = opaque_windows;
  result->isolate_cluster_output_faults_ = isolate_cluster_output_faults;
  result->reveal_cluster_underlay_while_waiting_ = reveal_cluster_underlay_while_waiting;
  result->presentation_queue_depths_ = presentation_queue_depths;
  result->feedback_poll_ms_ = feedback_poll_ms;
  result->video_presentation_advance_ms_ = video_presentation_advance_ms;
  result->simulate_gauges_ = simulate_gauges;
  result->first_pairing_animation_ = first_pairing_animation;
  result->brand_greeting_ = brand_greeting;
  result->greeting_sequence_file_ = std::move(greeting_sequence_file);
  result->vehicle_socket_ = std::move(vehicle_socket);
  result->cluster_startup_layout_ = std::move(cluster_startup_layout);
  result->drive_mode_profiles_file_ = std::move(drive_mode_profiles_file);
  result->cluster_layout_file_ = std::move(cluster_layout_file);
  result->display_response_file_ = std::move(display_response_file);
  result->startup_warning_lights_file_ = std::move(startup_warning_lights_file);
  result->persist_cluster_layout_ = persist_cluster_layout;
  result->warning_overlay_ = std::move(warning_overlay);
  result->bench_assistance_view_ = bench_assistance_view;
  result->camera_ = std::move(camera_configuration);
  result->camera_control_file_ = std::move(camera_control_file);
  result->camera_status_file_ = std::move(camera_status_file);
  result->native_camera_control_file_ = std::move(native_camera_control_file);
  result->native_camera_lease_file_ = std::move(native_camera_lease_file);
  result->native_page_control_file_ = std::move(native_page_control_file);
  result->native_page_lease_file_ = std::move(native_page_lease_file);
  result->wayland_displays_ = std::move(display_names);
  result->bundle_ = std::make_shared<AssetBundle>(AssetBundle::open(
      (directory / "archive.zip").string(), (directory / "signature.der").string(),
      (directory / "certificate.pem").string(), (directory / "trusted-root.pem").string(), policy));
  if (result->bundle_->info().expired_signer_exception)
    std::clog << "Ultra asset trust: expiredSigningLeafExceptionUsed=true; chain and archive "
                 "signature verified\n";
  result->local_profile_ = read_local(fields.at("vehicleProfile"), 65536);
  auto local_vehicle = vehicle::decode_local_profile(result->local_profile_);
  if (first_pairing_animation && !local_vehicle.scene_inputs)
    throw std::invalid_argument("first-pairing animation needs session scene inputs");
  if (brand_greeting && !local_vehicle.scene_inputs)
    throw std::invalid_argument("Linux Head Unit greeting needs session scene inputs");
  if (local_vehicle.scene_inputs || bench_assistance_view ||
      (result->camera_ && result->camera_->enabled)) {
    const auto& info = result->bundle_->info();
    if (bench_assistance_view && (info.identifier != "44C9FA31EC" || info.version != 985))
      throw std::invalid_argument(
          "bench assistance slot is qualified only for the regular reference pack");
    const bool inspected =
        (info.version == 985 &&
         (info.identifier == "44C9FA31EC" || info.identifier == "44C9FA31ECCN")) ||
        (info.version == 547 &&
         (info.identifier == "44C9FA31EC-Single" || info.identifier == "44C9FA31ECCN-Single"));
    if (!inspected)
      throw std::invalid_argument(
          "scene/camera dialect is not qualified for this asset identity/version");
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (result->camera_ && result->camera_->enabled)
      (void)vehicle::CameraVisibility(local_vehicle, result->camera_->stream_identifier,
                                      result->camera_->maximum_age_ms, now);
  }
  if (persist_cluster_layout) {
    const auto& info = result->bundle_->info();
    (void)LayoutControl(result->cluster_layout_file_,
                        info.identifier + ":" + std::to_string(info.version));
  }
  return result;
}
void InstalledUi::validate_content_request(
    const vehicle::ContentRequestCapability& capability) const {
  const auto profile = vehicle::decode_local_profile(local_profile_);
  auto state = std::make_shared<vehicle::ContentStateSnapshot>(capability);
  (void)vehicle::ContentLocalInput(profile, std::move(state));
}
UiSession::Factory InstalledUi::factory(
    std::string main_uuid, std::string cluster_uuid, std::shared_ptr<UiClockSnapshot> clock,
    std::shared_ptr<video::ReceiverVideo> ingress, std::shared_ptr<headunit::TouchMailbox> touches,
    ReceiverDisplays displays, std::shared_ptr<MainPresentationSnapshot> main_presentation,
    std::shared_ptr<vehicle::CameraStateSnapshot> camera_state,
    LayoutControl::Publisher layout_publisher, bool newly_paired,
    std::shared_ptr<vehicle::ContentStateSnapshot> content_state) const {
  if (!clock) throw std::invalid_argument("Ultra UI needs a connection clock");
  for (unsigned i = 0; i < 2; ++i)
    if (displays[i].wayland_display.empty()) displays[i].wayland_display = wayland_displays_[i];
  const auto& asset = bundle_->info();
  const auto drive_mode_scope = asset.identifier + ":" + std::to_string(asset.version);
  const auto preference_scope = persist_cluster_layout_
                                    ? asset.identifier + ":" + std::to_string(asset.version)
                                    : std::string{};
  auto preferences = preference_scope.empty()
                         ? std::shared_ptr<PreferenceFile>{}
                         : std::make_shared<PreferenceFile>(cluster_layout_file_ + ".confirmed." +
                                                            preference_scope);
  // This factory belongs to one real control connection. Service replacement
  // may recover rendering, but cannot replay its greeting or pairing option.
  auto launched = std::make_shared<std::atomic<bool>>(false);
  return [launched, first_pairing = newly_paired && first_pairing_animation_, displays,
          greeting_sequence_file = greeting_sequence_file_, brand_greeting = brand_greeting_,
          native_page_control_file = native_page_control_file_,
          native_page_lease_file = native_page_lease_file_,
          native_camera_control_file = native_camera_control_file_,
          native_camera_lease_file = native_camera_lease_file_, camera_configuration = camera_,
          camera_control_file = camera_control_file_, camera_status_file = camera_status_file_,
          bench_assistance_view = bench_assistance_view_, warning_overlay = warning_overlay_,
          cluster_startup_layout = cluster_startup_layout_,
          cluster_layout_file = cluster_layout_file_,
          drive_mode_profiles_file = drive_mode_profiles_file_, drive_mode_scope,
          display_response_file = display_response_file_,
          startup_warning_lights_file = startup_warning_lights_file_, preference_scope, preferences,
          simulate_gauges = simulate_gauges_, vehicle_socket = vehicle_socket_,
          diagnostics = diagnostics_, diagnostic_reports = diagnostic_reports_,
          script_log_lines = script_log_lines_, overlap_script_tick = overlap_script_tick_,
          defer_scene_fence = defer_scene_fence_, opaque_windows = opaque_windows_,
          isolate_cluster_output_faults = isolate_cluster_output_faults_,
          reveal_cluster_underlay_while_waiting = reveal_cluster_underlay_while_waiting_,
          presentation_queue_depths = presentation_queue_depths_,
          feedback_poll_ms = feedback_poll_ms_,
          video_presentation_advance_ms = video_presentation_advance_ms_, bundle = bundle_,
          profile = local_profile_, main_uuid = std::move(main_uuid),
          cluster_uuid = std::move(cluster_uuid), clock, ingress, touches, main_presentation,
          camera_state, content_state, layout_publisher] {
    const auto scene_launch = launched->exchange(true)
                                  ? SceneLifecycle::Launch::resume
                                  : (first_pairing ? SceneLifecycle::Launch::first_pairing
                                                   : SceneLifecycle::Launch::startup);
    const auto generation = touches ? touches->begin() : 0;
    const auto presentation_generation = main_presentation ? main_presentation->begin() : 0;
    const auto queues = ingress ? ingress->claim() : video::ReceiverVideo::Queues{};
    UiWorker::Configuration config;
    config.resources = cache_resources([bundle](std::string_view name) {
      auto b = bundle->resource(std::string(name));
      return std::string(b.begin(), b.end());
    });
    for (const auto& d : displays)
      config.displays.push_back({d.panel_name, d.size.width, d.size.height, 1.0 / d.size.max_fps});
    config.routes = {{main_uuid, displays[0].panel_name}, {cluster_uuid, displays[1].panel_name}};
    const auto worker_fps = std::max(displays[0].size.max_fps, displays[1].size.max_fps);
    config.frame_interval_ns = (1000000000ULL + worker_fps / 2) / worker_fps;
    config.feedback_poll_ns = std::uint64_t(feedback_poll_ms) * 1000000;
    if (diagnostics)
      config.command_diagnostic = [](const UiCommandDiagnostic& event) {
        // Private bench diagnostics: these boundaries precede encrypted transport
        // acceptance and say nothing about phone receipt or script acceptance.
        std::osyncstream log(std::cerr);
        log << "Ultra UI command panel=" << unsigned(event.panel) << " boundary="
            << (event.direction == UiCommandDiagnostic::Direction::incoming_vm_queue
                    ? "incoming_vm_queue"
                    : "outgoing_worker_queue")
            << " generation=" << event.generation << " count=" << event.count
            << " mono_ns=" << event.monotonic_ns
            << " typeShape=" << ui_field_shape_name(event.type_shape);
        if (event.command_type) log << " command=" << *event.command_type;
        log << " dataShape=" << ui_field_shape_name(event.data_shape);
        for (std::size_t i = 0; i < event.fields.size(); ++i)
          if (event.fields[i] != UiFieldShape::absent)
            log << ' ' << event.field_names[i] << "Shape=" << ui_field_shape_name(event.fields[i]);
        if (event.focus_token) log << " focusTokenPrivate=" << *event.focus_token;
        if (event.transitioning) log << " transitioning=" << *event.transitioning;
        log << " budgetExhausted=" << event.budget_exhausted << '\n';
      };
    if (diagnostics)
      config.frame_report = [](const UiWorker::FrameReport& r) {
        if (!r.turns || !r.duration_ns) return;
        std::osyncstream(std::cerr)
            << "Ultra worker frame diagnostic turns=" << r.turns
            << " ownerHz=" << r.turns * 1e9 / double(r.duration_ns)
            << " missedSlots=" << r.missed_slots
            << " ownerMeanMs=" << r.owner_ns / double(r.turns) / 1e6
            << " ownerMaxMs=" << r.max_owner_ns / 1e6
            << " mainDisplayMeanMs=" << r.presentation_ns[0] / double(r.turns) / 1e6
            << " mainScriptMeanMs=" << r.task_ns[0] / double(r.turns) / 1e6
            << " clusterDisplayMeanMs=" << r.presentation_ns[1] / double(r.turns) / 1e6
            << " clusterScriptMeanMs=" << r.task_ns[1] / double(r.turns) / 1e6 << '\n';
      };
    config.limits.startup_ms = 10000;
    struct Outputs {
      std::shared_ptr<MainPresentationSnapshot> main_presentation;
      std::uint64_t presentation_generation = 0;
      std::shared_ptr<vehicle::CameraStateSnapshot> camera_state;
      std::uint64_t camera_generation = 0;
      ~Outputs() {
        if (camera_status) {
          try {
            camera_status->publish({camera_started_ns, ++camera_status_sequence,
                                    presentation_monotonic_ns(), camera::ControlPhase::stopped});
          } catch (...) {
            // A failed publisher expires; shutdown still retires all input owners.
          }
        }
        assistance_provider.reset();  // Stop CPU publication before GPU streams close.
        if (scene) scene->retire(presentation_monotonic_ns() / 1000000);
        if (camera_state) camera_state->retire(camera_generation, presentation_monotonic_ns());
        if (main_presentation) main_presentation->retire(presentation_generation);
      }
      std::unique_ptr<SceneLifecycle> scene;
      std::unique_ptr<vehicle::GaugeInput> gauges;
      std::unique_ptr<vehicle::ContentLocalInput> content;
      std::unique_ptr<graphics::WarningOverlay> warnings;
      std::unique_ptr<LayoutControl> layout_control;
      std::unique_ptr<DriveModeControl> drive_modes;
      std::unique_ptr<DisplayResponseFile> display_response;
      std::unique_ptr<camera::ActivationControl> camera_control;
      std::unique_ptr<camera::StatusPublisher> camera_status;
      std::uint64_t camera_started_ns = 0, camera_status_sequence = 0, camera_status_last_ns = 0;
      std::shared_ptr<camera::Queue> camera_queue;
      std::shared_ptr<vehicle::CameraVisibility> camera_visibility;
      std::unique_ptr<camera::SceneBuilder> camera_scene;
      std::shared_ptr<video::CameraInput> camera_input;
      // Destruction order: producer retires before its queue; GPU inputs retire
      // before both. This source never opens a car or camera device.
      std::shared_ptr<camera::Queue> assistance_queue;
      std::unique_ptr<vehicle::VehicleViewProvider> assistance_provider;
      std::uint64_t assistance_sequence = 0, assistance_last_ns = 0;
      bool camera_requested = false;
      bool camera_dismissed = false;
      std::unique_ptr<camera::ActivationControl> native_camera_control;
      std::unique_ptr<camera::CameraSurfaceLease> native_camera_lease;
      std::unique_ptr<MainSurfaceControl> native_page_control;
      std::array<std::unique_ptr<graphics::GpuDisplay>, 2> displays;
      std::array<std::shared_ptr<video::NativeInput>, 2> inputs;
      const std::uint64_t report_owner_started_ns = presentation_monotonic_ns();
      std::array<DisplayReportSeries, 2> report_series;
      std::array<unsigned, 2> suspension_events{};
      std::array<std::uint64_t, 2> last_suspensions{};
      std::array<graphics::GpuDisplayStats, 2> last_stats{};
      std::array<video::NativeInputStats, 2> last_video_stats{};
      bool touch_down = false;
      std::array<DisplayDeadlineWindow, 2> deadlines{};
    };
    auto outputs = std::make_shared<Outputs>();
    outputs->main_presentation = main_presentation;
    outputs->presentation_generation = presentation_generation;
    outputs->camera_state = camera_state;
    if (camera_state) outputs->camera_generation = camera_state->begin(presentation_monotonic_ns());
    config.configure = [outputs, profile, displays, clock, queues, content_state,
                        native_page_control_file, native_page_lease_file,
                        native_camera_control_file, native_camera_lease_file, camera_configuration,
                        camera_control_file, camera_status_file, warning_overlay, brand_greeting,
                        greeting_sequence_file, bench_assistance_view, cluster_startup_layout,
                        cluster_layout_file, display_response_file, startup_warning_lights_file,
                        preference_scope, drive_mode_profiles_file, drive_mode_scope, preferences,
                        simulate_gauges, vehicle_socket, overlap_script_tick, defer_scene_fence,
                        opaque_windows, isolate_cluster_output_faults,
                        reveal_cluster_underlay_while_waiting, presentation_queue_depths,
                        video_presentation_advance_ms, diagnostics, diagnostic_reports,
                        script_log_lines, touches, generation, layout_publisher,
                        scene_launch](Channel& channel) {
      auto vehicle = vehicle::decode_local_profile(profile);
      if (content_state)
        outputs->content = std::make_unique<vehicle::ContentLocalInput>(
            vehicle, content_state, presentation_monotonic_ns() / 1000000);
      if (vehicle.scene_inputs) {
        outputs->scene = std::make_unique<SceneLifecycle>(vehicle, scene_launch);
        outputs->scene->pump(presentation_monotonic_ns() / 1000000,
                             Runtime::ScenePhase::unavailable, false);
      }
      std::unique_ptr<headunit::VehicleSource> source;
      if (simulate_gauges) {
        source = std::make_unique<headunit::SimulatedVehicle>(headunit::Clock::now());
        std::osyncstream(std::cerr) << "Ultra gauge source=simulation; bench values only\n";
      } else if (!vehicle_socket.empty())
        source = std::make_unique<headunit::SocketVehicle>(vehicle_socket);
      if (source)
        outputs->gauges = std::make_unique<vehicle::GaugeInput>(vehicle, std::move(source));
      if (warning_overlay && warning_overlay->enabled)
        outputs->warnings = std::make_unique<graphics::WarningOverlay>(*warning_overlay);
      if (!native_page_control_file.empty())
        outputs->native_page_control =
            std::make_unique<MainSurfaceControl>(native_page_control_file, native_page_lease_file);
      if (!native_camera_control_file.empty()) {
        outputs->native_camera_control =
            std::make_unique<camera::ActivationControl>(native_camera_control_file);
        outputs->native_camera_lease =
            std::make_unique<camera::CameraSurfaceLease>(native_camera_lease_file);
      }
      if (camera_configuration && camera_configuration->enabled) {
        const auto& c = *camera_configuration;
        const auto canvas_width =
            displays[0].asset_width ? displays[0].asset_width : displays[0].size.width;
        const auto canvas_height =
            displays[0].asset_height ? displays[0].asset_height : displays[0].size.height;
        if (displays[0].panel_name != "Center_Display" || canvas_width != c.canvas_width ||
            canvas_height != c.canvas_height)
          throw std::invalid_argument(
              "camera destination must match the inspected Center_Display canvas");
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        outputs->camera_control = std::make_unique<camera::ActivationControl>(camera_control_file);
        if (!camera_status_file.empty()) {
          outputs->camera_started_ns = now;
          outputs->camera_status = std::make_unique<camera::StatusPublisher>(camera_status_file);
        }
        outputs->camera_queue = std::make_shared<camera::Queue>(c);
        outputs->camera_scene = std::make_unique<camera::SceneBuilder>(c);
        outputs->camera_visibility = std::make_shared<vehicle::CameraVisibility>(
            vehicle, c.stream_identifier, c.maximum_age_ms, now);
      }
      for (std::size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        auto& runtime = channel.runtime(d.panel_name);
        runtime.bind_vehicle(
            vehicle.source, vehicle.bindings, {},
            outputs->content ? outputs->content->dismissals() : vehicle::ScriptView::Dismissals{});
        if (i == 1 && (outputs->scene || outputs->warnings || outputs->content || brand_greeting))
          runtime.enable_scene_observation();
        if (diagnostics)
          runtime.bind_session_diagnostic_sink(
              [panel = i](const SessionDiagnostic& event) {
                std::osyncstream(std::cerr)
                    << "Ultra UI outcome panel=" << panel
                    << " kind=" << session_diagnostic_name(event.kind) << " count=" << event.count
                    << " mono_ns=" << event.monotonic_ns << " connected=" << event.connected
                    << " foregrounded=" << event.foregrounded << '\n';
              },
              diagnostic_reports);
        if (diagnostics) bind_bounded_script_log(runtime, i, script_log_lines);
        if (i == 0 && !display_response_file.empty())
          outputs->display_response = std::make_unique<DisplayResponseFile>(display_response_file);
        if (i == 1) {
          // Leave an unspecified remote component to the phone. Forcing Map
          // into every layout request overrides its own centre-content choice.
          // Explicit native Map selections still use the verified catalogue.
          if (!cluster_layout_file.empty() || !drive_mode_profiles_file.empty() ||
              bench_assistance_view)
            runtime.enable_live_layout_selection();
          if (!drive_mode_profiles_file.empty())
            outputs->drive_modes = std::make_unique<DriveModeControl>(
                drive_mode_profiles_file, drive_mode_scope, layout_publisher);
          if (!cluster_layout_file.empty()) {
            outputs->layout_control = std::make_unique<LayoutControl>(
                cluster_layout_file, preference_scope, layout_publisher, preferences);
          }
          if (!cluster_startup_layout.empty()) {
            runtime.select_startup_layout(cluster_startup_layout);
            std::osyncstream(std::cerr)
                << "Ultra cluster startup layout=" << cluster_startup_layout << '\n';
          }
        }
        graphics::GpuDisplayOptions options;
        if (i == 0 && outputs->native_camera_control) {
          options.surface_lease = graphics::GpuDisplayOptions::SurfaceLease{
              [owner = outputs.get()] {
                return !owner->native_camera_control->poll() &&
                       owner->native_camera_lease->acquire_shared();
              },
              [owner = outputs.get()] {
                owner->native_camera_lease->release();
              }};
        }
        if (i == 0 && outputs->native_page_control) {
          options.surface_lease = graphics::GpuDisplayOptions::SurfaceLease{
              [owner = outputs.get()] { return owner->native_page_control->renderer_acquire(); },
              [owner = outputs.get()] {
                owner->native_page_control->renderer_release();
              }};
        }
        options.asset_canvas = {double(d.asset_width), double(d.asset_height)};
        if (i == 1 && brand_greeting) {
          options.decorate_scene = [runtime = &runtime,
                                    greeting = std::make_shared<graphics::BrandGreeting>()](
                                       const std::shared_ptr<const graphics::Layer>& root) {
            return greeting->scene(root, runtime->scene_phase() == Runtime::ScenePhase::logos,
                                   presentation_monotonic_ns());
          };
        }
        if (i == 1 && !greeting_sequence_file.empty()) {
          auto signed_brand = options.decorate_scene;
          options.decorate_scene = [signed_brand, sequence = std::make_shared<GreetingSequenceFile>(
                                                      greeting_sequence_file)](
                                       const std::shared_ptr<const graphics::Layer>& root) {
            const auto scene = signed_brand ? signed_brand(root) : root;
            // Connected sessions never claim a boot greeting. Explicit dated
            // cosmetic events stay on this existing VM/GPU owner.
            return graphics::with_scene_overlay(
                scene,
                graphics::greeting_overlay(sequence->frame(headunit::Clock::now()), {1920, 720}));
          };
        }
        if (i == 1 && outputs->warnings) {
          // Displays are destroyed before these owner-thread objects. Borrow
          // pointers so retained options cannot form a shared ownership cycle.
          auto preference =
              startup_warning_lights_file.empty()
                  ? std::shared_ptr<StartupWarningLightsFile>{}
                  : std::make_shared<StartupWarningLightsFile>(startup_warning_lights_file);
          options.overlay = [warnings = outputs->warnings.get(), gauges = outputs->gauges.get(),
                             runtime = &runtime, preference](graphics::Size size) {
            const auto now = headunit::Clock::now();
            if (preference)
              if (const auto hide = preference->poll(now)) warnings->set_hide_during_startup(*hide);
            const auto phase = runtime->scene_phase();
            // No observed scene yet is also clean. Vehicle acquisition and its
            // original timestamp continue unchanged throughout the animation.
            const bool startup = phase == Runtime::ScenePhase::unavailable ||
                                 phase == Runtime::ScenePhase::startup ||
                                 phase == Runtime::ScenePhase::logos;
            return warnings->scene(gauges->sample(), now, size, startup);
          };
        }
        options.overlap_script_tick = overlap_script_tick;
        options.window.defer_scene_fence = defer_scene_fence;
        options.window.opaque_background = opaque_windows;
        options.isolate_output_faults = i == 1 && isolate_cluster_output_faults;
        options.reveal_underlay_while_waiting = i == 1 && reveal_cluster_underlay_while_waiting;
        if (options.isolate_output_faults)
          options.isolated_output_fault = [panel = i](std::string_view reason) {
            const auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
            std::osyncstream(std::cerr) << "Ultra output isolated panel=" << panel
                                        << " mono_ns=" << mono_ns << " reason=" << reason << '\n';
          };
        options.window.max_pending_presentations = presentation_queue_depths[i];
        if (diagnostics) {
          // Display callbacks run only while Outputs owns the display. Borrow
          // the window to avoid a shared_ptr cycle through display options.
          auto* window = &outputs->deadlines[i];
          options.slow_frame_threshold_ns = 16000000;
          options.slow_frame = [window](const auto& trace) {
            window->observe(trace);
          };
          options.presentation_gap_threshold_ns = 20000000;
          options.presentation_gap = [window](const auto& trace) {
            window->observe(trace);
          };
        }
        if (queues[i]) {
          auto stream = runtime.add_video_input({"iPhoneVideo", d.size.width, d.size.height});
          outputs->inputs[i] = std::make_shared<video::NativeInput>(
              stream, queues[i], std::string{},
              std::uint64_t(video_presentation_advance_ms) * 1000000);
          options.inputs.push_back(outputs->inputs[i]);
        }
        if (i == 1 && bench_assistance_view) {
          camera::Configuration c;
          c.enabled = true;
          c.source = camera::SourceKind::external;
          c.stream_identifier = "adasView";
          c.width = c.canvas_width = 600;
          c.height = c.canvas_height = 480;
          c.destination = {0, 0, 600, 480};
          outputs->assistance_queue = std::make_shared<camera::Queue>(c);
          outputs->assistance_provider = std::make_unique<vehicle::VehicleViewProvider>(
              *outputs->assistance_queue, false, c.maximum_age_ms);
          auto stream =
              runtime.add_video_input({"adasView", 600, 480, 1. / 30, video::InputType::widget});
          auto input = std::make_shared<video::LocalInput>(
              c, stream, outputs->assistance_queue, video::LocalInput::TimeSource{},
              video::LocalInput::VisibilitySink{}, video::InputType::widget);
          input->set_active(true);
          options.inputs.push_back(std::move(input));
        }
        if (i == 0 && outputs->camera_queue) {
          const auto& c = *camera_configuration;
          auto stream = runtime.add_video_input({c.stream_identifier, c.width, c.height,
                                                 1. / c.frame_rate, video::InputType::camera});
          outputs->camera_input = std::make_shared<video::CameraInput>(
              c, stream, std::move(outputs->camera_queue), video::CameraInput::TimeSource{},
              [visibility = outputs->camera_visibility, state = outputs->camera_state,
               camera_generation = outputs->camera_generation](bool on, std::uint64_t capture,
                                                               std::uint64_t observed) {
                if (on)
                  (void)visibility->refresh(true, capture, observed);
                else
                  visibility->withdraw(observed);
                if (state)
                  (void)state->observe(camera_generation, visibility->visible(), capture, observed);
              });
          options.inputs.push_back(outputs->camera_input);
          if (outputs->camera_state) {
            options.input_withdrawal = graphics::GpuDisplayOptions::InputWithdrawal{
                options.inputs.size() - 1,
                [state = outputs->camera_state, g = outputs->camera_generation](std::uint64_t at) {
                  return state->off_for_composition(g, at);
                },
                [state = outputs->camera_state,
                 g = outputs->camera_generation](const graphics::InputWithdrawalReceipt& r) {
                  (void)state->presented_off(g, r.sequence, r.submission_epoch, r.current_epoch,
                                             r.draw, r.submitted_ns, r.presented_ns, r.observed_ns);
                }};
          }
          options.overlay = [owner = outputs.get(),
                             simulated = c.source == camera::SourceKind::synthetic_test_pattern](
                                graphics::Size size) {
            return owner->camera_scene->status_overlay(
                owner->camera_requested, owner->camera_input->availability(), simulated, size);
          };
        }
        options.window.title = std::string("Independent Ultra: ") + d.panel_name;
        options.window.display_name = d.wayland_display;
        options.window.output_name = d.output_name;
        options.window.app_id = i ? "org.headunit.ultra.cluster" : "org.headunit.ultra.main";
        options.frame_interval_ns = (1000000000ULL + d.size.max_fps / 2) / d.size.max_fps;
        if (i == 1) options.scene_stall_timeout_ns = 8000000000ULL;
        if (i == 0 && touches) {
          options.window.accept_touch = true;
          options.window.touch_diagnostics = diagnostics;
          options.touches = [touches, generation,
                             owner = outputs.get()](std::span<const headunit::TouchInput> reports) {
            // Exterior-camera phone control IDs are not yet verified. Do not
            // route a camera-screen touch into the hidden CarPlay app.
            if (owner->camera_requested || owner->camera_dismissed) {
              touches->cancel(generation);
              owner->touch_down = false;
              return;
            }
            touches->push(generation, reports);
            // Time each new finger-down against the phone's next video frame.
            for (const auto& report : reports) {
              if (report.pressed && !owner->touch_down && owner->inputs[0]) {
                const auto at = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
                owner->inputs[0]->touch_pressed(std::uint64_t(at));
              }
              owner->touch_down = report.pressed;
            }
          };
          options.cancel_touches = [touches, generation] {
            touches->cancel(generation);
          };
        }
        outputs->displays[i] = std::make_unique<graphics::GpuDisplay>(
            runtime, Display{d.panel_name, d.size.width, d.size.height, 1.0 / d.size.max_fps},
            [clock] { return clock->sample(); }, options);
      }
    };
    config.presentation = [outputs, diagnostics, diagnostic_reports, displays, layout_publisher](
                              Runtime& runtime, const Display& display,
                              UiWorker::DisplayPhase phase) {
      const unsigned i = display.identifier == displays[0].panel_name ? 0 : 1;
      if (i == 1 && outputs->content && phase == UiWorker::DisplayPhase::frame)
        outputs->content->pump(presentation_monotonic_ns() / 1000000,
                               runtime.scene_phase() == Runtime::ScenePhase::drive);
      if (i == 1 && outputs->assistance_provider && phase == UiWorker::DisplayPhase::frame) {
        const auto at = presentation_monotonic_ns();
        if (at > outputs->assistance_last_ns && at - outputs->assistance_last_ns >= 33333333) {
          outputs->assistance_last_ns = at;
          vehicle::ViewObservation observation;
          observation.generation = 1;  // Per-factory explicit simulation owner.
          observation.sequence = ++outputs->assistance_sequence;
          observation.acquired_ns = at;
          observation.provenance = vehicle::ViewProvenance::simulated;
          observation.content = vehicle::LaneView{-1.75, 1.75, false};
          (void)outputs->assistance_provider->submit(std::move(observation), at);
        }
      }
      if (i == 0 && outputs->camera_control) {
        const auto at = presentation_monotonic_ns();
        const auto activation = outputs->camera_control->read(at);
        if (activation == camera::ActivationRequest::off) outputs->camera_dismissed = false;
        if (outputs->camera_state) {
          if (const auto request =
                  outputs->camera_state->pending_off(outputs->camera_generation, at)) {
            // Apply on this existing owner, then date the completed withdrawal.
            // Missing/stale visibility is never a substitute for this action.
            outputs->camera_dismissed = true;
            outputs->camera_requested = false;
            outputs->camera_input->set_active(false);
            (void)outputs->camera_state->owner_off(outputs->camera_generation, request->sequence,
                                                   presentation_monotonic_ns());
          }
        }
        const bool requested =
            activation == camera::ActivationRequest::on && !outputs->camera_dismissed;
        if (requested != outputs->camera_requested) {
          if (requested && outputs->camera_state)
            outputs->camera_state->cancel_off(outputs->camera_generation);
          outputs->camera_requested = requested;
          outputs->camera_input->set_active(requested);
        }
      }
      if (i == 0 && outputs->camera_status && phase == UiWorker::DisplayPhase::frame) {
        const auto at = presentation_monotonic_ns();
        if (at > outputs->camera_status_last_ns &&
            at - outputs->camera_status_last_ns >= 100000000) {
          // Owner eligibility heartbeat only. It never substitutes for acquisition
          // dates or the separate camera withdrawal/presentation receipts.
          const auto phase = camera::source_control_phase(outputs->camera_requested,
                                                          outputs->camera_input->availability());
          outputs->camera_status->publish(
              {outputs->camera_started_ns, ++outputs->camera_status_sequence, at, phase});
          outputs->camera_status_last_ns = at;
        }
      }
      if (i == 0 && outputs->display_response && phase == UiWorker::DisplayPhase::frame) {
        // The native settings page chooses a bounded presentation policy. It
        // applies live: later staging uses the new advance and the main window
        // honours the new queue bound once pending presentations drain.
        if (const auto mode = outputs->display_response->poll(std::chrono::steady_clock::now())) {
          const auto policy = display_response_policy(*mode);
          for (const auto& input : outputs->inputs)
            if (input) input->set_presentation_advance(policy.presentation_advance_ns);
          outputs->displays[0]->set_presentation_queue_depth(policy.main_queue_depth);
          std::osyncstream(std::cerr)
              << "Ultra display response mode=" << display_response_name(*mode)
              << " advanceMs=" << policy.presentation_advance_ns / 1000000
              << " mainQueueDepth=" << policy.main_queue_depth << '\n';
        }
      }
      if (i == 1 && outputs->layout_control && phase == UiWorker::DisplayPhase::frame)
        outputs->layout_control->poll_cluster(
            [&runtime](const auto& request) { return runtime.cluster_selection(request); },
            std::chrono::steady_clock::now());
      if (outputs->gauges) outputs->gauges->pump(headunit::Clock::now());
      if (i == 1 && outputs->drive_modes && phase == UiWorker::DisplayPhase::frame)
        outputs->drive_modes->poll(
            outputs->gauges ? outputs->gauges->sample() : VehicleSample{}, headunit::Clock::now(),
            [&runtime](const auto& request) { return runtime.cluster_selection(request); });
      const auto turn = [&] {
        try {
          return outputs->displays[i]->turn(phase);
        } catch (const std::exception& e) {
          if (i == 0 && outputs->main_presentation)
            outputs->main_presentation->retire(outputs->presentation_generation);
          const auto failed = outputs->displays[i]->stats();
          const auto mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
          std::osyncstream(std::cerr)
              << "Ultra display fault panel=" << i << " mono_ns=" << mono_ns
              << " scenes=" << failed.scene_presentations
              << " compositions=" << failed.scene_compositions
              << " suspensions=" << failed.suspensions
              << " clockEpoch=" << failed.clock_epoch_suspensions
              << " clockUnavailable=" << failed.clock_unavailable_suspensions
              << " ownerGap=" << failed.owner_gap_suspensions
              << " feedbackDelay=" << failed.feedback_delay_suspensions
              << " ignoredReceipts=" << failed.ignored_receipts
              << " composeMaxMs=" << failed.max_compose_ns / 1e6 << " reason=" << e.what() << '\n';
          throw std::runtime_error(display.identifier + ": " + e.what());
        }
      }();
      const auto stats = outputs->displays[i]->stats();
      if (i == 1 && outputs->scene && phase != UiWorker::DisplayPhase::feedback)
        outputs->scene->pump(presentation_monotonic_ns() / 1000000, runtime.scene_phase(),
                             stats.scene_visible && turn.pts.has_value() && !stats.faulted &&
                                 !stats.output_isolated);
      if (i == 0 && outputs->main_presentation) {
        const bool home = outputs->native_page_control &&
                          outputs->native_page_control->native_requested() &&
                          stats.surface_withdrawn;
        // turn.pts is retained only while the actual phone clock/epoch remains
        // valid. It is never invented from this publication's host clock.
        outputs->main_presentation->observe(
            outputs->presentation_generation, stats.scene_visible, home,
            turn.pts.has_value() && !stats.faulted && !stats.output_isolated,
            stats.surface_restorations);
      }
      const auto now = std::chrono::steady_clock::now();
      if (diagnostics && stats.suspensions != outputs->last_suspensions[i]) {
        outputs->last_suspensions[i] = stats.suspensions;
        if (outputs->suspension_events[i]++ < 16) {
          const auto mono_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
          std::osyncstream(std::cerr)
              << "Ultra display suspension panel=" << i << " mono_ns=" << mono_ns
              << " total=" << stats.suspensions << " clockEpoch=" << stats.clock_epoch_suspensions
              << " clockUnavailable=" << stats.clock_unavailable_suspensions
              << " ownerGap=" << stats.owner_gap_suspensions
              << " feedbackDelay=" << stats.feedback_delay_suspensions
              << " scenes=" << stats.scene_presentations << '\n';
        }
      }
      const auto report = diagnostics ? outputs->report_series[i].observe(
                                            now, stats.scene_presentations, diagnostic_reports)
                                      : std::nullopt;
      if (report) {
        const bool first = !report->previous;
        std::osyncstream log(std::cerr);
        const auto report_seconds =
            report->previous ? std::chrono::duration<double>(now - *report->previous).count() : 0;
        report->write_identity(log, outputs->report_owner_started_ns, layout_publisher.nonce,
                               layout_publisher.connection_generation, i);
        log << " scenes=" << stats.scene_presentations
            << " surfaceWithdrawn=" << stats.surface_withdrawn
            << " surfaceWithdrawals=" << stats.surface_withdrawals
            << " surfaceRestorations=" << stats.surface_restorations
            << " suspensions=" << stats.suspensions
            << " clockEpoch=" << stats.clock_epoch_suspensions
            << " clockUnavailable=" << stats.clock_unavailable_suspensions
            << " ownerGap=" << stats.owner_gap_suspensions
            << " feedbackDelay=" << stats.feedback_delay_suspensions << " waiting=" << stats.waiting
            << " sceneVisible=" << stats.scene_visible
            << " underlaySubmissions=" << stats.underlay_submissions
            << " zeroCopyScenes=" << stats.scene_zero_copy_presentations
            << " maxGpuQueued=" << stats.cache.max_deferred_gpu_frames;
        if (!first) {
          const auto& previous = outputs->last_stats[i];
          const auto seconds = report_seconds;
          const auto composed = stats.scene_compositions - previous.scene_compositions;
          const auto submitted = stats.scene_submissions - previous.scene_submissions;
          const auto receipts = stats.feedback_receipts - previous.feedback_receipts;
          const auto timed_scenes =
              stats.measured_scene_receipts - previous.measured_scene_receipts;
          log << " fps=" << (stats.scene_presentations - previous.scene_presentations) / seconds
              << " composeMeanMs="
              << (composed ? (stats.compose_ns - previous.compose_ns) / double(composed) / 1e6 : 0)
              << " submitMeanMs="
              << (submitted ? (stats.submit_ns - previous.submit_ns) / double(submitted) / 1e6 : 0)
              << " pendingTurns=" << (stats.pending_turns - previous.pending_turns)
              << " feedbackMeanMs="
              << (receipts ? (stats.feedback_ns - previous.feedback_ns) / double(receipts) / 1e6
                           : 0)
              << " feedbackMaxMs=" << stats.max_feedback_ns / 1e6
              << " refreshMs=" << stats.last_refresh_ns / 1e6 << " submitToPresentMeanMs="
              << (timed_scenes ? (stats.present_latency_ns - previous.present_latency_ns) /
                                     double(timed_scenes) / 1e6
                               : 0)
              << " presentToPollMeanMs="
              << (timed_scenes ? (stats.presentation_poll_ns - previous.presentation_poll_ns) /
                                     double(timed_scenes) / 1e6
                               : 0)
              << " preparationMeanMs="
              << (composed
                      ? (stats.preparation_ns - previous.preparation_ns) / double(composed) / 1e6
                      : 0)
              << " importMeanMs="
              << (composed ? (stats.import_ns - previous.import_ns) / double(composed) / 1e6 : 0)
              << " traversalMeanMs="
              << (composed ? (stats.traversal_ns - previous.traversal_ns) / double(composed) / 1e6
                           : 0)
              << " flushMeanMs="
              << (composed ? (stats.flush_ns - previous.flush_ns) / double(composed) / 1e6 : 0)
              << " validationMeanMs="
              << (composed ? (stats.validation_ns - previous.validation_ns) / double(composed) / 1e6
                           : 0)
              << " completionWaitMeanMs="
              << (composed ? (stats.completion_wait_ns - previous.completion_wait_ns) /
                                 double(composed) / 1e6
                           : 0)
              << " overlappedWorkMeanMs="
              << (composed ? (stats.overlap_ns - previous.overlap_ns) / double(composed) / 1e6 : 0)
              << " cacheBuilds=" << (stats.cache.builds - previous.cache.builds)
              << " cacheHits=" << (stats.cache.hits - previous.cache.hits)
              << " imageBuilds=" << (stats.cache.image_builds - previous.cache.image_builds);
        }
        log << " composeMaxMs=" << stats.max_compose_ns / 1e6
            << " submitMaxMs=" << stats.max_submit_ns / 1e6;
        outputs->last_stats[i] = stats;
        if (const auto input = outputs->inputs[i]) {
          const auto identity = input->identity();
          log << " videoIdentityObservedMonoNs=" << identity.observed_ns
              << " videoOwnerGeneration=" << identity.owner_generation
              << " videoIngressGeneration=" << identity.ingress_generation
              << " videoOwnerConnectionId=";
          if (identity.owner_connection_id)
            log << *identity.owner_connection_id;
          else
            log << "null";
          log << " videoIngressConnectionId=";
          if (identity.ingress_connection_id)
            log << *identity.ingress_connection_id;
          else
            log << "null";
          log << " videoAvailable=" << identity.available << " videoClosed=" << identity.closed
              << " videoOwnerCurrent=" << identity.owner_current;
          video::write_input_presentation(log, input->presentation());
          const auto v = input->stats();
          const auto& previous = outputs->last_video_stats[i];
          log << " decoded=" << v.decoded << " late=" << v.late
              << " clockDropped=" << v.clock_dropped << " videoPresented=" << v.presented
              << " maxStaged=" << v.max_staged;
          if (!first)
            log << " freshVideoFps=" << (v.presented - previous.presented) / report_seconds
                << " videoSubmittedFps=" << (v.submitted - previous.submitted) / report_seconds;
          // Window mean of decoded-frame lead; extremes are since connection start.
          const auto lead_samples = v.lead_samples - previous.lead_samples;
          const auto lead_sum_ns = v.lead_sum_ns - previous.lead_sum_ns;
          log << " videoLeadMeanMs="
              << (lead_samples ? double(lead_sum_ns) / double(lead_samples) / 1e6 : 0)
              << " videoLeadMinMs=" << v.min_lead_ns / 1e6
              << " videoLeadMaxMs=" << v.max_lead_ns / 1e6;
          // Window counts of decoded frames that never reached the panel:
          // superseded by a newer due frame before staging, or staged and then
          // discarded by a cancelled scene or script flush.
          log << " videoSubmittedWindow=" << v.submitted - previous.submitted
              << " videoDecodedWindow=" << v.decoded - previous.decoded
              << " videoSupersededWindow=" << v.superseded - previous.superseded
              << " videoDiscardedWindow=" << v.discarded - previous.discarded
              << " videoLateWindow=" << v.late - previous.late
              << " videoClockDroppedWindow=" << v.clock_dropped - previous.clock_dropped
              << " videoNonMonotonicWindow=" << v.non_monotonic - previous.non_monotonic
              << " videoBackpressureWindow=" << v.backpressure - previous.backpressure
              << " videoGenerationChangesWindow="
              << v.generation_changes - previous.generation_changes
              << " videoGenerationDroppedWindow="
              << v.generation_dropped - previous.generation_dropped;
          if (i == 0) {
            const auto& t = v.touch_response;
            log << " touchResponses=" << t.responses << " touchUnanswered=" << t.unanswered
                << " touchSkippedBusy=" << t.skipped_busy << " touchToDecodeMeanMs="
                << (t.responses ? double(t.decode_sum_ns) / double(t.responses) / 1e6 : 0)
                << " touchToDecodeMaxMs=" << t.decode_max_ns / 1e6 << " touchToPresentMeanMs="
                << (t.responses ? double(t.present_sum_ns) / double(t.responses) / 1e6 : 0)
                << " touchToPresentMaxMs=" << t.present_max_ns / 1e6;
          }
          outputs->last_video_stats[i] = v;
        } else {
          log << " videoIdentityObservedMonoNs=null videoOwnerGeneration=null "
                 "videoIngressGeneration=null"
              << " videoOwnerConnectionId=null videoIngressConnectionId=null"
              << " videoAvailable=null videoClosed=null videoOwnerCurrent=null "
                 "videoPresentation=null";
        }
        if (i == 0 && outputs->camera_input) {
          const auto camera = outputs->camera_input->statistics();
          log << " cameraRequested=" << outputs->camera_requested
              << " cameraVisible=" << outputs->camera_visibility->visible()
              << " cameraStaged=" << camera.staged << " cameraPresented=" << camera.presented
              << " cameraDiscarded=" << camera.discarded << " cameraUnmapped=" << camera.unmapped
              << " cameraNonMonotonic=" << camera.non_monotonic
              << " cameraExpired=" << camera.expired << " cameraConsumed=";
          if (const auto consumed = outputs->camera_input->consumption())
            consumed->write_json(log);
          else
            log << "null";
        }
        log << '\n';
        outputs->deadlines[i].write(log, i);
        outputs->deadlines[i] = {};
        // Numeric scene metadata only; never asset source, image pixels or phone data.
        if (first) {
          unsigned visited = 0, reported = 0;
          const auto walk = [&](auto&& self, const std::shared_ptr<const graphics::Layer>& layer,
                                unsigned depth) -> void {
            if (!layer || depth > 64 || ++visited > 4096 || layer->hidden || layer->opacity == 0)
              return;
            if (reported < 24 &&
                (layer->kind == graphics::Kind::video ||
                 (layer->bounds.size.width >= 600 && layer->bounds.size.height >= 300))) {
              ++reported;
              log << "Ultra layer diagnostic panel=" << i << " depth=" << depth
                  << " kind=" << unsigned(layer->kind) << " size=" << layer->bounds.size.width
                  << 'x' << layer->bounds.size.height << " background=" << layer->background.r
                  << ',' << layer->background.g << ',' << layer->background.b << ','
                  << layer->background.a << " fill=" << layer->fill.r << ',' << layer->fill.g << ','
                  << layer->fill.b << ',' << layer->fill.a;
              if (layer->video)
                log << " videoActive=" << layer->video->active()
                    << " native=" << bool(layer->video->native());
              log << '\n';
            }
            for (const auto& child : layer->children)
              self(self, child, depth + 1);
          };
          walk(walk, runtime.scene_root(), 0);
        }
      }
      return turn;
    };
    return config;
  };
}
}  // namespace headunit::theme::script
