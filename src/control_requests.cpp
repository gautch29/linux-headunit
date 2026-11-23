// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "control_receiver.hpp"
#include <headunit/theme/content_control.hpp>

namespace headunit::control {
namespace {
struct CleanseOnExit {
  void* data;
  std::size_t size;
  ~CleanseOnExit() { OPENSSL_cleanse(data, size); }
};

// A small, private first-command sample reveals protocol shapes without
// printing phone metadata. Optional diagnostic failure must not end CarPlay.
void capture_command(Connection& connection, const std::string& directory,
                     std::span<const std::uint8_t> body) {
  if (connection.command_capture_failed || body.empty() || connection.command_requests > 32 ||
      body.size() > 65536 - connection.command_capture_bytes)
    return;
  const auto path = directory + "/command-" + std::to_string(connection.number) + "-" +
                    std::to_string(connection.command_requests) + ".plist";
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    connection.command_capture_failed = true;
    return;
  }
  try {
    write_all(fd, body.data(), body.size());
    connection.command_capture_bytes += body.size();
  } catch (...) {
    connection.command_capture_failed = true;
    (void)unlink(path.c_str());
  }
  close(fd);
}

void send_all(int fd, const std::uint8_t* bytes, std::size_t length) {
  headunit::send_control_response(fd, {bytes, length});
}

void send_text(int fd, const std::string& value) {
  send_all(fd, reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

void log_pairing_shape(std::string_view target, std::string_view hkp, std::string_view body) {
  std::cout << "pairing path=" << target << " hkp=" << (hkp.empty() ? "unspecified" : hkp);
  try {
    const auto* data = reinterpret_cast<const std::uint8_t*>(body.data());
    const auto fields = headunit::decode_hap_tlv({data, body.size()});
    for (const auto& field : fields) {
      if ((field.type == 0 || field.type == 6 || field.type == 0x13) && field.value.size() == 1) {
        const auto* label = field.type == 0 ? " method=" : field.type == 6 ? " state=" : " flags=";
        std::cout << label << static_cast<unsigned>(field.value[0]);
      } else if (field.type == 3 || field.type == 4 || field.type == 5) {
        const auto* label = field.type == 3   ? " public_key_bytes="
                            : field.type == 4 ? " proof_bytes="
                                              : " encrypted_bytes=";
        std::cout << label << field.value.size();
      }
    }
  } catch (const std::invalid_argument&) {
    std::cout << " non_tlv_body_bytes=" << body.size();
  }
  std::cout << '\n' << std::flush;
}
void log_protocol_coverage(const headunit::ProtocolCoverage& coverage, unsigned connection,
                           const char* scope, bool command) noexcept {
  try {
    const auto row = [&](const headunit::ProtocolCoverage::Counts& counts, unsigned channel,
                         const char* name, std::uint64_t fingerprint) {
      if (!counts.received) return;
      std::cout << "protocol coverage connection=" << connection << " scope=" << scope
                << " channel=" << channel << " category=" << name << " fingerprint=" << fingerprint
                << " received=" << counts.received << " results=" << counts.results
                << " ok=" << counts.ok << " badRequest=" << counts.bad_request
                << " unsupported=" << counts.unsupported << " other=" << counts.other << '\n';
    };
    for (unsigned channel = 0; channel < 2; ++channel)
      row(coverage.totals()[channel], channel, "total", 0);
    for (unsigned category = 0; category < coverage.known().size(); ++category) {
      const auto* name =
          command ? (category < static_cast<unsigned>(headunit::AirplayCommandKind::count)
                         ? headunit::airplay_command_name(
                               static_cast<headunit::AirplayCommandKind>(category))
                         : "malformed")
                  : headunit::control_route_name(category);
      for (unsigned channel = 0; channel < 2; ++channel)
        row(coverage.known()[category][channel], channel, name, 0);
    }
    for (const auto& item : coverage.unknown())
      if (item)
        for (unsigned channel = 0; channel < 2; ++channel)
          row(item->channels[channel], channel, "unknown", item->fingerprint);
    std::cout << "protocol coverage connection=" << connection << " scope=" << scope
              << " unknownEvictions=" << coverage.evictions()
              << " diagnosticErrors=" << coverage.errors() << '\n'
              << std::flush;
  } catch (...) {
    // Numeric diagnostic failure cannot prevent owner retirement.
  }
}
void log_vehicle_coverage(const Connection& connection) noexcept {
  if (!connection.vehicle_session || !connection.vehicle_session->coverage()) return;
  try {
    const auto& coverage = *connection.vehicle_session->coverage();
    for (unsigned channel = 0; channel < 2; ++channel) {
      const auto& c = coverage.summary().channels[channel];
      std::cout << "vehicle coverage connection=" << connection.number << " channel=" << channel
                << " requests=" << c.received << " writes=" << c.writes
                << " responseErrors=" << c.response_errors << " outputs=" << c.outputs
                << " unsupportedTypes=" << c.unsupported_types << " retries=" << c.retries
                << " faults=" << c.faults << " receipts=" << c.receipts << '\n';
    }
    auto resources = coverage.resources();
    // Late unknown IIDs remain visible even if the configured profile is large.
    std::sort(resources.begin(), resources.end(), [](const auto& a, const auto& b) {
      if (a.service_iid.has_value() != b.service_iid.has_value()) return !a.service_iid;
      return a.iid < b.iid;
    });
    unsigned printed = 0, omitted = 0;
    for (const auto& resource : resources)
      for (unsigned channel = 0; channel < 2; ++channel) {
        const auto& c = resource.channels[channel];
        if (c == headunit::theme::vehicle::Coverage::ResourceCounts{}) continue;
        if (printed == 64) {
          ++omitted;
          continue;
        }
        ++printed;
        std::cout << "vehicle coverage iid=" << resource.iid << " channel=" << channel
                  << " service=" << resource.service_iid.value_or(-1)
                  << " type=" << resource.service_type.value_or(-1) << " read=" << c.reads
                  << " register=" << c.registrations << " unregister=" << c.unregistrations
                  << " write=" << c.writes << " returnedValues=" << c.returned_values
                  << " returnedErrors=" << c.returned_errors
                  << " serviceConfirmedWrites=" << c.confirmed_writes
                  << " transferredValues=" << c.transferred_values
                  << " transferredErrors=" << c.transferred_errors << '\n';
      }
    std::cout << "vehicle coverage omittedRows=" << omitted
              << " unknownEvictions=" << coverage.summary().unknown_evictions
              << " diagnosticErrors=" << coverage.summary().diagnostic_errors << '\n'
              << std::flush;
  } catch (...) {
    // Optional evidence collection must never interrupt owner retirement.
  }
}
}  // namespace

void close_connection(Connection& connection, headunit::BluetoothIapProfile* bluetooth,
                      UltraRuntime& ultra,
                      headunit::ConnectionDiagnostics::ControlRetirement reason) {
  connection.diagnostics.mark(ConnectionStage::retired, static_cast<unsigned>(reason));
  if (connection.socket >= 0) close(connection.socket);
  if (connection.capture >= 0) close(connection.capture);
  std::cout << "connection=" << connection.number << " captured_bytes=" << connection.bytes;
  if (connection.session_setup) {
    std::cout << " video_configs=" << connection.session_setup->video_configs()
              << " video_frames=" << connection.session_setup->video_frames()
              << " iap_messages=" << connection.session_setup->iap_messages()
              << " iapNoticeDrops=" << connection.session_setup->iap_control_notice_drops()
              << " event_requests=" << connection.session_setup->event_requests()
              << " timing_replies=" << connection.session_setup->timing_replies();
    for (const auto& audio : connection.session_setup->media_status()) {
      std::cout << " audio_packets=" << audio.authenticated << " audio_rejected=" << audio.rejected
                << " audio_datagrams=" << audio.wire_datagrams
                << " audio_truncated=" << audio.truncated_datagrams
                << " audio_control_ignored=" << audio.control_datagrams
                << " audio_control_truncated=" << audio.control_truncated_datagrams
                << " audio_output_accepted=" << audio.output_accepted
                << " audio_output_discontinuities=" << audio.output_discontinuities
                << " audio_output_underruns=" << audio.output_underruns
                << " audio_output_starved=" << audio.output_starved
                << " audio_blocks=" << audio.decoded_blocks
                << " audio_backpressure=" << audio.output_backpressure
                << " audio_resource_requested=" << audio.resource_requested
                << " audio_resource_configured=" << audio.resource_configured
                << " audio_media_enabled=" << audio.media_resource_enabled
                << " audio_voice=" << audio.voice
                << " microphone_requested=" << audio.microphone_requested
                << " microphone_available=" << audio.microphone_available
                << " microphone_sent=" << audio.microphone_sent
                << " microphone_dropped=" << audio.microphone_dropped << " microphone_fault="
                << (audio.microphone_fault.empty() ? "none" : audio.microphone_fault)
                << " audio_fault=" << (audio.fault.empty() ? "none" : audio.fault);
      if (audio.decoder) {
        const auto& decoder = *audio.decoder;
        std::cout << " audio_decoder_submitted=" << decoder.submitted
                  << " audio_decoder_invalid=" << decoder.invalid_packets
                  << " audio_decoder_backpressure=" << decoder.input_backpressure
                  << " audio_decoder_failures=" << decoder.input_failures
                  << " audio_pcm_discarded_frames=" << decoder.discarded_pcm_frames
                  << " audio_pcm_invalid_buffers=" << decoder.invalid_pcm_buffers
                  << " audio_sink_dropped_buffers=";
        if (decoder.appsink_dropped_buffers)
          std::cout << *decoder.appsink_dropped_buffers;
        else
          std::cout << "unavailable";
        if (decoder.jitter) {
          const auto& jitter = *decoder.jitter;
          std::cout << " audio_jitter_pushed=" << jitter.pushed
                    << " audio_jitter_considered_lost=" << jitter.considered_lost
                    << " audio_jitter_late=" << jitter.late
                    << " audio_jitter_duplicates=" << jitter.duplicates
                    << " audio_jitter_average_ns=" << jitter.average_jitter_ns;
        } else
          std::cout << " audio_jitter_stats=unavailable";
      } else
        std::cout << " audio_decoder_stats=unavailable";
    }
  }
  std::cout << " commands=" << connection.command_requests
            << " feedback=" << connection.feedback_requests
            << " rejected_requests=" << connection.rejected_requests
            << " touch_reports=" << connection.touch_reports;
  std::cout << '\n' << std::flush;
  log_protocol_coverage(connection.route_coverage, connection.number, "main-route", false);
  if (connection.session_setup) {
    log_protocol_coverage(connection.session_setup->event_route_coverage(), connection.number,
                          "event-route", false);
    log_protocol_coverage(connection.session_setup->command_coverage(), connection.number,
                          "command", true);
  }
  log_vehicle_coverage(connection);
  if (connection.iap_engine && connection.iap_engine->navigation())
    connection.iap_engine->navigation()->clear();
  if (connection.iap_engine && connection.iap_engine->now_playing())
    connection.iap_engine->now_playing()->clear();
  if (bluetooth) bluetooth->release_session(connection.iap_engine);
  ultra.retire(connection);
  connection.session_setup.reset();
  connection.iap_engine.reset();
  connection.socket = -1;
  connection.capture = -1;
}

void write_all(int fd, const std::uint8_t* bytes, std::size_t length) {
  while (length) {
    const auto count = write(fd, bytes, length);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("private capture write failed");
    bytes += count;
    length -= static_cast<std::size_t>(count);
  }
}

std::unique_ptr<SetupSecret> read_setup_secret(const char* path) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("cannot open private setup secret");
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & 077) != 0 ||
      (info.st_uid != getuid() && getuid() != 0)) {
    close(fd);
    throw std::runtime_error("setup secret must be a private regular file");
  }
  auto secret = std::make_unique<SetupSecret>();
  std::array<char, 130> bytes{};
  const auto count = read(fd, bytes.data(), bytes.size());
  close(fd);
  if (count <= 0 || count > 129) {
    throw std::runtime_error("invalid setup secret length");
  }
  secret->value.assign(bytes.data(), static_cast<std::size_t>(count));
  OPENSSL_cleanse(bytes.data(), bytes.size());
  while (!secret->value.empty() && (secret->value.back() == '\r' || secret->value.back() == '\n')) {
    secret->value.pop_back();
  }
  if (secret->value.empty() || secret->value.size() > 128 ||
      std::any_of(secret->value.begin(), secret->value.end(),
                  [](unsigned char c) { return c < 0x20 || c > 0x7e; })) {
    throw std::runtime_error("invalid setup secret content");
  }
  return secret;
}

std::optional<headunit::ConnectionDiagnostics::ControlRetirement> process_requests(
    Connection& connection, const ReceiverContext& context) {
  using Retirement = headunit::ConnectionDiagnostics::ControlRetirement;
  const auto& directory = context.directory;
  const auto& preview_sink = context.preview_sink;
  const auto& preview_decoder = context.preview_decoder;
  [[maybe_unused]] const auto& displays = context.displays;
  const auto& info_body = context.info_body;
  const auto* setup_secret = context.setup_secret;
  const auto* identity = context.identity;
  const auto* private_key_path = context.private_key_path;
  const auto* pair_store = context.pair_store;
  auto* mfi_auth = context.mfi_auth;
  auto* bluetooth = context.bluetooth;
  [[maybe_unused]] auto& ultra = context.ultra;
  for (;;) {
    const auto parsed =
        headunit::parse_control_request(connection.request_buffer, max_bytes_per_connection);
    if (parsed.status == headunit::ControlParseStatus::invalid) {
      connection.route_coverage.observe(
          {static_cast<unsigned>(headunit::ControlRoute::invalid_framing), 0}, 0);
      return Retirement::invalid_request;
    }
    if (parsed.status == headunit::ControlParseStatus::incomplete) return std::nullopt;
    const auto& request = parsed.request;
    const auto route_key = headunit::control_route_key(request.method, request.target);
    connection.route_coverage.observe(route_key, 0);
    const auto& method = request.method;
    const auto& target = request.target;
    const auto& protocol = request.protocol;
    const auto& cseq = request.cseq;
    if (method == "GET" && target == "/info")
      connection.diagnostics.mark(ConnectionStage::info_begin);
    if (method == "POST" && target == "/pair-setup")
      connection.diagnostics.mark(ConnectionStage::pair_setup_begin);
    if (method == "POST" && target == "/pair-verify")
      connection.diagnostics.mark(ConnectionStage::pair_verify_begin);
    if (method == "POST" && target == "/auth-setup")
      connection.diagnostics.mark(ConnectionStage::auth_setup_begin);
    const bool initial_setup = !connection.setup_completed;
    if (method == "SETUP")
      connection.diagnostics.mark(initial_setup ? ConnectionStage::initial_setup_begin
                                                : ConnectionStage::stream_setup_begin);
    const auto& hkp = request.hkp;
    const auto end = request.header_bytes - 4;
    const auto content_length = request.body_bytes;
    const auto total = request.consumed_bytes;
    if (method == "POST" && (target == "/pair-setup" || target == "/pair-verify")) {
      log_pairing_shape(
          target, hkp, std::string_view(connection.request_buffer).substr(end + 4, content_length));
    }
    std::optional<std::vector<std::uint8_t>> pairing_body;
    std::optional<std::vector<std::uint8_t>> authentication_body;
    std::optional<std::vector<std::uint8_t>> setup_body;
    bool pairing_bad_request = false;
    bool authentication_bad_request = false;
    bool setup_bad_request = false;
    const bool record_bad_request = method == "RECORD" && connection.framing && content_length != 0;
    const bool record = method == "RECORD" && content_length == 0 && connection.framing &&
                        connection.session_setup && connection.session_setup->start_record();
    if (record) connection.record_accepted = true;
    bool teardown = false;
    bool teardown_bad_request = false;
    if (method == "TEARDOWN" && connection.framing) {
      if (content_length == 0)
        teardown = true;
      else if (connection.session_setup) {
        try {
          const auto* body =
              reinterpret_cast<const std::uint8_t*>(connection.request_buffer.data() + end + 4);
          connection.session_setup->teardown_services({body, content_length});
          teardown = true;
        } catch (const std::invalid_argument&) {
          teardown_bad_request = true;
        }
      } else
        teardown_bad_request = true;
    }
    if (teardown && content_length == 0) {
      connection.record_accepted = false;
      connection.presentation_retired = true;
    }
    const bool command =
        method == "POST" && target == "/command" && connection.framing && connection.session_setup;
    const bool feedback =
        method == "POST" && target == "/feedback" && connection.framing && connection.session_setup;
    const bool options_bad_request =
        method == "OPTIONS" && connection.framing && content_length != 0;
    const bool options = method == "OPTIONS" && connection.framing && content_length == 0;
    // RTSP FLUSH: the phone discards its own queue (for example on a track
    // change) and names the next RTP sequence. Ours must not keep playing it.
    bool flush = false, flush_bad_request = false;
    if (method == "FLUSH" && connection.framing) {
      const auto rtp_info =
          headunit::parse_rtp_info(std::string_view(connection.request_buffer).substr(0, end));
      if (content_length != 0 || !connection.session_setup || !rtp_info.valid) {
        flush_bad_request = true;
      } else {
        flush = true;
        std::optional<std::uint16_t> resume;
        if (rtp_info.info) resume = rtp_info.info->seq;
        const auto streams = connection.session_setup->flush_audio(resume);
        const auto count = ++connection.flush_requests;
        if (count <= 4 || (count & (count - 1)) == 0)
          std::cout << "AirPlay flush count=" << count << " streams=" << streams
                    << " resume=" << (resume ? std::to_string(*resume) : std::string("none"))
                    << '\n'
                    << std::flush;
      }
    }
    bool command_bad_request = false, command_unavailable = false;
    if (command) {
      ++connection.command_requests;
      const auto* body =
          reinterpret_cast<const std::uint8_t*>(connection.request_buffer.data() + end + 4);
      capture_command(connection, directory, {body, content_length});
      const auto status = connection.session_setup->dispatch_command({body, content_length});
      command_bad_request = status == 400;
      command_unavailable = status == 501;
    }
    if (feedback) ++connection.feedback_requests;
    if (method == "POST" && target == "/pair-setup" && setup_secret) {
      try {
        if (!connection.pairing) {
          connection.pairing = std::make_unique<headunit::HapPairSetup>(
              setup_secret->value, identity ? identity->pairing_id : std::string{},
              private_key_path ? *private_key_path : std::string{}, mfi_auth);
        }
        const auto* body =
            reinterpret_cast<const std::uint8_t*>(connection.request_buffer.data() + end + 4);
        pairing_body = connection.pairing->respond({body, content_length});
        if (pair_store && connection.pairing->paired_controller()) {
          const auto& record = *connection.pairing->paired_controller();
          const bool new_controller = !pair_store->load(record.identifier).has_value();
          pair_store->save(record);
          if (new_controller) connection.new_pairing_record = record;
          // A controller may probe pair-verify before it knows it needs to
          // pair. Discard that incomplete exchange before its fresh M1.
          connection.verification.reset();
        }
      } catch (const std::invalid_argument&) {
        pairing_bad_request = true;
      }
    }
    if (method == "POST" && target == "/pair-verify" && pair_store && identity &&
        private_key_path) {
      try {
        if (!connection.verification) {
          connection.verification = std::make_unique<headunit::HapPairVerify>(
              identity->pairing_id, *private_key_path, *pair_store);
        }
        const auto* body =
            reinterpret_cast<const std::uint8_t*>(connection.request_buffer.data() + end + 4);
        pairing_body = connection.verification->respond({body, content_length});
      } catch (const std::invalid_argument&) {
        pairing_bad_request = true;
      }
    }
    if (method == "POST" && target == "/auth-setup" && connection.framing && mfi_auth) {
      try {
        const auto* body =
            reinterpret_cast<const std::uint8_t*>(connection.request_buffer.data() + end + 4);
        headunit::AirplayAuthSetup authentication(*mfi_auth);
        authentication_body = authentication.respond({body, content_length});
      } catch (const std::invalid_argument&) {
        authentication_bad_request = true;
      }
    }
    if (method == "SETUP" && connection.framing) {
      try {
        if (!connection.session_setup) {
          std::shared_ptr<headunit::Iap2SessionEngine> iap_engine;
          if (bluetooth) {
            iap_engine = bluetooth->engine();
            if (iap_engine && (!iap_engine->established() || !iap_engine->handoff_sent())) {
              iap_engine.reset();
            }
          }
          connection.session_setup = std::make_unique<headunit::AirplaySessionSetup>();
          if (context.vehicle_profile || context.vehicle_configuration) {
            if (context.vehicle_configuration) {
              auto provider = context.vehicle_provider;
              if (context.vehicle_configuration->content_request) {
                connection.content_state =
                    std::make_shared<headunit::theme::vehicle::ContentStateSnapshot>(
                        *context.vehicle_configuration->content_request);
                provider = headunit::theme::vehicle::content_request_provider(
                    provider, *context.vehicle_configuration->content_request,
                    context.ultra.launch_nonce, connection.number, connection.content_state);
              }
              if (context.vehicle_configuration->camera) {
                connection.camera_state =
                    std::make_shared<headunit::theme::vehicle::CameraStateSnapshot>();
                provider = headunit::theme::vehicle::camera_state_provider(
                    provider, *context.vehicle_configuration->camera, connection.camera_state);
              }
              connection.vehicle_session = std::make_unique<headunit::theme::vehicle::Session>(
                  *context.vehicle_configuration, std::move(provider), context.vehicle_observer);
            } else
              connection.vehicle_session =
                  std::make_unique<headunit::theme::vehicle::Session>(*context.vehicle_profile);
            connection.vehicle_session->attach(*connection.session_setup);
          }
          if (!context.audio_sink.empty())
            connection.session_setup->set_audio_sink(context.audio_sink, context.audio_device,
                                                     context.audio_level);
          connection.session_setup->set_audio_observation(context.audio_observation);
          if (!context.microphone_source.empty())
            connection.session_setup->set_microphone(context.microphone_source,
                                                     context.microphone_device);
#ifdef HEADUNIT_HAVE_LIVE_UI
          if (ultra.installed_ui) ultra.attach(connection, displays, *identity);
#endif
          auto selected_preview = preview_sink;
#ifdef HEADUNIT_HAVE_LIVE_UI
          if (connection.ultra_video && !ultra.negotiate)
            selected_preview = "none";  // Explicit native replay diagnostics.
#endif
          connection.video_output =
              std::make_shared<headunit::VideoOutput>(headunit::VideoOutput::Options{
                  directory, connection.number, selected_preview, preview_decoder,
                  context.standard_displays[0].size, context.standard_displays[1].size,
                  context.standard_cluster_wayland, context.main_preview, true});
          connection.session_setup->set_video_sink(
              [capture = connection.video_output, setup = connection.session_setup.get()](
                  std::uint64_t type, const headunit::AirplayVideoPacket& packet) {
                capture->select_mode(setup->negotiated_mode());
                capture->record(type, packet, setup->timing_sample());
              });
          if (iap_engine) {
            connection.session_setup->attach_iap_engine(iap_engine);
            connection.iap_engine = std::move(iap_engine);
          }
          sockaddr_in6 peer{};
          socklen_t peer_length = sizeof(peer);
          if (getpeername(connection.socket, reinterpret_cast<sockaddr*>(&peer), &peer_length) !=
                  0 ||
              peer_length != sizeof(peer)) {
            throw std::runtime_error("cannot read control peer address");
          }
          connection.session_setup->set_peer(peer);
          if (connection.verification && connection.verification->verified()) {
            auto secret = connection.verification->shared_secret();
            if (!secret) throw std::runtime_error("missing verified stream secret");
            const CleanseOnExit clean_secret{secret->data(), secret->size()};
            connection.session_setup->set_secret(*secret);
          } else if (connection.pairing) {
            auto secret = connection.pairing->transient_control_secret();
            if (!secret) throw std::runtime_error("missing transient stream secret");
            const CleanseOnExit clean_secret{secret->data(), secret->size()};
            connection.session_setup->set_secret(*secret);
          }
        }
        if (bluetooth && !connection.iap_engine) {
          auto iap_engine = bluetooth->engine();
          if (iap_engine && iap_engine->established() && iap_engine->handoff_sent()) {
            connection.session_setup->attach_iap_engine(iap_engine);
            connection.iap_engine = std::move(iap_engine);
          }
        }
        const auto* body =
            reinterpret_cast<const std::uint8_t*>(connection.request_buffer.data() + end + 4);
        setup_body = connection.session_setup->respond(
            {body, content_length}, &connection.diagnostics,
            connection.iap_engine ? connection.iap_engine->diagnostic_generation() : 0);
        if (connection.video_output)
          connection.video_output->select_mode(connection.session_setup->negotiated_mode());
        const auto mode = connection.session_setup->negotiated_mode();
        const auto link =
            connection.iap_engine ? connection.iap_engine->diagnostic_generation() : 0;
        // Observations of the existing SETUP owner, never a readiness grant.
        if (mode == headunit::CarplayMode::standard)
          connection.diagnostics.mark(ConnectionStage::negotiated_standard, 0, link);
        else if (mode == headunit::CarplayMode::ultra)
          connection.diagnostics.mark(ConnectionStage::negotiated_ultra, 0, link);
        else if (initial_setup)
          connection.diagnostics.mark(ConnectionStage::ultra_requested, 0, link);
        for (const auto& stream : connection.session_setup->video_status())
          connection.diagnostics.mark(stream.type == 110 ? ConnectionStage::main_stream_setup
                                                         : ConnectionStage::cluster_stream_setup,
                                      0, link);
      } catch (const std::invalid_argument& error) {
        std::cout << "SETUP rejected: " << error.what() << '\n' << std::flush;
        setup_bad_request = true;
      }
    }
    connection.request_buffer.erase(0, total);

    const auto version = protocol == "RTSP/1.0" ? "RTSP/1.0" : "HTTP/1.1";
    const bool info = method == "GET" && target == "/info" && info_body;
    const auto mode =
        connection.session_setup ? connection.session_setup->negotiated_mode() : std::nullopt;
    const auto* advertised_info =
        info_body ? &headunit::select_airplay_info(*info_body, context.standard_info_body, mode)
                  : nullptr;
    std::optional<std::vector<std::uint8_t>> configured_info;
    if (info && mode != headunit::CarplayMode::standard && context.vehicle_configuration) {
      headunit::theme::vehicle::ConfiguredModel model(*context.vehicle_configuration,
                                                      context.vehicle_provider);
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
      model.refresh(now);
      configured_info =
          headunit::theme::vehicle::add_server_info(*advertised_info, model.service(), now);
      advertised_info = &*configured_info;
    }
    std::optional<std::vector<std::uint8_t>> night_info;
    if (info) {
      if (const auto night = headunit::receiver_night_mode_now()) {
        night_info = headunit::with_night_mode(*advertised_info, *night);
        advertised_info = &*night_info;
      }
      if (headunit::receiver_limited_ui()) {
        // Start unrestricted; the session sends setLimitedUI once speed is known.
        night_info = headunit::with_limited_ui(*advertised_info, false);
        advertised_info = &*night_info;
      }
    }
    const auto status = info || pairing_body || authentication_body || setup_body || record ||
                                teardown ||
                                (command && !command_bad_request && !command_unavailable) ||
                                feedback || options || flush
                            ? "200 OK"
                        : pairing_bad_request || authentication_bad_request || setup_bad_request ||
                                teardown_bad_request || command_bad_request || record_bad_request ||
                                options_bad_request || flush_bad_request
                            ? "400 Bad Request"
                            : "501 Not Implemented";
    if (std::string_view(status) != "200 OK") {
      const auto count = ++connection.rejected_requests;
      if (count <= 4 || (count & (count - 1)) == 0)
        std::cout << "control request rejected count=" << count << " status=" << status << " route="
                  << (route_key.category ? headunit::control_route_name(*route_key.category)
                                         : "unclassified")
                  << '\n'
                  << std::flush;
    }
    const auto body_length = info                  ? advertised_info->size()
                             : pairing_body        ? pairing_body->size()
                             : authentication_body ? authentication_body->size()
                             : setup_body          ? setup_body->size()
                                                   : 0;
    std::string response = std::string(version) + " " + status + "\r\n";
    if (!cseq.empty()) response += "CSeq: " + cseq + "\r\n";
    if (info) response += "Content-Type: application/x-apple-binary-plist\r\n";
    if (pairing_body) response += "Content-Type: application/pairing+tlv8\r\n";
    if (authentication_body) response += "Content-Type: application/octet-stream\r\n";
    if (setup_body) response += "Content-Type: application/x-apple-binary-plist\r\n";
    response += "Content-Length: " + std::to_string(body_length) + "\r\n\r\n";
    if (connection.framing) {
      std::vector<std::uint8_t> plaintext(response.begin(), response.end());
      if (info) plaintext.insert(plaintext.end(), advertised_info->begin(), advertised_info->end());
      if (pairing_body)
        plaintext.insert(plaintext.end(), pairing_body->begin(), pairing_body->end());
      if (authentication_body)
        plaintext.insert(plaintext.end(), authentication_body->begin(), authentication_body->end());
      if (setup_body) plaintext.insert(plaintext.end(), setup_body->begin(), setup_body->end());
      const auto encrypted = connection.framing->encode(plaintext);
      send_all(connection.socket, encrypted.data(), encrypted.size());
    } else {
      send_text(connection.socket, response);
      if (info) send_all(connection.socket, advertised_info->data(), advertised_info->size());
      if (pairing_body) send_all(connection.socket, pairing_body->data(), pairing_body->size());
    }
    // End dates are after the actual response write, not a queued promise.
    const unsigned diagnostic_result = std::string_view(status) == "200 OK"            ? 200
                                       : std::string_view(status) == "400 Bad Request" ? 400
                                                                                       : 501;
    connection.route_coverage.result(route_key, 0, diagnostic_result);
    const auto link_generation =
        connection.iap_engine ? connection.iap_engine->diagnostic_generation() : 0;
    if (info)
      connection.diagnostics.mark(ConnectionStage::info_end, diagnostic_result, link_generation);
    if (method == "POST" && target == "/pair-setup")
      connection.diagnostics.mark(ConnectionStage::pair_setup_end, diagnostic_result);
    if (method == "POST" && target == "/pair-verify")
      connection.diagnostics.mark(ConnectionStage::pair_verify_end, diagnostic_result);
    if (method == "POST" && target == "/auth-setup")
      connection.diagnostics.mark(ConnectionStage::auth_setup_end, diagnostic_result);
    if (method == "SETUP") {
      connection.diagnostics.mark(
          initial_setup ? ConnectionStage::initial_setup_end : ConnectionStage::stream_setup_end,
          diagnostic_result, link_generation);
      if (setup_body) connection.setup_completed = true;
    }
    if (record) connection.diagnostics.mark(ConnectionStage::record_accepted, 200, link_generation);
    if (std::string_view(status) == "200 OK" && !command && !feedback)
      std::cout << (info                  ? "GET /info answered"
                    : authentication_body ? "POST /auth-setup answered"
                    : setup_body          ? "SETUP ports allocated"
                    : record              ? "RECORD accepted"
                    : teardown            ? "TEARDOWN acknowledged"
                    : options             ? "OPTIONS acknowledged"
                    : pairing_body        ? "pairing message answered"
                                          : "unsupported control request captured")
                << '\n'
                << std::flush;
    if (teardown && content_length == 0) return Retirement::full_teardown;
    if (!connection.framing) {
      auto activate = [&](std::span<const std::uint8_t> secret) {
        connection.framing = std::make_unique<headunit::HapControlFrame>(secret);
        if (!connection.request_buffer.empty()) {
          const auto* bytes =
              reinterpret_cast<const std::uint8_t*>(connection.request_buffer.data());
          const auto decrypted =
              connection.framing->receive({bytes, connection.request_buffer.size()});
          connection.request_buffer.assign(reinterpret_cast<const char*>(decrypted.data()),
                                           decrypted.size());
        }
        connection.diagnostics.mark(ConnectionStage::encrypted_control);
        std::cout << "encrypted control ready\n" << std::flush;
      };
      if (connection.verification && connection.verification->verified()) {
        auto shared = connection.verification->shared_secret();
        if (!shared) return Retirement::owner_failure;
        const CleanseOnExit clean_shared{shared->data(), shared->size()};
        activate(*shared);
      } else if (connection.pairing) {
        auto transient = connection.pairing->transient_control_secret();
        if (transient) {
          const CleanseOnExit clean_transient{transient->data(), transient->size()};
          activate(*transient);
        }
      }
    }
  }
}

void check_private_directory(const std::string& path) {
  if (mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) {
    throw std::runtime_error("cannot create private capture directory");
  }
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
      (info.st_mode & 077) != 0) {
    throw std::runtime_error("capture directory must be private and owned by this user");
  }
}

unsigned parse_number(const char* value, unsigned maximum, const char* name, bool allow_zero) {
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoul(value, &end, 10);
  if (errno || !end || *end != '\0' || (!allow_zero && parsed == 0) || parsed > maximum) {
    throw std::invalid_argument(std::string("invalid ") + name);
  }
  return static_cast<unsigned>(parsed);
}
}  // namespace headunit::control
