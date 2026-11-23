// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "control_receiver.hpp"
#include "headunit/audio_microphone.hpp"
#include <headunit/theme/content_control.hpp>
#include <exception>

namespace {
using namespace headunit::control;

volatile std::sig_atomic_t stop_requested = 0;

void on_signal(int) {
  stop_requested = 1;
}

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (!EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) ||
      length != 32)
    throw std::runtime_error("cannot hash iAP artwork");
  constexpr char digits[] = "0123456789abcdef";
  std::string text(64, '0');
  for (unsigned i = 0; i < 32; ++i) {
    text[i * 2] = digits[digest[i] >> 4];
    text[i * 2 + 1] = digits[digest[i] & 15];
  }
  return text;
}

void publish_iap_media(const std::string& destination, const std::string& content) {
  const auto temporary = destination + ".tmp." + std::to_string(getpid());
  int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("cannot create iAP media snapshot");
  try {
    write_all(fd, reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    if (close(fd)) throw std::runtime_error("cannot close iAP media snapshot");
    fd = -1;
    if (rename(temporary.c_str(), destination.c_str()))
      throw std::runtime_error("cannot publish iAP media snapshot");
  } catch (...) {
    if (fd >= 0) close(fd);
    unlink(temporary.c_str());
    throw;
  }
}

// --- Per-connection session pump -------------------------------------------
// Each poll iteration drains the phone session and reports state changes.
// Logging is change-triggered so a steady session stays quiet.

void attach_late_iap(Connection& connection, headunit::BluetoothIapProfile* bluetooth) {
  if (bluetooth && !connection.iap_engine) {
    auto iap_engine = bluetooth->engine();
    if (iap_engine && iap_engine->established() && iap_engine->handoff_sent()) {
      connection.session_setup->attach_iap_engine(iap_engine);
      connection.iap_engine = std::move(iap_engine);
      std::cout << "late iAP handoff attached\n" << std::flush;
    }
  }
}

// Navigation and now-playing subscriptions arrive over iAP2.
void report_phone_metadata(Connection& connection) {
  if (connection.iap_engine && connection.video_output)
    connection.video_output->set_navigation(connection.iap_engine->navigation());
  if (connection.iap_engine && connection.iap_engine->navigation()) {
    const auto requests = connection.iap_engine->navigation_requests();
    if (requests != connection.navigation_requests) {
      std::cout << "navigation subscriptions=" << requests
                << " wifi=" << connection.iap_engine->wifi_active() << '\n'
                << std::flush;
      connection.navigation_requests = requests;
    }
    const auto count = connection.iap_engine->navigation_updates();
    const auto nav = connection.iap_engine->navigation()->snapshot(headunit::Clock::now());
    if (nav.available != connection.navigation_available ||
        nav.route_state.value_or(255) != connection.navigation_state ||
        (count != connection.navigation_updates &&
         (connection.navigation_updates == 0 || count / 20 > connection.navigation_updates / 20))) {
      std::cout << "navigation updates=" << count
                << " rejected=" << connection.iap_engine->navigation_rejections()
                << " available=" << nav.available
                << " state=" << unsigned(nav.route_state.value_or(255))
                << " maneuver=" << (nav.maneuver ? int(nav.maneuver->type.value_or(255)) : -1)
                << " distance_m=" << (nav.distance_m ? std::to_string(*nav.distance_m) : "unknown")
                << '\n'
                << std::flush;
    }
    connection.navigation_updates = count;
    connection.navigation_available = nav.available;
    connection.navigation_state = nav.route_state.value_or(255);
  }
  if (connection.iap_engine && connection.iap_engine->now_playing()) {
    const auto requests = connection.iap_engine->now_playing_requests();
    const auto updates = connection.iap_engine->now_playing_updates();
    if (requests != connection.media_requests || updates != connection.media_updates) {
      std::cout << "media subscriptions=" << requests << " updates=" << updates
                << " rejected=" << connection.iap_engine->now_playing_rejections() << '\n'
                << std::flush;
      connection.media_requests = requests;
      connection.media_updates = updates;
    }
  }
}

// Phone-facing Climate/settings resources (simulated provider on the bench).
void pump_vehicle(Connection& connection) {
  if (connection.vehicle_session) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    connection.vehicle_session->pump(*connection.session_setup, now);
    const auto status = connection.vehicle_session->status();
    if (status != connection.vehicle_status) {
      std::cout << "vehicle channels=" << status.channels << " requests=" << status.received
                << " writes=" << status.writes << " config=" << status.requests.configuration
                << " read=" << status.requests.read << " register=" << status.requests.registration
                << " unregister=" << status.requests.unregistration
                << " control=" << status.requests.control << " other=" << status.requests.other
                << '\n'
                << std::flush;
      if (status.camera_resources &&
          status.camera_resources != connection.vehicle_status.camera_resources) {
        for (unsigned index = 0; index < status.camera_resources->size(); ++index) {
          const auto& r = (*status.camera_resources)[index];
          std::cout << "camera resource=" << (index ? "exterior" : "input") << " read=" << r.reads
                    << " register=" << r.registrations << " unregister=" << r.unregistrations
                    << " responseOnTransferred=" << r.responses_on
                    << " responseOffTransferred=" << r.responses_off
                    << " responseErrorsTransferred=" << r.response_errors
                    << " updateOnTransferred=" << r.updates_on
                    << " updateOffTransferred=" << r.updates_off
                    << " updateErrorsTransferred=" << r.update_errors << '\n'
                    << std::flush;
        }
      }
      connection.vehicle_status = status;
    }
    if (!connection.vehicle_fault_logged && !connection.vehicle_session->failure_reason().empty()) {
      std::cout << "vehicle channel stopped: " << connection.vehicle_session->failure_reason()
                << '\n'
                << std::flush;
      connection.vehicle_fault_logged = true;
    }
  }
}

#ifdef HEADUNIT_HAVE_LIVE_UI
// Drives the Ultra UI-Sync worker, its touch mailbox and native video input.
// Returns false when the Ultra session is terminal and the control connection
// must close.
bool pump_ultra(Connection& connection, const UltraRuntime& ultra,
                const headunit::AirplayIdentity* identity,
                const std::optional<headunit::AirplayClockSample>& timing) {
  if (connection.ultra_ui && (!ultra.negotiate || connection.session_setup->negotiated_mode() ==
                                                      headunit::CarplayMode::ultra)) {
    connection.ultra_clock->update(timing);
    connection.ultra_ui->pump(*connection.session_setup);
    if (const auto status = connection.ultra_ui->status()) {
      const int state = static_cast<int>(status->state);
      if (timing && identity && connection.ultra_video &&
          status->state == headunit::theme::script::UiWorker::State::running) {
        for (unsigned i = 0; i < 2; ++i) {
          const auto id = connection.session_setup->video_connection_id(110 + i);
          if (id && connection.ultra_refresh_generation[i] != id &&
              connection.session_setup->request_video_refresh(
                  110 + i, i ? identity->cluster_display_id : identity->main_display_id)) {
            connection.ultra_refresh_generation[i] = id;
            std::cout << "Ultra generation video refresh queued type=" << 110 + i << '\n'
                      << std::flush;
          }
        }
      }
      if (state != connection.ultra_state) {
        std::cout << "Ultra worker state=" << state << " received=" << status->received
                  << " sent=" << status->sent << '\n'
                  << std::flush;
        connection.ultra_state = state;
      }
      if (status->sent != connection.ultra_sent) {
        if (connection.ultra_sent < 8)
          std::cout << "Ultra UI output count=" << status->sent << '\n' << std::flush;
        connection.ultra_sent = status->sent;
      }
      if (status->received != connection.ultra_received) {
        if (connection.ultra_received < 4)
          std::cout << "Ultra UI input count=" << status->received << '\n' << std::flush;
        connection.ultra_received = status->received;
      }
    }
    if (!connection.ultra_fault_logged && !connection.ultra_ui->failure_reason().empty()) {
      if (connection.camera_state)
        connection.camera_state->close(headunit::presentation_monotonic_ns());
      std::cout << "Ultra UI stopped: " << connection.ultra_ui->failure_reason() << '\n'
                << std::flush;
      connection.ultra_fault_logged = true;
    }
    if (connection.cluster_input) {
      const auto status = connection.ultra_ui->status();
      const auto presentation = connection.ultra_presentation
                                    ? connection.ultra_presentation->sample()
                                    : headunit::MainPresentationStatus{};
      const bool ready = connection.record_accepted && connection.ultra_ui->stream_id() && status &&
                         status->state == headunit::theme::script::UiWorker::State::running &&
                         presentation.qualified && presentation.healthy;
      connection.cluster_input->poll(
          ready,
          [&](headunit::HidKeyTarget target, std::uint8_t report) {
            if (target == headunit::HidKeyTarget::media)
              return connection.session_setup->send_media_button(report);
            return connection.session_setup->send_cluster_button(report);
          },
          std::chrono::steady_clock::now(),
          [&](const std::string& url) {
            const bool queued = connection.session_setup->request_ui(url);
            if (queued) std::cout << "phone app requested url=" << url << '\n' << std::flush;
            return queued;
          });
    }
    if (connection.ultra_touches) {
      const auto status = connection.ultra_ui->status();
      if (!connection.ultra_ui->stream_id() || !status ||
          status->state == headunit::theme::script::UiWorker::State::faulted)
        connection.ultra_touches->retire();
      const auto sent = connection.ultra_touches->drain([&](const headunit::TouchInput& report) {
        return connection.session_setup->send_touch(report);
      });
      if (sent && !connection.touch_reports)
        std::cout << "first native Ultra touch report sent\n" << std::flush;
      connection.touch_reports += sent;
      const auto now = std::chrono::steady_clock::now();
      if (sent && connection.touch_diagnostic_reports < 24 &&
          now >= connection.next_touch_diagnostic) {
        const auto timing = connection.ultra_touches->take_timing();
        if (timing.reports) {
          std::cout << "Ultra touch mailbox diagnostic reports=" << timing.reports
                    << " enqueueToHidQueueMeanMs=" << timing.total_ns / double(timing.reports) / 1e6
                    << " p50UpperMs=" << timing.percentile_upper_ns(50) / 1e6
                    << " p95UpperMs=" << timing.percentile_upper_ns(95) / 1e6
                    << " maxMs=" << timing.max_ns / 1e6 << '\n'
                    << std::flush;
          ++connection.touch_diagnostic_reports;
          connection.next_touch_diagnostic = now + std::chrono::seconds(5);
        }
      }
    }
    if (connection.ultra_video) {
      connection.ultra_video->pump(*connection.session_setup);
      if (!connection.ultra_ui->failure_reason().empty() ||
          (connection.ultra_video->claimed() && !connection.ultra_ui->stream_id()))
        connection.ultra_video->close();
      if (connection.ultra_video->closed()) {
        if (const auto id = connection.ultra_ui->stream_id())
          (void)connection.session_setup->close_service(*id);
        connection.ultra_ui->stop();
      }
    }
    // A native Ultra renderer cannot resume from the middle of HEVC
    // delta frames after its worker or paired video input dies. Leaving
    // the AirPlay control socket established also blocks our selected-
    // phone reconnect helper. End this terminal session explicitly;
    // the next connection gets fresh crypto, decoders and script VMs.
    if (!connection.ultra_ui->failure_reason().empty() ||
        (connection.ultra_video && connection.ultra_video->closed())) {
      std::cout << "terminal Ultra session; closing control connection" << '\n' << std::flush;
      connection.diagnostics.mark(ConnectionStage::failed);
      return false;
    }
  }
  return true;
}
#endif

void report_clock(Connection& connection,
                  const std::optional<headunit::AirplayClockSample>& timing) {
  if (connection.video_output) connection.video_output->update_clock(timing);
  if (timing.has_value() != connection.clock_locked) {
    connection.clock_locked = timing.has_value();
    std::cout << "phone clock " << (timing ? "synchronized" : "unavailable");
    if (timing) std::cout << " uncertainty_ns=" << timing->uncertainty_ns;
    const auto diagnostics = connection.session_setup->timing_diagnostics();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    std::cout << " mono_ns=" << now_ns << " probes=" << diagnostics.sent
              << " accepted=" << connection.session_setup->timing_replies()
              << " unmatched=" << diagnostics.unmatched
              << " invalid=" << diagnostics.invalid_exchange << " delayed=" << diagnostics.delayed
              << " last_delay_ns=" << diagnostics.last_delay_ns
              << " max_delay_ns=" << diagnostics.max_delay_ns;
    if (diagnostics.last_accepted_ns)
      std::cout << " accepted_age_ns=" << std::uint64_t(now_ns) - diagnostics.last_accepted_ns;
    std::cout << '\n' << std::flush;
  }
  if (connection.session_setup->timing_failed() && !connection.clock_fault_logged) {
    connection.clock_fault_logged = true;
    const auto fault = connection.session_setup->timing_diagnostics().fault;
    std::cout << "phone clock discontinuity: new Ultra timeline required reason="
              << (fault.reason == headunit::AirplayClock::FaultReason::relock_gap ? "relock_gap"
                                                                                  : "phase_jump")
              << " phase_delta_ns=" << fault.phase_delta_ns
              << " phase_bound_ns=" << fault.phase_bound_ns
              << " accepted_age_ns=" << fault.accepted_age_ns
              << " previous_uncertainty_ns=" << fault.previous_uncertainty_ns
              << " next_uncertainty_ns=" << fault.next_uncertainty_ns
              << " exchange_ns=" << fault.exchange_ns << " processing_ns=" << fault.processing_ns
              << '\n'
              << std::flush;
  }
}

// Cluster map request, Standard touch forwarding and video/iAP diagnostics.
void pump_standard_outputs(Connection& connection, const headunit::AirplayIdentity* identity) {
  if (identity && !connection.cluster_map_requested &&
      connection.session_setup->show_cluster_map(identity->cluster_display_id)) {
    connection.cluster_map_requested = true;
    std::cout << "cluster map showUI queued once\n" << std::flush;
  }
  if (connection.video_output) {
    (void)connection.video_output->drain_touches([&](headunit::TouchInput touch) {
      if (connection.session_setup->send_touch(touch)) {
        ++connection.touch_reports;
        if (connection.touch_reports == 1) std::cout << "first touch report sent\n" << std::flush;
        return true;
      }
      return false;
    });
  }
  const auto video_configs = connection.session_setup->video_configs();
  const auto video_frames = connection.session_setup->video_frames();
  if (video_configs) connection.diagnostics.mark(ConnectionStage::video_config);
  if (video_frames) connection.diagnostics.mark(ConnectionStage::video_frame);
  if (video_configs > connection.logged_video_configs) {
    std::cout << "video configuration received count=" << video_configs << '\n' << std::flush;
    connection.logged_video_configs = video_configs;
  }
  if (video_frames > connection.logged_video_frames &&
      (connection.logged_video_frames == 0 ||
       video_frames / 120 > connection.logged_video_frames / 120)) {
    std::cout << "video frames received count=" << video_frames << '\n' << std::flush;
  }
  connection.logged_video_frames = video_frames;
  for (const auto& control : connection.session_setup->take_iap_controls()) {
    std::cout << "Wi-Fi iAP control id=0x" << std::hex << control.identifier << std::dec
              << " parameter-bytes=" << control.parameters.size() << '\n'
              << std::flush;
  }
}

// Day/night appearance: the phone learns support from /info nightMode, then
// each change (including the first after RECORD) is sent as setNightMode.
void pump_night_mode(Connection& connection) {
  const auto night = headunit::receiver_night_mode_now();
  if (!night || connection.night_mode_sent == night) return;
  if (!connection.session_setup->send_night_mode(*night)) return;
  connection.night_mode_sent = night;
  std::cout << "night mode sent " << (*night ? "on" : "off") << '\n' << std::flush;
}

// Driving restrictions follow fresh vehicle speed with hysteresis; an unknown
// speed keeps the last state. The first change after RECORD is always sent.
void pump_limited_ui(Connection& connection) {
  const auto policy = headunit::receiver_limited_ui();
  if (!policy) return;
  std::optional<double> speed;
  if (const auto facts = headunit::fresh_vehicle_facts()) speed = facts->speed_kmh;
  const bool limited =
      headunit::limited_ui_next(*policy, connection.limited_ui_sent.value_or(false), speed);
  if (connection.limited_ui_sent == limited) return;
  if (!connection.session_setup->send_limited_ui(limited)) return;
  connection.limited_ui_sent = limited;
  std::cout << "limited UI sent " << (limited ? "on" : "off") << '\n' << std::flush;
}

// Returns false when the connection must be retired as an owner failure.
bool pump_session(Connection& connection, headunit::BluetoothIapProfile* bluetooth,
                  [[maybe_unused]] const UltraRuntime& ultra,
                  const headunit::AirplayIdentity* identity) {
  try {
    attach_late_iap(connection, bluetooth);
    connection.session_setup->drain();
    report_phone_metadata(connection);
    const auto timing = connection.session_setup->timing_sample();
    if (timing) connection.diagnostics.mark(ConnectionStage::clock_locked);
    pump_vehicle(connection);
#ifdef HEADUNIT_HAVE_LIVE_UI
    if (!pump_ultra(connection, ultra, identity, timing)) return false;
#endif
    report_clock(connection, timing);
    pump_standard_outputs(connection, identity);
    pump_night_mode(connection);
    pump_limited_ui(connection);
    return true;
  } catch (const std::exception&) {
    std::cerr << "stream session rejected invalid data\n";
    connection.diagnostics.mark(ConnectionStage::failed);
    return false;
  }
}

// --- Control listener ------------------------------------------------------

int open_listener(unsigned port) {
  const int server = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (server < 0) throw std::runtime_error("cannot create control listener");
  int enabled = 1;
  int dual_stack = 0;
  if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) ||
      setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, &dual_stack, sizeof(dual_stack))) {
    close(server);
    throw std::runtime_error("cannot configure control listener");
  }
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(static_cast<std::uint16_t>(port));
  address.sin6_addr = in6addr_any;
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
      listen(server, static_cast<int>(max_connections))) {
    close(server);
    throw std::runtime_error("cannot bind control listener");
  }
  return server;
}

// Returns false when accept would block or was interrupted; the caller then
// skips the rest of this iteration.
bool accept_connection(int server, const std::string& directory,
                       std::vector<Connection>& connections, unsigned& next_number) {
  const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (client < 0 && (errno == EINTR || errno == EAGAIN)) return false;
  if (client < 0) throw std::runtime_error("control connection accept failed");
  const std::string filename = directory + "/connection-" + std::to_string(next_number) + ".bin";
  const int capture = open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (capture < 0) {
    close(client);
    throw std::runtime_error("cannot open private capture file");
  }
  auto& accepted = connections.emplace_back();
  accepted.socket = client;
  accepted.capture = capture;
  accepted.number = next_number++;
  accepted.diagnostics = headunit::ConnectionDiagnostics(
      headunit::ConnectionDiagnostics::Scope::control, accepted.number);
  accepted.diagnostics.mark(ConnectionStage::accepted);
  if (!next_number) throw std::runtime_error("control connection generation exhausted");
  std::cout << "connection accepted\n" << std::flush;
  return true;
}

// --- Publications for the native page and audio controls -------------------

// Content-free actual owner facts, never Bluetooth, process liveness or
// parsed diagnostics. The newest authenticated session is authoritative.
void publish_presentation(
    std::unique_ptr<headunit::PresentationStatusPublisher>& presentation_publisher,
    std::vector<Connection>& connections, std::uint64_t& presentation_connection,
    const headunit::AirplayIdentity* identity) {
  if (!presentation_publisher) return;
  // Content-free actual owner facts, never Bluetooth, process liveness or
  // parsed diagnostics. The newest authenticated session is authoritative.
  Connection* selected = nullptr;
  for (auto& connection : connections)
    if (connection.framing && connection.session_setup &&
        (!selected || connection.number > selected->number))
      selected = &connection;
  auto phase = headunit::PresentationState::disconnected;
  bool eligible = false;
  if (selected && selected->number >= presentation_connection) {
    presentation_connection = selected->number;
    phase = headunit::PresentationState::starting;
    if (selected->presentation_retired || selected->session_setup->timing_failed())
      phase = headunit::PresentationState::fault;
    else if (selected->record_accepted) {
      headunit::MainPresentationStatus main;
#ifdef HEADUNIT_HAVE_LIVE_UI
      if (selected->session_setup->negotiated_mode() == headunit::CarplayMode::ultra &&
          selected->ultra_ui) {
        const auto worker = selected->ultra_ui->status();
        if (worker && worker->state == headunit::theme::script::UiWorker::State::running &&
            selected->ultra_ui->stream_id() && selected->ultra_presentation &&
            selected->ultra_video && !selected->ultra_video->closed())
          main = selected->ultra_presentation->sample();
      } else
#endif
          if (selected->video_output)
        main = selected->video_output->main_presentation_status();
      // Home drained the main video in either mode; without a request the
      // returned window waits for the phone's next spontaneous keyframe
      // (25 s observed on a static CarPlay home screen). Ask once per return.
      if (identity && main.surface_restorations > selected->main_restorations &&
          selected->session_setup->request_video_refresh(110, identity->main_display_id)) {
        selected->main_restorations = main.surface_restorations;
        std::cout << "Main return video refresh queued returns=" << main.surface_restorations
                  << '\n'
                  << std::flush;
      }
      // The phone's car icon releases mainScreen to us: ask the native page to
      // open Home. Once Home has opened and closed again while we still own
      // the screen, ask the phone to show CarPlay (requestUI).
      const auto screen_owner =
          headunit::airplay_resource_owner(selected->session_setup->modes().resources[0]);
      if (screen_owner == headunit::AirplayResourceOwner::accessory &&
          selected->screen_owner_seen == headunit::AirplayResourceOwner::controller) {
        ++selected->phone_home_requests;
        selected->home_opened_since_release = false;
        std::cout << "phone released main screen requests=" << selected->phone_home_requests << '\n'
                  << std::flush;
      }
      selected->screen_owner_seen = screen_owner;
      if (screen_owner == headunit::AirplayResourceOwner::accessory) {
        if (main.home)
          selected->home_opened_since_release = true;
        else if (selected->home_opened_since_release && selected->session_setup->request_ui()) {
          selected->home_opened_since_release = false;
          std::cout << "native Home closed; requestUI queued\n" << std::flush;
        }
      }
      eligible = main.qualified && main.healthy;
      if (eligible) {
        selected->diagnostics.mark(
            ConnectionStage::presentation_ready, 0,
            selected->iap_engine ? selected->iap_engine->diagnostic_generation() : 0);
        selected->presentation_ever_ready = true;
        phase = main.home ? headunit::PresentationState::home : headunit::PresentationState::ready;
      } else if (selected->presentation_ever_ready && !main.healthy)
        phase = headunit::PresentationState::fault;
    }
  }
  try {
    presentation_publisher->update(
        phase, presentation_connection, eligible,
        selected ? selected->session_setup->negotiated_mode() : std::nullopt,
        selected && selected->number == presentation_connection ? selected->phone_home_requests
                                                                : 0);
  } catch (const std::exception& error) {
    // Native actions require fresh status from this receiver. Retiring the
    // publisher alone would leave a live session permanently unobservable.
    std::cerr << "Presentation status unavailable: " << error.what() << '\n';
    throw;
  }
}

struct AudioControlPoller {
  explicit AudioControlPoller(headunit::audio_control::Bridge* bridge) : audio_bridge(bridge) {}

  headunit::audio_control::Bridge* audio_bridge;
  std::chrono::steady_clock::time_point next_audio_control_at{};
  std::string audio_publication_error;

  void poll(const std::vector<Connection>& connections) {
    if (!audio_bridge || std::chrono::steady_clock::now() < next_audio_control_at) return;
    std::vector<std::shared_ptr<headunit::audio_control::Mailbox>> routes;
    for (const auto& connection : connections)
      if (connection.session_setup) {
        auto controls = connection.session_setup->media_controls();
        routes.insert(routes.end(), controls.begin(), controls.end());
      }
    const auto now = std::chrono::steady_clock::now();
    audio_bridge->poll(routes, headunit::audio_control::monotonic_time_ns());
    next_audio_control_at = now + std::chrono::milliseconds(100);
    if (audio_bridge->publication_error() != audio_publication_error) {
      audio_publication_error = audio_bridge->publication_error();
      if (!audio_publication_error.empty())
        std::cerr << "Audio control state: " << audio_publication_error << '\n';
    }
  }
};

// Publishes the current now-playing snapshot (and artwork) for the native page.
struct IapMediaPublisher {
  IapMediaPublisher(std::string path, std::string nonce)
      : iap_media_path(std::move(path)), launch_nonce(std::move(nonce)) {}

  std::string iap_media_path;
  std::string launch_nonce;
  std::string last_iap_media;
  std::shared_ptr<const std::vector<std::uint8_t>> last_iap_artwork;
  std::string last_iap_artwork_hash;
  std::chrono::steady_clock::time_point last_iap_media_at{};
  bool iap_media_write_failed = false;

  void publish(const std::vector<Connection>& connections) {
    if (iap_media_path.empty()) return;
    headunit::NowPlayingSnapshot current;
    headunit::NowPlayingPublicationScope scope{launch_nonce};
    for (const auto& connection : connections) {
      if (connection.iap_engine && connection.iap_engine->now_playing()) {
        auto candidate = connection.iap_engine->now_playing_snapshot();
        if (candidate.available) {
          current = std::move(candidate);
          scope.engine_generation = connection.iap_engine->diagnostic_generation();
          scope.connection_generation = connection.number;
          break;
        }
      }
    }
    if (current.artwork != last_iap_artwork) {
      last_iap_artwork_hash.clear();
      if (current.artwork) {
        try {
          const auto bytes = std::span(*current.artwork);
          const auto hash = sha256_hex(bytes);
          publish_iap_media(iap_media_path + ".art",
                            std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
          last_iap_artwork_hash = hash;
          last_iap_artwork = current.artwork;
        } catch (const std::exception& error) {
          std::cerr << "iAP artwork snapshot: " << error.what() << '\n';
        }
      } else
        last_iap_artwork.reset();
    }
    const auto content = headunit::now_playing_json(current, last_iap_artwork_hash, scope);
    const auto now = std::chrono::steady_clock::now();
    if (content != last_iap_media || now - last_iap_media_at >= std::chrono::seconds(1)) {
      try {
        publish_iap_media(iap_media_path, content);
        last_iap_media = content;
        last_iap_media_at = now;
        iap_media_write_failed = false;
      } catch (const std::exception& error) {
        if (!iap_media_write_failed) std::cerr << "iAP media snapshot: " << error.what() << '\n';
        iap_media_write_failed = true;
      }
    }
  }

  void remove() const {
    if (iap_media_path.empty()) return;
    unlink(iap_media_path.c_str());
    unlink((iap_media_path + ".art").c_str());
  }
};
}  // namespace

int main(int argc, char** argv) try {
  if (const auto result = headunit::audio_observation::File::collector_main(argc, argv))
    return *result;
  if (argc >= 2 && std::string_view(argv[1]) == "--check-software-mfi") {
    if (argc != 4)
      throw std::invalid_argument("Software MFi preflight requires key and certificate paths");
    headunit::SoftwareMfiAuthenticator authentication(argv[2], argv[3]);
    const std::array<std::uint8_t, 32> digest{};
    (void)authentication.sign_digest(digest);
    std::cout << "Software MFi identity consistent; phone acceptance unverified\n";
    return 0;
  }
  UltraRuntime ultra;
  const auto* presentation_path = std::getenv("HEADUNIT_PRESENTATION_STATUS_FILE");
  const auto* presentation_nonce = std::getenv("HEADUNIT_PRESENTATION_STATUS_NONCE");
  if (presentation_nonce) ultra.launch_nonce = presentation_nonce;
  if (bool(presentation_path) != bool(presentation_nonce))
    throw std::invalid_argument("presentation status path and nonce must be supplied together");
  std::unique_ptr<headunit::PresentationStatusPublisher> presentation_publisher;
  if (presentation_path)
    presentation_publisher = std::make_unique<headunit::PresentationStatusPublisher>(
        presentation_path, presentation_nonce);
  std::unique_ptr<headunit::NavigationPublisher> navigation_publisher;
  if (const auto* path = std::getenv("HEADUNIT_NAVIGATION_SOCKET")) {
    try {
      navigation_publisher = std::make_unique<headunit::NavigationPublisher>(path);
    } catch (const std::exception& error) {
      std::cerr << "Navigation publication unavailable: " << error.what() << '\n';
    }
  }
  if (argc < 4 || argc > 11 || argc == 10) {
    throw std::invalid_argument(
        "usage: headunit-control-probe PORT SECONDS PRIVATE_DIR "
        "[IDENTITY_FILE [PRIVATE_SETUP_SECRET_FILE [PRIVATE_KEY_PEM "
        "[PAIR_STORE_DIR [--mfi-auth [--iap2-wifi-config PATH]]]]]]");
  }
  const auto port = parse_number(argv[1], 65535, "port");
  const auto seconds = parse_number(argv[2], 3600, "duration", true);
  const std::string directory(argv[3]);
  check_private_directory(directory);
  const char* preview_environment = std::getenv("HEADUNIT_PREVIEW_SINK");
  const std::string preview_sink = preview_environment ? preview_environment : "none";
  if (preview_sink != "none" && preview_sink != "x11" && preview_sink != "wayland" &&
      preview_sink != "wayland-touch" && preview_sink != "wayland-gpu" && preview_sink != "fake")
    throw std::invalid_argument("invalid video preview sink");
  headunit::MainPreviewOptions main_preview;
  const auto* camera_request = std::getenv("HEADUNIT_STANDARD_CAMERA_CONTROL");
  const auto* camera_lease = std::getenv("HEADUNIT_STANDARD_CAMERA_LEASE");
  const auto* page_request = std::getenv("HEADUNIT_NATIVE_PAGE_CONTROL");
  const auto* page_lease = std::getenv("HEADUNIT_NATIVE_PAGE_SURFACE_LEASE");
  if (bool(page_request) != bool(page_lease))
    throw std::invalid_argument("native page request and lease must be supplied together");
  if (page_request && (camera_request || camera_lease))
    throw std::invalid_argument("native page and camera ownership cannot be combined");
  if (preview_sink != "wayland-gpu" && page_request)
    throw std::invalid_argument("native page requires the owned Standard presenter");
  if (bool(camera_request) != bool(camera_lease))
    throw std::invalid_argument("Standard camera request and lease must be supplied together");
  if (preview_sink != "wayland-gpu" && camera_request)
    throw std::invalid_argument("Standard camera requires the owned Standard presenter");
  if (preview_sink == "wayland-gpu") {
#ifndef HEADUNIT_HAVE_STANDARD_GPU
    throw std::invalid_argument("owned Standard GPU preview is not built");
#else
    // Both capabilities may be available. Only the actual phone negotiation
    // admits a presentation owner; neither is started by configuration alone.
#endif
  }
  const char* decoder_environment = std::getenv("HEADUNIT_PREVIEW_DECODER");
  const std::string preview_decoder = decoder_environment ? decoder_environment : "v4l2h264dec";
  if (preview_decoder != "v4l2h264dec" && preview_decoder != "avdec_h264")
    throw std::invalid_argument("invalid video preview decoder");
  if (preview_sink != "none")
    std::cout << "video preview decoder=" << preview_decoder << '\n' << std::flush;
  std::optional<std::vector<std::uint8_t>> info_body;
  std::vector<std::uint8_t> standard_info_body;
  auto displays = headunit::default_receiver_displays();
  if (const char* config = std::getenv("HEADUNIT_DISPLAY_CONFIG")) {
    displays = headunit::load_display_configuration(config);
  } else {
    // Legacy command-line diagnostics only. Configured runs ignore these.
    if (const char* width = std::getenv("HEADUNIT_MAIN_WIDTH"))
      displays[0].size.width = parse_number(width, 8192, "main display width");
    if (const char* height = std::getenv("HEADUNIT_MAIN_HEIGHT"))
      displays[0].size.height = parse_number(height, 4320, "main display height");
  }
  auto standard_displays = displays;
  if (const char* config = std::getenv("HEADUNIT_STANDARD_DISPLAY_CONFIG"))
    standard_displays = headunit::load_display_configuration(config);
  const auto main_display = displays[0].size;
  const auto cluster_display = displays[1].size;
  if (preview_sink == "wayland-gpu") {
#ifdef HEADUNIT_HAVE_STANDARD_GPU
    if ((!std::getenv("HEADUNIT_STANDARD_DISPLAY_CONFIG") &&
         !std::getenv("HEADUNIT_DISPLAY_CONFIG")) ||
        standard_displays[0].wayland_display.empty() || standard_displays[0].output_name.empty() ||
        standard_displays[0].size.width > 4096 || standard_displays[0].size.height > 2160)
      throw std::invalid_argument(
          "owned Standard preview requires an explicit bounded main display/output");
    main_preview.wayland_display = standard_displays[0].wayland_display;
    main_preview.output_name = standard_displays[0].output_name;
    if (page_request)
      main_preview.native_page_control =
          std::make_shared<headunit::MainSurfaceControl>(page_request, page_lease);
    if (camera_request) {
      main_preview.camera_control =
          std::make_shared<headunit::camera::ActivationControl>(camera_request);
      headunit::camera::CameraSurfaceLease::validate(camera_lease);
      // Startup validation rejects a shared endpoint, including hard-link aliases.
      struct stat request_stat{}, lease_stat{};
      if (std::string_view(camera_request) == camera_lease ||
          (::lstat(camera_request, &request_stat) == 0 && ::lstat(camera_lease, &lease_stat) == 0 &&
           request_stat.st_dev == lease_stat.st_dev && request_stat.st_ino == lease_stat.st_ino))
        throw std::invalid_argument("Standard camera request and lease must be distinct");
      main_preview.camera_lease_file = camera_lease;
    }
#endif
  }
  const char* audio_setting = std::getenv("HEADUNIT_AUDIO_SINK");
  const std::string audio_sink = audio_setting ? audio_setting : "";
  const char* audio_device_setting = std::getenv("HEADUNIT_AUDIO_DEVICE");
  const std::string audio_device = audio_device_setting ? audio_device_setting : "";
  if (!audio_sink.empty() && audio_sink != "fakesink" && audio_sink != "alsasink" &&
      audio_sink != "pulsesink")
    throw std::invalid_argument("invalid audio sink");
  if (audio_sink.empty() && !audio_device.empty())
    throw std::invalid_argument("audio device requires an audio sink");
  if (!audio_sink.empty()) headunit::AudioOutput::validate_selection(audio_sink, audio_device);
  const char* microphone_setting = std::getenv("HEADUNIT_MICROPHONE_SOURCE");
  const char* microphone_device_setting = std::getenv("HEADUNIT_MICROPHONE_DEVICE");
  const std::string microphone_source = microphone_setting ? microphone_setting : "";
  const std::string microphone_device = microphone_device_setting ? microphone_device_setting : "";
  if (!microphone_source.empty()) {
    if (audio_sink.empty())
      throw std::invalid_argument("microphone requires configured audio output");
    headunit::AudioMicrophone::validate_selection(microphone_source, microphone_device);
  } else if (!microphone_device.empty())
    throw std::invalid_argument("microphone device requires a source");
  headunit::audio_control::Level audio_level;
  const char* audio_volume = std::getenv("HEADUNIT_AUDIO_VOLUME_PERCENT");
  const char* audio_muted = std::getenv("HEADUNIT_AUDIO_MUTED");
  const char* audio_control_path = std::getenv("HEADUNIT_AUDIO_CONTROL_FILE");
  const char* audio_state_path = std::getenv("HEADUNIT_AUDIO_STATE_FILE");
  if ((audio_volume || audio_muted || audio_control_path || audio_state_path) && audio_sink.empty())
    throw std::invalid_argument("head-unit audio controls require an audio sink");
  if (bool(audio_control_path) != bool(audio_state_path))
    throw std::invalid_argument("audio control/state paths must be supplied together");
  if (audio_volume) {
    const auto text = std::string_view(audio_volume);
    if (text.empty() || text.size() > 3 ||
        !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
      throw std::invalid_argument("invalid audio volume percent");
    audio_level.volume_percent = parse_number(audio_volume, 100, "audio volume percent", true);
  }
  if (audio_muted) {
    if (std::string_view(audio_muted) != "0" && std::string_view(audio_muted) != "1")
      throw std::invalid_argument("audio mute must be 0 or 1");
    audio_level.muted = std::string_view(audio_muted) == "1";
  }
  std::unique_ptr<headunit::audio_control::Bridge> audio_bridge;
  if (audio_control_path)
    audio_bridge = std::make_unique<headunit::audio_control::Bridge>(
        audio_control_path, audio_state_path, unsigned(getuid()));
  for (const auto& d : displays)
    std::cout << "display panel=" << d.panel_name << " pixels=" << d.size.width << 'x'
              << d.size.height << " fps=" << d.size.max_fps << " physicalMm=" << d.size.width_mm
              << 'x' << d.size.height_mm << '\n'
              << std::flush;
  if (const char* limited = std::getenv("HEADUNIT_LIMITED_UI")) {
    const auto policy = headunit::parse_limited_ui_policy(limited);
    if (!policy) throw std::invalid_argument("invalid HEADUNIT_LIMITED_UI");
    headunit::set_receiver_limited_ui(policy);
    std::cout << "driving restrictions follow vehicle speed\n" << std::flush;
  }
  if (const char* multitouch = std::getenv("HEADUNIT_MULTITOUCH")) {
    if (std::string_view(multitouch) != "1")
      throw std::invalid_argument("invalid HEADUNIT_MULTITOUCH");
    headunit::set_multitouch(true);
    std::cout << "two-finger touch declared\n" << std::flush;
  }
  const char* oem_icon_setting = std::getenv("HEADUNIT_OEM_ICON");
  if (oem_icon_setting && std::string_view(oem_icon_setting) != "1")
    throw std::invalid_argument("invalid HEADUNIT_OEM_ICON");
  const bool oem_icon = oem_icon_setting != nullptr;
  std::optional<headunit::AirplayIdentity> identity;
  if (argc >= 5) {
    identity = headunit::load_airplay_identity(argv[4]);
    standard_info_body =
        headunit::make_airplay_info(*identity, standard_displays[0].size, standard_displays[1].size,
                                    false, standard_displays[0].panel_name,
                                    standard_displays[1].panel_name, !audio_sink.empty(), oem_icon);
    info_body = headunit::make_airplay_info(*identity, main_display, cluster_display, false,
                                            displays[0].panel_name, displays[1].panel_name,
                                            !audio_sink.empty(), oem_icon);
  }
  if (const char* night = std::getenv("HEADUNIT_NIGHT_MODE")) {
    const auto policy = headunit::parse_night_mode_policy(night);
    if (!policy) throw std::invalid_argument("invalid HEADUNIT_NIGHT_MODE");
    headunit::set_receiver_night_mode(policy);
    std::cout << "night mode policy configured\n" << std::flush;
  }
  if (const char* profile = std::getenv("HEADUNIT_ULTRA_INSTALLED_UI")) {
#ifdef HEADUNIT_HAVE_LIVE_UI
    if (!identity) throw std::invalid_argument("Ultra UI requires receiver identity");
    ultra.installed_ui = headunit::theme::script::InstalledUi::open(profile);
    std::cout << "verified installed Ultra UI consumer attached\n" << std::flush;
#else
    (void)profile;
    throw std::invalid_argument("Ultra UI requires the GPU build");
#endif
  }
  if (const char* native = std::getenv("HEADUNIT_ULTRA_NATIVE_VIDEO")) {
#ifdef HEADUNIT_HAVE_LIVE_UI
    if (std::string_view(native) != "1" || !ultra.installed_ui)
      throw std::invalid_argument("native Ultra video requires the installed UI consumer");
    ultra.native_video = true;
#else
    (void)native;
    throw std::invalid_argument("native Ultra video requires the GPU/native-video build");
#endif
  }
  if (const char* negotiate = std::getenv("HEADUNIT_ULTRA_NEGOTIATE")) {
#ifdef HEADUNIT_HAVE_LIVE_UI
    if (std::string_view(negotiate) != "1" || !ultra.native_video || !ultra.installed_ui ||
        argc != 11)
      throw std::invalid_argument(
          "Ultra negotiation requires native inputs, installed UI and Bluetooth/MFi");
    const auto& asset = ultra.installed_ui->asset_info();
    if (asset.minimum_sdk != "19.0" || asset.version == 0 || asset.version > 0x7fffffff)
      throw std::invalid_argument("unsupported reference asset protocol version");
    ultra.negotiate = true;
    info_body = headunit::make_airplay_info(*identity, main_display, cluster_display, true,
                                            displays[0].panel_name, displays[1].panel_name,
                                            !audio_sink.empty(), oem_icon);
    std::cout << "experimental installed Ultra negotiation; configured panels\n" << std::flush;
#else
    (void)negotiate;
    throw std::invalid_argument("Ultra negotiation requires native GPU build");
#endif
  }
  std::optional<headunit::theme::vehicle::BenchProfile> vehicle_profile;
  std::shared_ptr<const headunit::theme::vehicle::Configuration> vehicle_configuration;
  std::shared_ptr<headunit::theme::vehicle::Provider> vehicle_provider;
  headunit::theme::vehicle::Transport::WriteObserver vehicle_observer;
  const auto capability_path = [](const char* key) -> std::optional<std::filesystem::path> {
    const auto* value = std::getenv(key);
    return value && *value ? std::optional<std::filesystem::path>{value} : std::nullopt;
  };
  const auto climate_config = capability_path("HEADUNIT_CLIMATE_CONFIG");
  const auto settings_config = capability_path("HEADUNIT_CAR_SETTINGS_CONFIG");
  const auto camera_config = capability_path("HEADUNIT_CAMERA_STATE_CONFIG");
  if (climate_config || settings_config || camera_config) {
    const char* profile = std::getenv("HEADUNIT_VEHICLE_STATE_PROFILE");
    if (profile && std::string_view(profile) != "none")
      throw std::invalid_argument("choose capability files or a legacy vehicle profile");
    if (!info_body)
      throw std::invalid_argument("vehicle capability files require receiver identity");
#ifdef HEADUNIT_HAVE_LIVE_UI
    if (!ultra.negotiate)
      throw std::invalid_argument("vehicle capability files require native Ultra negotiation");
#else
    throw std::invalid_argument("vehicle capability files require native Ultra build");
#endif
    const char* backend = std::getenv("HEADUNIT_VEHICLE_STATE_BACKEND");
    if (!backend || std::string_view(backend) != "simulation")
      throw std::invalid_argument(
          "capability files need an explicit vehicle-state backend; only simulation is installed");
    vehicle_configuration = std::make_shared<headunit::theme::vehicle::Configuration>(
        headunit::theme::vehicle::load_configuration(climate_config, settings_config,
                                                     camera_config));
    if (vehicle_configuration->content_request) {
      headunit::theme::vehicle::validate_content_request_endpoint(
          vehicle_configuration->content_request->control_file);
#ifdef HEADUNIT_HAVE_LIVE_UI
      if (!ultra.installed_ui)
        throw std::invalid_argument("content request needs the signed cluster scene");
      ultra.installed_ui->validate_content_request(*vehicle_configuration->content_request);
#else
      throw std::invalid_argument("content request needs the signed cluster scene");
#endif
    }
    if (vehicle_configuration->camera) {
#ifdef HEADUNIT_HAVE_LIVE_UI
      const auto& source = ultra.installed_ui->camera_configuration();
      const auto& capability = *vehicle_configuration->camera;
      if (!source || !source->enabled ||
          source->stream_identifier != capability.stream_identifier ||
          capability.max_age_ms <= source->maximum_age_ms)
        throw std::invalid_argument(
            "camera capability must match the installed signed input and outlive its frame-age "
            "limit");
#else
      throw std::invalid_argument("camera capability requires the installed signed camera input");
#endif
    }
    vehicle_provider = headunit::theme::vehicle::simulated_provider(*vehicle_configuration);
    if (const auto* trace = std::getenv("HEADUNIT_VEHICLE_TRACE_WRITES");
        trace && std::string_view(trace) == "1") {
      vehicle_observer = [configuration = vehicle_configuration](const auto& request,
                                                                 const auto& response) {
        const auto scalar = [](const headunit::theme::opack::Value& value) {
          if (const auto* p = std::get_if<bool>(&value.data))
            return std::string(*p ? "bool(true)" : "bool(false)");
          if (const auto* p = std::get_if<std::int64_t>(&value.data))
            return "integer(" + std::to_string(*p) + ")";
          if (const auto* p = std::get_if<double>(&value.data))
            return "real(" + std::to_string(*p) + ")";
          return std::string("non-scalar");
        };
        std::int64_t iid = -1;
        std::string requested = "missing", applied = "missing";
        const auto first = [](const auto& values) -> const headunit::theme::opack::Dictionary* {
          return values ? std::get_if<headunit::theme::opack::Dictionary>(&values->data) : nullptr;
        };
        if (const auto* values = first(request.values); values && values->size() == 1) {
          if (const auto* key = std::get_if<std::int64_t>(&values->front().first.data)) iid = *key;
          requested = scalar(values->front().second);
        }
        if (const auto* values = first(response.values); values && values->size() == 1)
          applied = scalar(values->front().second);
        std::string binding = "unknown";
        for (const auto& s : configuration->signals)
          if (s.policy.iid == iid) binding = s.key;
        std::cout << "vehicle write iid=" << iid << " binding=" << binding
                  << " requested=" << requested << " applied=" << applied
                  << " rejected=" << bool(response.error || response.errors) << '\n'
                  << std::flush;
      };
    }
    headunit::theme::vehicle::ConfiguredModel model(*vehicle_configuration, vehicle_provider);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    model.refresh(now);
    (void)headunit::theme::vehicle::add_server_info(*info_body, model.service(), now);
    std::cout << "vehicle capability files loaded: backend=simulation signals="
              << vehicle_configuration->signals.size()
              << " experimental=" << vehicle_configuration->experimental << '\n'
              << std::flush;
  }
  if (const char* profile = std::getenv("HEADUNIT_VEHICLE_STATE_PROFILE");
      profile && std::string_view(profile) != "none") {
    if (!info_body) throw std::invalid_argument("vehicle-state profile requires receiver identity");
    vehicle_profile = headunit::theme::vehicle::bench_profile(profile);
#ifdef HEADUNIT_HAVE_LIVE_UI
    if (*vehicle_profile != headunit::theme::vehicle::BenchProfile::empty && !ultra.negotiate)
      throw std::invalid_argument("Climate/settings bench requires native Ultra negotiation");
#else
    if (*vehicle_profile != headunit::theme::vehicle::BenchProfile::empty)
      throw std::invalid_argument("Climate/settings bench requires native Ultra build");
#endif
    headunit::theme::vehicle::Bench bench(*vehicle_profile);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    bench.refresh(now);
    info_body = headunit::theme::vehicle::add_server_info(*info_body, bench.service(), now);
    // Standalone empty-profile diagnostics retain their explicit service after
    // Standard SETUP. Dual-mode Standard still omits Ultra vehicle resources.
    if (!ultra.negotiate)
      standard_info_body =
          headunit::theme::vehicle::add_server_info(standard_info_body, bench.service(), now);
    std::cout << "vehicle-state research profile=" << profile
              << " (memory-only; phone compatibility unverified)\n"
              << std::flush;
  }
  auto setup_secret = argc >= 6 ? read_setup_secret(argv[5]) : nullptr;
  std::optional<std::string> private_key_path;
  if (argc >= 7) {
    private_key_path = argv[6];
    const headunit::HapEd25519Identity pairing_key(*private_key_path);
    const auto actual = pairing_key.public_key();
    for (std::size_t index = 0; index < actual.size(); ++index) {
      const auto pair = identity->public_key_hex.substr(index * 2, 2);
      const auto expected = std::stoul(pair, nullptr, 16);
      if (actual[index] != expected) {
        throw std::runtime_error("pairing key does not match advertised identity");
      }
    }
  }
  std::unique_ptr<headunit::HapPairStore> pair_store;
  if (argc >= 8) pair_store = std::make_unique<headunit::HapPairStore>(argv[7]);
  std::unique_ptr<headunit::LinuxMfiBus> mfi_bus;
  std::unique_ptr<headunit::MfiAuthenticator> mfi_auth;
  if (argc >= 9) {
    if (std::string_view(argv[8]) != "--mfi-auth") {
      throw std::invalid_argument("unknown control probe option");
    }
    const auto* selected = std::getenv("HEADUNIT_MFI_BACKEND");
    const std::string_view backend = selected ? selected : "hardware";
    const auto* key = std::getenv("HEADUNIT_MFI_PRIVATE_KEY");
    const auto* certificate = std::getenv("HEADUNIT_MFI_CERTIFICATE");
    if (backend == "hardware") {
      if (key || certificate)
        throw std::invalid_argument("Software MFi inputs require explicit software backend");
      const auto* device = std::getenv("HEADUNIT_MFI_DEVICE");
      const auto* address = std::getenv("HEADUNIT_MFI_ADDRESS");
      const auto mfi_address = address ? parse_number(address, 119, "MFi address") : 16;
      if (mfi_address < 8) throw std::invalid_argument("reserved I2C address");
      mfi_bus =
          std::make_unique<headunit::LinuxMfiBus>(device ? device : "/dev/i2c-1", mfi_address);
      mfi_auth = std::make_unique<headunit::MfiCoprocessor>(*mfi_bus);
    } else if (backend == "software") {
      if (!key || !certificate)
        throw std::invalid_argument("Software MFi requires private key and certificate paths");
      mfi_auth = std::make_unique<headunit::SoftwareMfiAuthenticator>(key, certificate);
    } else {
      throw std::invalid_argument("Unknown MFi authentication backend");
    }
    std::cout << "mfi-backend=" << backend << '\n';
  }
  std::unique_ptr<headunit::BluetoothIapProfile> bluetooth;
  if (argc == 11) {
    if (std::string_view(argv[9]) != "--iap2-wifi-config") {
      throw std::invalid_argument("unknown control probe option");
    }
    const auto wifi = headunit::load_private_iap_wifi_config(argv[10]);
    std::optional<headunit::Iap2CarplayStart> start;
#ifdef HEADUNIT_HAVE_LIVE_UI
    if (ultra.negotiate) {
      const auto& asset = ultra.installed_ui->asset_info();
      start =
          headunit::Iap2CarplayStart{*identity, wifi, "10.10.0.1", port,
                                     headunit::Iap2ThemeAsset{asset.identifier, asset.minimum_sdk,
                                                              std::uint32_t(asset.version)}};
    }
#endif
    auto iap_identity =
        identity ? headunit::make_iap2_identity(*identity) : headunit::Iap2Identity{};
    iap_identity.supports_now_playing = std::getenv("HEADUNIT_IAP_MEDIA_STATE") != nullptr;
    if (const auto* enabled = std::getenv("HEADUNIT_ROUTE_GUIDANCE");
        enabled && std::string_view(enabled) == "1") {
      headunit::RouteGuidanceConfig config;
      if (const auto* id = std::getenv("HEADUNIT_ROUTE_COMPONENT_ID"))
        config.component_id = parse_number(id, 65535, "route component ID");
      if (const auto* name = std::getenv("HEADUNIT_ROUTE_COMPONENT_NAME")) config.name = name;
      (void)headunit::route_guidance_component(config);
      iap_identity.route_guidance = config;
    }
    bluetooth = std::make_unique<headunit::BluetoothIapProfile>(
        *mfi_auth, wifi, start, iap_identity,
        std::getenv("HEADUNIT_SELECTED_DEVICE") ? std::getenv("HEADUNIT_SELECTED_DEVICE") : "");
  }
  const char* standard_display = std::getenv("HEADUNIT_STANDARD_CLUSTER_DISPLAY");
  const std::string standard_cluster_wayland = standard_display ? standard_display : "";
  std::unique_ptr<headunit::audio_observation::File> audio_trace;
  if (const auto* path = std::getenv("HEADUNIT_AUDIO_OBSERVATION_FILE"))
    audio_trace = std::make_unique<headunit::audio_observation::File>(path, argv[0]);
  const ReceiverContext context{directory,
                                preview_sink,
                                preview_decoder,
                                standard_cluster_wayland,
                                main_preview,
                                audio_sink,
                                audio_device,
                                microphone_source,
                                microphone_device,
                                audio_level,
                                displays,
                                standard_displays,
                                info_body,
                                standard_info_body,
                                setup_secret.get(),
                                identity ? &*identity : nullptr,
                                private_key_path ? &*private_key_path : nullptr,
                                pair_store.get(),
                                mfi_auth.get(),
                                bluetooth.get(),
                                ultra,
                                vehicle_profile,
                                vehicle_configuration,
                                vehicle_provider,
                                vehicle_observer,
                                audio_trace ? audio_trace->capture() : nullptr};

  const int server = open_listener(port);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::cout << "control listener ready on port " << port << '\n' << std::flush;
  headunit::ConnectionDiagnostics startup(headunit::ConnectionDiagnostics::Scope::startup, 0);
  startup.mark(headunit::ConnectionDiagnostics::Stage::control_listening);
  if (bluetooth) {
    bluetooth->start();
    startup.mark(headunit::ConnectionDiagnostics::Stage::iap_profile_ready);
  }

  std::vector<Connection> connections;
  AudioControlPoller audio_controls{audio_bridge.get()};
  IapMediaPublisher iap_media{
      std::getenv("HEADUNIT_IAP_MEDIA_STATE") ? std::getenv("HEADUNIT_IAP_MEDIA_STATE") : "",
      ultra.launch_nonce};
  unsigned next_number = 1;
  std::uint64_t presentation_connection = 0;
  std::uint64_t navigation_generation = 0;
  auto next_navigation_at = headunit::Clock::time_point{};
  const auto withdraw_presentation = [&](headunit::PresentationState state) {
    if (presentation_publisher) try {
        presentation_publisher->update(state, presentation_connection, false);
      } catch (const std::exception& error) {
        std::cerr << "Presentation status unavailable: " << error.what() << '\n';
        throw;
      }
  };
  struct StopPresentation {
    const decltype(withdraw_presentation)& withdraw;
    ~StopPresentation() {
      // A terminal publication is best effort during stack unwinding. The
      // original failure must still leave through the normal failure exit.
      try {
        withdraw(headunit::PresentationState::stopped);
      } catch (...) {
      }
    }
  } stop_presentation{withdraw_presentation};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  std::exception_ptr receiver_failure;
  try {
    while (!stop_requested && (!seconds || std::chrono::steady_clock::now() < deadline)) {
      ultra.reap();
      const bool ui_capacity = ultra.has_capacity();
      if (bluetooth) bluetooth->pump();
      std::vector<pollfd> descriptors{
          {server,
           static_cast<short>(connections.size() < max_connections && ui_capacity ? POLLIN : 0),
           0}};
      for (const auto& connection : connections) {
        descriptors.push_back({connection.socket, POLLIN, 0});
      }
      for (const auto& connection : connections)
        if (connection.session_setup)
          connection.session_setup->append_poll_descriptors(descriptors);
      // Append after control sockets: descriptors[index + 1] remains their
      // stable mapping. Input wakes the loop; its owner dispatches below.
      for (const auto& connection : connections)
        if (connection.video_output)
          connection.video_output->append_input_poll_descriptor(descriptors);
#ifdef HEADUNIT_HAVE_LIVE_UI
      // Independent worker mailbox wakeup; append after stable control indices.
      // Consumed by drain(), including backpressure, so readiness never spins.
      for (const auto& connection : connections)
        if (connection.ultra_touches)
          descriptors.push_back({connection.ultra_touches->wake_fd(), POLLIN, 0});
#endif
      if (bluetooth) bluetooth->append_poll_descriptors(descriptors);
      const int ready = poll(descriptors.data(), descriptors.size(),
                             connections.empty() ? (bluetooth ? 50 : 100) : 10);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) throw std::runtime_error("control listener poll failed");
      if (bluetooth) {
        bluetooth->pump();
        bluetooth->maintain_connection(connections.empty());
      }

      // Pump sessions first; failures are retired below in the socket pass.
      std::vector<bool> failed(connections.size(), false);
      const auto* receiver_identity = identity ? &*identity : nullptr;
      for (std::size_t index = 0; index < connections.size(); ++index) {
        auto& connection = connections[index];
        if (connection.session_setup &&
            !pump_session(connection, bluetooth.get(), ultra, receiver_identity))
          failed[index] = true;
      }

      // Process existing sockets before accepting new ones so descriptor indices
      // remain valid for this poll result.
      for (std::size_t index = connections.size(); index > 0; --index) {
        auto& connection = connections[index - 1];
        if (failed[index - 1]) {
          if (connection.number == presentation_connection)
            withdraw_presentation(headunit::PresentationState::fault);
          close_connection(connection, bluetooth.get(), ultra,
                           headunit::ConnectionDiagnostics::ControlRetirement::owner_failure);
          connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(index - 1));
          continue;
        }
        const auto events = descriptors[index].revents;
        if (!events) continue;
        std::array<std::uint8_t, 4096> buffer{};
        const auto count = recv(connection.socket, buffer.data(), buffer.size(), 0);
        using Retirement = headunit::ConnectionDiagnostics::ControlRetirement;
        auto reason = count == 0 ? Retirement::peer_eof : Retirement::socket_error;
        if (count > 0) {
          const auto available = max_bytes_per_connection - connection.bytes;
          const auto stored = std::min<std::size_t>(static_cast<std::size_t>(count), available);
          if (stored) write_all(connection.capture, buffer.data(), stored);
          connection.bytes += stored;
          try {
            if (connection.framing) {
              const auto decrypted =
                  connection.framing->receive({buffer.data(), static_cast<std::size_t>(count)});
              connection.request_buffer.append(reinterpret_cast<const char*>(decrypted.data()),
                                               decrypted.size());
            } else {
              connection.request_buffer.append(reinterpret_cast<const char*>(buffer.data()),
                                               static_cast<std::size_t>(count));
            }
            const auto outcome = process_requests(connection, context);
            // The private capture has a fixed size; the live control session does
            // not end when that diagnostic limit is reached.
            if (!outcome) continue;
            reason = *outcome;
          } catch (const std::exception&) {
            connection.diagnostics.mark(ConnectionStage::failed);
            reason = Retirement::owner_failure;
          }
        } else if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
          continue;
        }
        if (connection.number == presentation_connection)
          withdraw_presentation(headunit::PresentationState::disconnected);
        close_connection(connection, bluetooth.get(), ultra, reason);
        connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(index - 1));
      }

      if (descriptors[0].revents & POLLIN && connections.size() < max_connections &&
          !accept_connection(server, directory, connections, next_number))
        continue;
      if (navigation_publisher && headunit::Clock::now() >= next_navigation_at) {
        const auto now = headunit::Clock::now();
        next_navigation_at = now + std::chrono::milliseconds(250);
        // Reuse the sole live phone engine in either mode. Two connections may
        // refer to that same engine; conflicting engines have no HUD authority.
        std::shared_ptr<headunit::Iap2SessionEngine> source;
        bool ambiguous = false;
        const auto consider = [&](const std::shared_ptr<headunit::Iap2SessionEngine>& engine) {
          if (!engine || !engine->established() || !engine->diagnostic_generation()) return;
          if (source && source != engine)
            ambiguous = true;
          else
            source = engine;
        };
        if (bluetooth) consider(bluetooth->engine());
        for (const auto& connection : connections)
          consider(connection.iap_engine);
        headunit::NavigationSnapshot navigation;
        if (!ambiguous && source) {
          navigation_generation = source->diagnostic_generation();
          if (source->navigation()) navigation = source->navigation()->snapshot(now);
        }
        try {
          navigation_publisher->publish(navigation, navigation_generation, now);
        } catch (const std::exception& error) {
          std::cerr << "Navigation publication unavailable: " << error.what() << '\n';
          navigation_publisher.reset();
        }
      }
      publish_presentation(presentation_publisher, connections, presentation_connection,
                           identity ? &*identity : nullptr);
      audio_controls.poll(connections);
      if (audio_trace) (void)audio_trace->poll();
      iap_media.publish(connections);
    }
    withdraw_presentation(headunit::PresentationState::stopped);
  } catch (...) {
    // Complete the same explicit connection/Ultra retirement as normal exit
    // before reporting the original mandatory-publication or loop failure.
    receiver_failure = std::current_exception();
  }
  for (auto& connection : connections)
    close_connection(connection, bluetooth.get(), ultra,
                     headunit::ConnectionDiagnostics::ControlRetirement::shutdown);
  if (audio_bridge) audio_bridge->poll({}, headunit::audio_control::monotonic_time_ns());
  if (audio_trace) (void)audio_trace->finish();
  iap_media.remove();
  close(server);
  if (receiver_failure) std::rethrow_exception(receiver_failure);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
