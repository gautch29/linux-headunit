// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Internal to headunit-control-probe: the receiver's per-connection state and
// the startup context shared by every phone session. Not a public interface.
#pragma once

#include "headunit/airplay_identity.hpp"
#include "headunit/airplay_info.hpp"
#include "headunit/display_configuration.hpp"
#include "headunit/airplay_auth_setup.hpp"
#include "headunit/airplay_session_setup.hpp"
#include "headunit/audio_output.hpp"
#include "headunit/audio_control_bridge.hpp"
#include "headunit/cluster_input.hpp"
#include "headunit/control_request.hpp"
#include "headunit/control_response.hpp"
#include "headunit/night_mode.hpp"
#include "headunit/limited_ui.hpp"
#include "headunit/vehicle_facts.hpp"
#include "headunit/connection_diagnostics.hpp"
#include "headunit/bluetooth_iap_profile.hpp"
#include "headunit/hap_tlv.hpp"
#include "headunit/hap_pair_setup.hpp"
#include "headunit/hap_pair_store.hpp"
#include "headunit/hap_pair_verify.hpp"
#include "headunit/hap_control_frame.hpp"
#include "headunit/linux_mfi_bus.hpp"
#include "headunit/software_mfi_authenticator.hpp"
#include "headunit/video_output.hpp"
#include "headunit/presentation_status.hpp"
#include "headunit/navigation_socket.hpp"
#ifdef HEADUNIT_HAVE_STANDARD_GPU
#include "headunit/camera_control.hpp"
#include "headunit/camera_surface_lease.hpp"
#include "headunit/main_surface_control.hpp"
#endif
#include "headunit/theme/vehicle_session.hpp"
#include "headunit/theme/camera_state.hpp"
#include "headunit/theme/content_control.hpp"
#ifdef HEADUNIT_HAVE_LIVE_UI
#include "headunit/theme/live_ui.hpp"
#endif

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <optional>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace headunit::control {
constexpr std::size_t max_connections = 4;
constexpr std::size_t max_bytes_per_connection = 65536;

using ConnectionStage = headunit::ConnectionDiagnostics::Stage;
struct Connection {
  headunit::ConnectionDiagnostics diagnostics{headunit::ConnectionDiagnostics::Scope::control, 0};
  headunit::ProtocolCoverage route_coverage;
  bool setup_completed = false;
  std::optional<headunit::HapPairRecord> new_pairing_record;
  int socket = -1;
  int capture = -1;
  std::size_t bytes = 0;
  unsigned number = 0;
  std::string request_buffer;
  std::unique_ptr<headunit::HapPairSetup> pairing;
  std::unique_ptr<headunit::HapPairVerify> verification;
  std::unique_ptr<headunit::HapControlFrame> framing;
  std::unique_ptr<headunit::AirplaySessionSetup> session_setup;
  std::shared_ptr<headunit::VideoOutput> video_output;
  bool record_accepted = false, presentation_retired = false, presentation_ever_ready = false;
  std::uint64_t main_restorations = 0;  // Home returns already given a keyframe request.
  std::unique_ptr<headunit::theme::vehicle::Session> vehicle_session;
  std::shared_ptr<headunit::theme::vehicle::CameraStateSnapshot> camera_state;
  std::shared_ptr<headunit::theme::vehicle::ContentStateSnapshot> content_state;
  headunit::theme::vehicle::Session::Status vehicle_status;
  bool vehicle_fault_logged = false;
#ifdef HEADUNIT_HAVE_LIVE_UI
  std::unique_ptr<headunit::theme::script::UiSession> ultra_ui;
  std::shared_ptr<headunit::MainPresentationSnapshot> ultra_presentation;
  std::shared_ptr<headunit::theme::script::UiClockSnapshot> ultra_clock;
  std::shared_ptr<headunit::TouchMailbox> ultra_touches;
  std::unique_ptr<headunit::ClusterInputControl> cluster_input;
  std::shared_ptr<headunit::theme::video::ReceiverVideo> ultra_video;
  int ultra_state = -1;
  bool ultra_fault_logged = false;
  std::size_t ultra_received = 0, ultra_sent = 0;
  unsigned touch_diagnostic_reports = 0;
  std::chrono::steady_clock::time_point next_touch_diagnostic{};
  std::array<std::optional<std::uint64_t>, 2> ultra_refresh_generation{};
#endif
  std::size_t navigation_updates = 0;
  unsigned navigation_requests = 0;
  std::size_t media_updates = 0;
  unsigned media_requests = 0;
  bool navigation_available = false;
  unsigned navigation_state = 255;
  std::shared_ptr<headunit::Iap2SessionEngine> iap_engine;
  std::size_t logged_video_configs = 0;
  std::size_t logged_video_frames = 0;
  std::size_t command_requests = 0;
  std::size_t flush_requests = 0;
  std::optional<bool> night_mode_sent;
  std::optional<bool> limited_ui_sent;
  // Phone car-icon handoff: mainScreen owner last seen, phone releases counted
  // for the native page, and whether native Home opened since the release.
  headunit::AirplayResourceOwner screen_owner_seen = headunit::AirplayResourceOwner::unobserved;
  std::uint64_t phone_home_requests = 0;
  bool home_opened_since_release = false;
  std::size_t feedback_requests = 0;
  std::size_t rejected_requests = 0;
  std::size_t touch_reports = 0;
  std::size_t command_capture_bytes = 0;
  bool command_capture_failed = false;
  bool cluster_map_requested = false;
  bool clock_locked = false, clock_fault_logged = false;
};

// One receiver owns its Ultra resource and workers. A disconnected session
// retires its worker here; the network loop reaps it after the worker finishes.
struct UltraRuntime {
  std::string launch_nonce;
#ifdef HEADUNIT_HAVE_LIVE_UI
  std::shared_ptr<headunit::theme::script::InstalledUi> installed_ui;
  bool native_video = false;
  bool negotiate = false;
  std::vector<std::unique_ptr<headunit::theme::script::UiSession>> retired;

  void attach(Connection& connection, const headunit::ReceiverDisplays& displays,
              const headunit::AirplayIdentity& identity) const {
    if (!installed_ui) return;
    if (negotiate && !installed_ui->cluster_input_file().empty())
      connection.cluster_input = std::make_unique<headunit::ClusterInputControl>(
          installed_ui->cluster_input_file(), launch_nonce, connection.number);
    if (native_video) {
      connection.ultra_video = std::make_shared<headunit::theme::video::ReceiverVideo>();
      connection.ultra_video->attach(*connection.session_setup, negotiate);
    }
    connection.ultra_touches =
        std::make_shared<headunit::TouchMailbox>(installed_ui->diagnostics_enabled());
    connection.ultra_clock = std::make_shared<headunit::theme::script::UiClockSnapshot>();
    connection.ultra_presentation = std::make_shared<headunit::MainPresentationSnapshot>();
    const auto controller =
        connection.verification ? connection.verification->verified_controller() : std::nullopt;
    const bool newly_paired = controller && connection.new_pairing_record &&
                              controller->identifier == connection.new_pairing_record->identifier &&
                              controller->public_key == connection.new_pairing_record->public_key;
    connection.ultra_ui =
        std::make_unique<headunit::theme::script::UiSession>(installed_ui->factory(
            negotiate ? displays[0].panel_name : identity.main_display_id,
            negotiate ? displays[1].panel_name : identity.cluster_display_id,
            connection.ultra_clock, connection.ultra_video, connection.ultra_touches, displays,
            connection.ultra_presentation, connection.camera_state,
            {launch_nonce, connection.number}, newly_paired, connection.content_state));
    connection.ultra_ui->attach(*connection.session_setup);
    if (negotiate) connection.session_setup->enable_native_ultra();
  }

  void retire(Connection& connection) {
    if (connection.camera_state)
      connection.camera_state->close(headunit::presentation_monotonic_ns());
    if (connection.content_state)
      connection.content_state->close(headunit::presentation_monotonic_ns() / 1000000);
    connection.ultra_presentation.reset();
    if (connection.ultra_video) connection.ultra_video->close();
    if (connection.ultra_touches) {
      connection.ultra_touches->retire();
      connection.ultra_touches.reset();
    }
    if (connection.ultra_ui) {
      connection.ultra_clock->update({});
      connection.ultra_ui->stop();
      retired.push_back(std::move(connection.ultra_ui));
      connection.ultra_clock.reset();
    }
  }

  void reap() {
    std::erase_if(retired, [](const auto& owner) { return owner->finished(); });
  }
  bool has_capacity() const { return retired.size() < max_connections; }
#else
  static constexpr bool negotiate = false;
  void retire(Connection&) {}
  void reap() {}
  bool has_capacity() const { return true; }
#endif
};

struct SetupSecret {
  std::string value;
  ~SetupSecret() {
    if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
  }
};

// Immutable startup configuration and services shared by every phone session.
// Connection keeps all mutable protocol state; this context borrows owners in
// main and never outlives them.
struct ReceiverContext {
  const std::string& directory;
  const std::string& preview_sink;
  const std::string& preview_decoder;
  const std::string& standard_cluster_wayland;
  const headunit::MainPreviewOptions& main_preview;
  const std::string& audio_sink;
  const std::string& audio_device;
  const std::string& microphone_source;
  const std::string& microphone_device;
  const headunit::audio_control::Level& audio_level;
  const headunit::ReceiverDisplays& displays;
  const headunit::ReceiverDisplays& standard_displays;
  const std::optional<std::vector<std::uint8_t>>& info_body;
  const std::vector<std::uint8_t>& standard_info_body;
  const SetupSecret* setup_secret;
  const headunit::AirplayIdentity* identity;
  const std::string* private_key_path;
  const headunit::HapPairStore* pair_store;
  headunit::MfiAuthenticator* mfi_auth;
  headunit::BluetoothIapProfile* bluetooth;
  UltraRuntime& ultra;
  std::optional<headunit::theme::vehicle::BenchProfile> vehicle_profile;
  std::shared_ptr<const headunit::theme::vehicle::Configuration> vehicle_configuration;
  std::shared_ptr<headunit::theme::vehicle::Provider> vehicle_provider;
  headunit::theme::vehicle::Transport::WriteObserver vehicle_observer;
  std::shared_ptr<headunit::audio_observation::Capture> audio_observation;
};

// Closes the sockets, logs the session summary, and retires every owner of
// this connection. The Ultra worker keeps running in UltraRuntime::retired
// until it finishes its own teardown.
void close_connection(Connection& connection, BluetoothIapProfile* bluetooth, UltraRuntime& ultra,
                      ConnectionDiagnostics::ControlRetirement reason);

// Answers every complete request in connection.request_buffer. Returns a
// retirement reason when the connection must close, std::nullopt otherwise.
std::optional<ConnectionDiagnostics::ControlRetirement> process_requests(
    Connection& connection, const ReceiverContext& context);

void write_all(int fd, const std::uint8_t* bytes, std::size_t length);
std::unique_ptr<SetupSecret> read_setup_secret(const char* path);
void check_private_directory(const std::string& path);
unsigned parse_number(const char* value, unsigned maximum, const char* name,
                      bool allow_zero = false);
}  // namespace headunit::control
