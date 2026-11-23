// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <filesystem>
#include <headunit/theme/scene.hpp>
#include <headunit/theme/video_stream.hpp>
#include <headunit/theme/vehicle_script_view.hpp>
#include <functional>
#include <memory>
#include <span>

namespace headunit::theme::script {
using ResourceReader = std::function<std::string(std::string_view)>;
// Share immutable verified resources across display VMs on one owner thread.
// Cache limits bound retained payload and entry count; misses beyond capacity
// still use the verified reader. Changing the bundle requires a new cache.
ResourceReader cache_resources(ResourceReader, std::size_t max_bytes = 32 * 1024 * 1024,
                               std::size_t max_entries = 4096);
// Caller-owned, trusted test/diagnostic directory. This does not verify a package.
struct TrustedDirectory {
  std::filesystem::path content_path;
};

struct Display {
  std::string identifier;
  std::uint32_t width = 1280, height = 720;
  double frame_duration = 1.0 / 60;
};
struct Limits {
  std::size_t memory_bytes = 64 * 1024 * 1024;
  std::size_t tasks = 256;
  std::uint64_t startup_ms = 2000, frame_ms = 20;
};
struct Stats {
  std::size_t memory_bytes = 0, modules = 0, active_tasks = 0, log_records = 0;
  bool faulted = false, foreground_ready_requested = false;
};
// Asset declarations for an independent UI monitor. Receiving these rectangles
// does not prove gauge correctness, visibility, or successful presentation.
struct VerificationRegion {
  std::int32_t x = 0, y = 0, width = 0, height = 0;
  std::string identifier, presentation_style;
  bool operator==(const VerificationRegion&) const = default;
};
struct VerificationReport {
  std::string display_identifier;
  std::optional<double> pts;
  std::vector<VerificationRegion> regions;
};
struct LayoutChoice {
  std::string identifier, name;
  bool operator==(const LayoutChoice&) const = default;
};
struct ClusterContentChoice {
  std::string zone, identifier, name;
  bool active = false;
  // Required local input identifiers are registered. This is not evidence of
  // fresh frames, a qualified provider, or phone-confirmed active content.
  bool available = true;
  bool operator==(const ClusterContentChoice&) const = default;
};
struct ClusterSelectionRequest {
  std::string layout, palette, zone, component;
};
enum class LayoutRequest { unavailable, unknown, busy, selected, requested };
// Current signed-layer state only; timestamp availability is not frame freshness
// or a GPU presentation receipt. No image pixels or phone timestamps are exposed.
struct ClusterVideoState {
  bool visible = false, has_current_pts = false;
  double opacity = 0;
};
struct LayoutSelection {
  std::vector<LayoutChoice> choices;
  std::string active;
  bool ready = false, transitioning = false, idle = false;
  LayoutRequest result = LayoutRequest::unavailable;
  std::vector<LayoutChoice> palettes;
  std::vector<ClusterContentChoice> contents;
  std::string active_palette;
  std::optional<ClusterVideoState> phone_video;
};
// Fixed outcomes recognized in this pack dialect's CP.log calls. These describe
// asset reports, not independently verified session or presentation state.
enum class SessionDiagnosticKind : std::uint8_t {
  session_start,
  session_reset,
  reset_received,
  reset_complete_received,
  session_ready,
  reset_watchdog,
  foreground_reset,
  identity_rejected,
  reset_state_rejected,
  session_number_rejected,
  sequence_rejected,
  sequence_gap,
  acknowledgement_rejected,
  malformed_message,
  transport_error,
  layout_requested,
  layout_not_ready,
  layout_transition_started,
  layout_transition_completed,
  transition_watchdog,
  transition_metadata_rejected,
  remote_ui_shown,
  remote_ui_hidden,
  video_pts_unavailable,
  video_late
};
std::string_view session_diagnostic_name(SessionDiagnosticKind);
struct SessionDiagnostic {
  SessionDiagnosticKind kind;
  std::uint64_t count = 0, monotonic_ns = 0;
  bool connected = false, foregrounded = false;
};

// One isolated Luau VM per display, opened from a previously verified asset.
// Only text modules inside that immutable bundle may be required. There is no
// arbitrary filesystem loader, native-module loader, process or network API.
// Call from one owning event-loop thread. A fault requires destroying the VM.
// The host implements our existing CP ABI and graphics bridge; full conformance
// and live independent-receiver integration still require validation.
class Runtime {
 public:
  Runtime(ResourceReader, Display, Limits = {});
  Runtime(const TrustedDirectory&, Display, Limits = {});
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  void evaluate(std::string_view trusted_source, std::string_view label = "host");
  // Host-supplied compositor/video time in seconds, never synthesized from CPU
  // elapsed time. A missing PTS is exposed as nil. Uptime uses a monotonic clock.
  void tick(std::optional<double> presented_pts = {});
  // One initial scheduler turn after entry-point evaluation, bounded by the
  // startup deadline. Subsequent turns always use the shorter frame budget.
  // Rejects a second call or a call after normal post-evaluation scheduling.
  void start_tasks(std::optional<double> presented_pts = {});
  void set_connected(bool);
  // Host/compositor lifecycle, independent of phone connectivity and PTS.
  // Defaults false. The session supervisor must hide/fallback on VM failure.
  void set_foregrounded(bool);
  bool receive(Bytes session_plist);
  std::optional<Bytes> take_outgoing();
  // Changes when reset/disconnect/fault clears message queues. A transport must
  // discard an unsent message taken before that change, for this display only.
  std::uint64_t message_generation() const;
  graphics::Image render(graphics::RenderCosts* costs =
                             nullptr);  // CPU reference frame; no presentation or PTS advancement.
  // Owner-thread access for a native compositor. Consume before the next VM
  // tick; this live scene is not an immutable cross-thread snapshot.
  std::shared_ptr<const graphics::Layer> scene_root() const;
  // Register explicitly negotiated inputs before loading the asset entry point.
  // Host retains the stream to submit mapped decoded frames and confirm display.
  std::shared_ptr<video::Stream> add_video_input(video::Input);
  // Bind before evaluating scripts. The clock must share the sensor provider's
  // monotonic millisecond domain; default is steady_clock since epoch.
  void bind_vehicle(std::shared_ptr<vehicle::ReadOnlyService>, std::vector<vehicle::ScriptBinding>,
                    std::function<std::uint64_t()> monotonic_ms = {},
                    vehicle::ScriptView::Dismissals = {});
  // Host policy for one verified asset/display. When a requested layout has
  // exactly one declared remote-only DCA_Map, select it if no component is set.
  // Configure before evaluating the asset; no receiver-specific IDs are baked in.
  void select_remote_map_component();
  // Bench-only local startup selection for a layout declared by this signed
  // asset. Supplies the asset's optional Config.json, never rewrites the pack.
  void select_startup_layout(std::string layout_id);
  // Read the signed workspace on its VM owner, without dispatching actions.
  // Supported pack dialect is the same LayoutController used by layout selection.
  enum class ScenePhase { unavailable, logos, startup, drive, other };
  void enable_scene_observation();  // Before evaluation.
  ScenePhase scene_phase();
  // Opt-in adapter for the signed pack's LayoutController module. Observes its
  // constructor without changing resource bytes or its returned object. Local
  // requests use that controller's existing ordered UI-Sync session; the phone
  // still owns START/END_LAYOUT_TRANSITION. Call enable before evaluation and
  // selection only on the VM owner thread, after entry-point evaluation.
  void enable_live_layout_selection();
  LayoutSelection layout_selection(std::optional<std::string_view> request = {});
  LayoutSelection cluster_selection(const std::optional<ClusterSelectionRequest>& request = {});
  // Required before an asset can report verified-UI regions. Invoked on the VM
  // owner thread; must not reenter this Runtime. Throw to reject a report.
  // A monitor must independently check the actual output and stale/missing data;
  // this notification ABI itself returns no verification result to the asset.
  void bind_verification_reporter(std::function<void(const VerificationReport&)>);
  // Optional owner-thread diagnostic sink. No script payload leaves the VM by
  // default. The host must bound retention and must not reenter this Runtime.
  void bind_log_sink(std::function<void(int, std::string_view, std::string_view)>);
  // Optional safe, fixed-size outcomes from allowlisted session/transition log
  // messages. Never forwards the original tag, text, identifiers or payload.
  // Bind before evaluation; default off. Emit at most max_events (1..256) per
  // VM lifetime and 16 per kind: first four occurrences, then powers of two.
  // The count includes skipped duplicates. Called on the owner thread; the
  // sink must not reenter the VM. Existing raw log-sink behavior is independent.
  void bind_session_diagnostic_sink(std::function<void(const SessionDiagnostic&)>,
                                    std::size_t max_events = 64);
  // The owner supplies an asset/display-scoped private store. Values are
  // bounded binary property lists, with graph expansion checked before parsing;
  // an unbound VM cannot write preferences. Write callbacks must copy their
  // borrowed span before handing work to another thread.
  void bind_persistent_storage(
      std::function<std::optional<Bytes>(std::string_view)> read,
      std::function<void(std::string_view, std::span<const std::uint8_t>)> write);
  // Image-archive content is supplied by the owner when available. Missing
  // entries return nil to the asset; the VM never reads an arbitrary path.
  void bind_custom_image_provider(std::function<std::optional<Bytes>(std::string_view, int)>);
  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  void run_tasks(std::optional<double>, bool startup);
};
}  // namespace headunit::theme::script
