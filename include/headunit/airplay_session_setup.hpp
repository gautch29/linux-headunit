// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/airplay_video_stream.hpp"
#include "headunit/airplay_iap_stream.hpp"
#include "headunit/airplay_data_stream.hpp"
#include "headunit/airplay_event_channel.hpp"
#include "headunit/airplay_clock.hpp"
#include "headunit/airplay_timing.hpp"
#include "headunit/airplay_command.hpp"
#include "headunit/control_request.hpp"
#include "headunit/carplay_media_stream.hpp"
#include "headunit/iap2_session_engine.hpp"
#include "headunit/presentation_status.hpp"
#include "headunit/connection_diagnostics.hpp"
#include "headunit/pending_bytes.hpp"

#include <optional>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <span>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <memory>
#include <netinet/in.h>
#include <poll.h>

namespace headunit {

// Owns the sockets advertised by encrypted AirPlay SETUP replies. This is a
// bounded transport diagnostic with optional iAP2/event-channel attachment;
// it does not decode codecs or render video.
class AirplaySessionSetup {
 public:
  static constexpr std::size_t active_stream_limit = 12;
  // Retired IDs must remain reserved, but must not consume active stream slots.
  // Exhaustion requires a fresh control session rather than nonce/key reuse.
  static constexpr std::size_t stream_history_limit = 4096;
  AirplaySessionSetup() = default;
  ~AirplaySessionSetup();
  AirplaySessionSetup(const AirplaySessionSetup&) = delete;
  AirplaySessionSetup& operator=(const AirplaySessionSetup&) = delete;

  void set_peer(const sockaddr_in6& peer);
  void set_secret(std::span<const std::uint8_t> shared);
  // Optional borrowed trace is used synchronously; no diagnostic authority or
  // pointer survives this call. setup_features result: UI=1, HEVC=2, vehicle=4.
  std::vector<std::uint8_t> respond(std::span<const std::uint8_t> request,
                                    ConnectionDiagnostics* diagnostics = nullptr,
                                    std::uint64_t link_generation = 0);
  bool start_record();
  void enable_native_ultra();  // Before SETUP; requires attached UI/video consumers.
  // Capability/feature agreement alone is not actual UI-service selection.
  // No agreed UI-sync/HEVC means Standard; an agreed pair remains unknown
  // until the phone successfully negotiates the registered UI service.
  std::optional<CarplayMode> negotiated_mode() const { return negotiated_mode_; }
  void enable_vehicle_state();  // Before SETUP; both vehicle service consumers attached.
  void set_audio_sink(std::string factory, std::string device = {},
                      audio_control::Level initial_level = {});  // Before SETUP; shared output.
  void set_microphone(std::string factory, std::string device = {});
  void set_audio_observation(std::shared_ptr<audio_observation::Capture>);
  std::vector<MediaStreamStatus> media_status() const;
  std::vector<std::shared_ptr<audio_control::Mailbox>> media_controls() const;
  enum class AudioFocusResult { unhandled, accepted, unavailable };
  // /command ACK means accepted by the current audio worker, never playback.
  AudioFocusResult request_audio_focus(std::span<const std::uint8_t> command);
  // Both authenticated control sockets use this dispatcher. A modesChanged
  // 200 confirms observed state ingestion. Existing media permission is queued
  // to its worker; it is not applied screen focus or an audible receipt.
  enum class ControlChannel { main, event };
  unsigned dispatch_command(std::span<const std::uint8_t> command,
                            ControlChannel channel = ControlChannel::main);
  unsigned dispatch_event_request(const AirplayEventMessage& request);
  const AirplayModesState& modes() const { return modes_; }
  struct CommandDiagnostics {
    std::array<std::uint64_t, static_cast<std::size_t>(AirplayCommandKind::count)> by_kind{};
    std::uint64_t accepted = 0, unavailable = 0, unsupported = 0, malformed = 0;
    std::uint64_t unsupported_event_routes = 0;
  };
  CommandDiagnostics command_diagnostics() const { return command_diagnostics_; }
  const ProtocolCoverage& command_coverage() const noexcept { return command_coverage_; }
  const ProtocolCoverage& event_route_coverage() const noexcept { return event_route_coverage_; }
  // Register only services with an attached consumer. Does not advertise features.
  void register_data_service(const std::string& service_uuid);
  enum class ServiceState { negotiated, connected, closed };
  struct ServiceStatus {
    std::uint64_t stream_id;
    std::string service_uuid;
    ServiceState state;
  };
  // Owner-thread snapshot, including closed IDs. A consumer must stop on closed
  // even when no message arrived, and create fresh VMs for a replacement ID.
  std::vector<ServiceStatus> service_status() const;
  struct ServiceMessage {
    std::uint64_t stream_id;
    std::string service_uuid;
    DataMessage message;
  };
  std::vector<ServiceMessage> take_service_messages();
  // Deliver in stream order without removing a message until the consumer
  // accepts it. Callback must not mutate/reenter this session.
  std::size_t deliver_service_messages(std::uint64_t stream_id,
                                       const std::function<bool(const DataMessage&)>& accept,
                                       std::size_t limit = 32);
  bool pause_service_input(std::uint64_t stream_id, bool paused);
  bool send_service_message(std::uint64_t stream_id, const DataMessage& message);
  bool close_service(std::uint64_t stream_id);
  void teardown_services(std::span<const std::uint8_t> request);
  using VideoSink = std::function<void(std::uint64_t, const AirplayVideoPacket&)>;
  void set_video_sink(VideoSink sink) { video_sink_ = std::move(sink); }
  // Optional reliable owner-thread consumer, configured before stream SETUP.
  // False must leave the packet intact; true may move it. No reentry allowed.
  // Uses bounded per-screen TCP backpressure, never drops compressed deltas.
  using VideoConsumer = std::function<bool(std::uint64_t, AirplayVideoPacket&)>;
  void set_video_consumer(VideoConsumer consumer);
  struct VideoStatus {
    std::uint64_t type;
    ServiceState state;
    std::size_t retained_packets, retained_bytes;
    std::uint64_t connection_id;
  };
  std::vector<VideoStatus> video_status() const;
  std::optional<std::uint64_t> video_connection_id(std::uint64_t type) const;
  void drain();
  void append_poll_descriptors(std::vector<pollfd>& descriptors) const;
  std::size_t accepted_connections() const { return accepted_connections_; }
  std::size_t observed_bytes() const { return observed_bytes_; }
  std::size_t timing_replies() const { return timing_replies_; }
  std::optional<AirplayClockSample> timing_sample() const;
  AirplayClock::Diagnostics timing_diagnostics() const { return timing_clock_.diagnostics(); }
  bool timing_failed() const { return timing_clock_.failed(); }
  bool recording() const { return recording_; }
  std::size_t video_frames() const;
  std::size_t video_configs() const;
  std::size_t iap_messages() const;
  std::vector<std::vector<std::uint8_t>> take_iap_messages();
  bool send_iap_message(std::span<const std::uint8_t> message);
  bool show_cluster_map(const std::string& display_uuid);
  // RTSP FLUSH for main/media audio (not type-101 alternate guidance). Returns
  // the number of stream workers that accepted the request.
  std::size_t flush_audio(std::optional<std::uint16_t> resume_seq);
  // Queues setNightMode after RECORD. True is queueing, not phone acceptance.
  bool send_night_mode(bool night);
  bool send_limited_ui(bool limited);
  bool send_media_button(std::uint8_t report);
  bool request_ui(const std::string& url = {});
  bool request_video_refresh(std::uint64_t stream_type, const std::string& display_uuid);
  bool send_touch(const TouchInput& touch);
  bool send_cluster_button(std::uint8_t report);
  void attach_iap_engine(std::shared_ptr<Iap2SessionEngine> engine);
  std::vector<ControlMessage> take_iap_controls();
  std::size_t iap_control_notice_drops() const { return iap_control_notice_drops_; }
  std::size_t event_requests() const { return event_requests_; }
  std::optional<AirplayEventChannel::Diagnostics> event_diagnostics() const {
    return event_channel_ ? std::optional{event_channel_->diagnostics()} : std::nullopt;
  }

 private:
  struct Port {
    int listener = -1;
    int client = -1;
    std::uint16_t number = 0;
    std::uint64_t type = 0;
    std::unique_ptr<AirplayVideoStream> video;
    std::unique_ptr<CarplayMediaStream> audio;
    std::unique_ptr<AirplayIapStream> iap;
    std::unique_ptr<AirplayDataStream> data;
    std::uint64_t stream_id = 0;
    std::uint64_t connection_id = 0;
    std::string service_uuid;
    PendingBytes outbound{2 * 1024 * 1024};
    bool accepted = false;
    bool retired = false;
    bool input_paused = false;
    std::deque<AirplayVideoPacket> pending_video;
    std::size_t pending_video_bytes = 0;
    std::optional<std::chrono::steady_clock::time_point> video_blocked_since;
  };
  static Port listen_tcp(std::uint64_t type);
  static Port bind_udp();
  static void close_port(Port& port);
  static void close_video(Port& port);
  static void close_iap(Port& port);
  bool deliver_video(Port& port);
  void retire_event() noexcept;
  void require_event_live() const;
  void queue_iap_frames(const Iap2SessionOutput& output);
  void flush_iap_frames();
  std::uint64_t host_ntp_now();

  Port event_;
  Port timing_;
  std::vector<Port> streams_;
  std::size_t accepted_connections_ = 0;
  std::size_t observed_bytes_ = 0;
  sockaddr_in6 peer_{};
  bool peer_set_ = false;
  AirplayClock timing_clock_;
  AirplayHostClockMonitor host_clock_monitor_;
  // Per-connection anchor of the timeline sent to the phone (see host_ntp_now).
  std::optional<std::pair<std::uint64_t, std::uint64_t>> ntp_anchor_;
  std::size_t timing_replies_ = 0;
  std::chrono::steady_clock::time_point next_timing_probe_{};
  bool recording_ = false;
  bool native_ultra_ = false;
  bool ultra_agreed_ = false;
  std::optional<CarplayMode> negotiated_mode_;
  bool vehicle_state_ = false;
  std::string audio_sink_;
  std::string audio_device_;
  std::string microphone_source_, microphone_device_;
  std::weak_ptr<AudioPlayback> audio_playback_;
  std::shared_ptr<audio_observation::Capture> audio_observation_;
  std::uint64_t audio_observation_route_ = 0;
  audio_control::Level audio_level_;
  std::vector<std::uint8_t> shared_secret_;
  std::vector<std::vector<std::uint8_t>> pending_iap_;
  std::size_t pending_iap_bytes_ = 0;
  std::unique_ptr<AirplayEventChannel> event_channel_;
  PendingBytes event_outbound_{65536};
  std::size_t event_requests_ = 0;
  AirplayModesState modes_;
  std::string modes_summary_;
  CommandDiagnostics command_diagnostics_;
  ProtocolCoverage command_coverage_, event_route_coverage_;
  std::shared_ptr<Iap2SessionEngine> iap_engine_;
  std::vector<ControlMessage> pending_controls_;
  std::size_t iap_control_notice_drops_ = 0;
  std::vector<std::vector<std::uint8_t>> pending_iap_frames_;
  std::size_t pending_iap_frame_bytes_ = 0;
  VideoSink video_sink_;
  VideoConsumer video_consumer_;
  std::set<std::string> data_services_;
  // Video, audio and DataStream use the same output-key HKDF namespace.
  // Reservations survive closure so replacement streams cannot restart nonce 0.
  std::set<std::uint64_t> used_crypto_seeds_;
  std::uint64_t next_data_id_ = 2;  // 1 remains the classic iAP stream.
  std::vector<ServiceMessage> pending_services_;
  std::size_t pending_service_bytes_ = 0;
};

}  // namespace headunit
