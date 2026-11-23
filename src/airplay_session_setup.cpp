// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_session_setup.hpp"
#include "headunit/airplay_timing.hpp"
#include "headunit/audio_output.hpp"
#include "headunit/audio_microphone.hpp"

#include <plist/plist.h>
#include <openssl/crypto.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace headunit {
namespace {

constexpr const char* iap_service = "E9459FD0-BCAD-4C45-820F-1E72447EF2F2";
constexpr const char* ui_service = "07D9F906-8D64-4B54-A808-A20BCA2C51C2";
constexpr std::size_t service_queue_limit = 2 * 1024 * 1024;

std::string canonical_uuid(std::string value) {
  if (value.size() != 36) throw std::invalid_argument("invalid service UUID");
  for (std::size_t i = 0; i < value.size(); ++i) {
    const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
    const auto byte = static_cast<unsigned char>(value[i]);
    if (separator ? byte != '-' : !std::isxdigit(byte))
      throw std::invalid_argument("invalid service UUID");
    value[i] = static_cast<char>(std::toupper(byte));
  }
  return value;
}

std::string service_uuid(plist_t stream) {
  auto item = plist_dict_get_item(stream, "clientTypeUUID");
  if (!item) return iap_service;  // Classic iAP also uses an implicit service.
  if (plist_get_node_type(item) != PLIST_STRING)
    throw std::invalid_argument("invalid service type");
  std::uint64_t length = 0;
  const char* value = plist_get_string_ptr(item, &length);
  if (!value || length != 36) throw std::invalid_argument("invalid service UUID");
  return canonical_uuid(std::string(value, length));
}

struct Plist {
  plist_t value = nullptr;
  ~Plist() {
    if (value) plist_free(value);
  }
};

std::uint64_t required_uint(plist_t dictionary, const char* key) {
  const auto item = plist_dict_get_item(dictionary, key);
  if (!item || plist_get_node_type(item) != PLIST_UINT) {
    throw std::invalid_argument("SETUP missing integer field");
  }
  std::uint64_t value = 0;
  plist_get_uint_val(item, &value);
  return value;
}

// Bounded numeric/enum diagnostics only. Never include opaque IDs, keys,
// arbitrary category strings or request bodies in the public error log.
std::string unsupported_stream_description(plist_t stream, std::uint64_t type) {
  auto message = "unsupported stream type " + std::to_string(type);
  constexpr std::array fields{
      "audioFormat", "audioInputFormat", "audioOutputFormat", "audioLatencyMs", "ct", "spf", "sr"};
  for (const auto* field : fields) {
    const auto item = plist_dict_get_item(stream, field);
    if (item && plist_get_node_type(item) == PLIST_UINT) {
      std::uint64_t value = 0;
      plist_get_uint_val(item, &value);
      message += " " + std::string(field) + "=" + std::to_string(value);
    }
  }
  auto category = std::string_view{"other"};
  const auto item = plist_dict_get_item(stream, "audioType");
  if (item && plist_get_node_type(item) == PLIST_STRING) {
    std::uint64_t length = 0;
    const char* text = plist_get_string_ptr(item, &length);
    constexpr std::array known{std::string_view{"media"},     std::string_view{"default"},
                               std::string_view{"alert"},     std::string_view{"speechRecognition"},
                               std::string_view{"telephony"}, std::string_view{"compatibility"}};
    for (const auto value : known)
      if (text && length == value.size() && std::string_view(text, length) == value)
        category = value;
  }
  message += " audioType=" + std::string(category);
  message += plist_dict_get_item(stream, "dataPort") ? " microphoneDataPortPresent=1"
                                                     : " microphoneDataPortPresent=0";
  return message;
}

struct AudioOffer {
  unsigned latency = 1000;
  bool media = true;
  std::uint16_t microphone_port = 0;
  AudioRtpDecoder::Format format = AudioRtpDecoder::Format::aac_lc_48000_stereo;
};
AudioOffer audio_offer(plist_t stream, std::uint64_t type) {
  AudioOffer offer;
  const auto format = required_uint(stream, "audioFormat");
  const auto item = plist_dict_get_item(stream, "audioType");
  std::uint64_t length = 0;
  const auto* value = item && plist_get_node_type(item) == PLIST_STRING
                          ? plist_get_string_ptr(item, &length)
                          : nullptr;
  const auto category =
      value && length <= 32 ? std::string_view(value, length) : std::string_view{};
  if (type == 102) {
    if (format != 0x800000 || category != "media")
      throw std::invalid_argument(unsupported_stream_description(stream, type));
  } else {
    offer.format = static_cast<AudioRtpDecoder::Format>(format);
    AudioRtpDecoder::Description selected;
    try {
      selected = AudioRtpDecoder::describe(offer.format);
    } catch (const std::invalid_argument&) {
      throw std::invalid_argument(unsupported_stream_description(stream, type));
    }
    if (selected.encoding == AudioRtpDecoder::Encoding::aac ||
        (category != "default" && category != "alert" && category != "speechRecognition" &&
         category != "telephony" && category != "compatibility" && category != "media"))
      throw std::invalid_argument(unsupported_stream_description(stream, type));
    offer.media = category == "media";
    offer.latency = 80;
    if (plist_dict_get_item(stream, "dataPort")) {
      const auto port = required_uint(stream, "dataPort");
      if (port > 65535 || (port && (type == 101 || selected.channels != 1 || offer.media)))
        throw std::invalid_argument("invalid microphone destination");
      offer.microphone_port = static_cast<std::uint16_t>(port);
    }
    // The initial implementation negotiates ordinary 12-byte RTP packets.
    // Do not echo redundancy/high accuracy extensions that we cannot parse.
  }
  if (plist_dict_get_item(stream, "audioLatencyMs")) {
    const auto latency = required_uint(stream, "audioLatencyMs");
    if (latency < 20 || latency > 2000) throw std::invalid_argument("unsupported audio latency");
    offer.latency = static_cast<unsigned>(latency);
  }
  return offer;
}

std::vector<std::uint8_t> encode(plist_t value) {
  char* data = nullptr;
  std::uint32_t length = 0;
  if (plist_to_bin(value, &data, &length) != PLIST_ERR_SUCCESS || !data) {
    throw std::runtime_error("SETUP reply encoding failed");
  }
  std::vector<std::uint8_t> result(data, data + length);
  plist_mem_free(data);
  return result;
}

void number(plist_t dictionary, const char* key, std::uint64_t value) {
  plist_dict_set_item(dictionary, key, plist_new_uint(value));
}

std::uint16_t bound_port(int socket) {
  sockaddr_in6 address{};
  socklen_t length = sizeof(address);
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length)) {
    throw std::runtime_error("cannot read SETUP socket port");
  }
  return ntohs(address.sin6_port);
}

int new_socket(int type) {
  const int fd = socket(AF_INET6, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) throw std::runtime_error("cannot create SETUP socket");
  const int dual_stack = 0;
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &dual_stack, sizeof(dual_stack))) {
    close(fd);
    throw std::runtime_error("cannot configure SETUP socket");
  }
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address))) {
    close(fd);
    throw std::runtime_error("cannot bind SETUP socket");
  }
  return fd;
}
}  // namespace

AirplaySessionSetup::~AirplaySessionSetup() {
  close_port(event_);
  close_port(timing_);
  for (auto& stream : streams_)
    close_port(stream);
  if (!shared_secret_.empty()) OPENSSL_cleanse(shared_secret_.data(), shared_secret_.size());
}

void AirplaySessionSetup::set_secret(std::span<const std::uint8_t> shared) {
  if (!shared_secret_.empty() || !streams_.empty() ||
      (shared.size() != 32 && shared.size() != 64)) {
    throw std::invalid_argument("invalid SETUP shared secret");
  }
  shared_secret_.assign(shared.begin(), shared.end());
}

void AirplaySessionSetup::set_peer(const sockaddr_in6& peer) {
  if (peer.sin6_family != AF_INET6 || peer.sin6_port == 0 || peer_set_ || timing_.listener >= 0) {
    throw std::invalid_argument("invalid SETUP peer");
  }
  peer_ = peer;
  peer_set_ = true;
}

void AirplaySessionSetup::close_port(Port& port) {
  if (port.client >= 0) close(port.client);
  if (port.listener >= 0) close(port.listener);
  port = {};
}

AirplaySessionSetup::Port AirplaySessionSetup::listen_tcp(std::uint64_t type) {
  const int fd = new_socket(SOCK_STREAM);
  if (listen(fd, 1)) {
    close(fd);
    throw std::runtime_error("cannot listen on SETUP port");
  }
  Port port;
  port.listener = fd;
  try {
    port.number = bound_port(fd);
    port.type = type;
    return port;
  } catch (...) {
    close(fd);
    throw;
  }
}

AirplaySessionSetup::Port AirplaySessionSetup::bind_udp() {
  const int fd = new_socket(SOCK_DGRAM);
  Port port;
  port.listener = fd;
  try {
    port.number = bound_port(fd);
    return port;
  } catch (...) {
    close(fd);
    throw;
  }
}

void AirplaySessionSetup::enable_native_ultra() {
  if (event_.number != 0 || !video_consumer_ ||
      !data_services_.contains("07D9F906-8D64-4B54-A808-A20BCA2C51C2"))
    throw std::logic_error("Ultra negotiation requires attached consumers before SETUP");
  native_ultra_ = true;
}

void AirplaySessionSetup::set_audio_sink(std::string factory, std::string device,
                                         audio_control::Level initial_level) {
  AudioOutput::validate_selection(factory, device);
  if (initial_level.volume_percent > 100)
    throw std::invalid_argument("invalid initial audio volume");
  if (event_.number != 0 || !streams_.empty() || !audio_sink_.empty() ||
      (factory != "fakesink" && factory != "alsasink" && factory != "pulsesink"))
    throw std::invalid_argument("invalid or late audio output selection");
  audio_sink_ = std::move(factory);
  audio_device_ = std::move(device);
  audio_level_ = initial_level;
}

void AirplaySessionSetup::set_audio_observation(
    std::shared_ptr<audio_observation::Capture> capture) {
  if (event_.number != 0 || !streams_.empty())
    throw std::logic_error("audio observation must be configured before SETUP");
  audio_observation_ = std::move(capture);
}

void AirplaySessionSetup::set_microphone(std::string factory, std::string device) {
  AudioMicrophone::validate_selection(factory, device);
  if (event_.number != 0 || !streams_.empty() || audio_sink_.empty() || !microphone_source_.empty())
    throw std::invalid_argument("invalid or late microphone selection");
  microphone_source_ = std::move(factory);
  microphone_device_ = std::move(device);
}

std::vector<MediaStreamStatus> AirplaySessionSetup::media_status() const {
  std::vector<MediaStreamStatus> result;
  for (const auto& stream : streams_)
    if (stream.audio) result.push_back(stream.audio->status());
  return result;
}

std::vector<std::shared_ptr<audio_control::Mailbox>> AirplaySessionSetup::media_controls() const {
  std::vector<std::shared_ptr<audio_control::Mailbox>> result;
  for (const auto& stream : streams_)
    if (stream.audio && stream.audio->status().fault.empty()) {
      auto control = stream.audio->audio_control();
      if (std::find(result.begin(), result.end(), control) == result.end())
        result.push_back(std::move(control));
    }
  return result;
}
AirplaySessionSetup::AudioFocusResult AirplaySessionSetup::request_audio_focus(
    std::span<const std::uint8_t> bytes) {
  const auto command = decode_audio_focus_command(bytes);
  if (!command) return AudioFocusResult::unhandled;
  if (event_.accepted && event_.client < 0) return AudioFocusResult::unavailable;
  for (const auto& stream : streams_)
    if (stream.audio && stream.audio->request_audio_focus(*command))
      return AudioFocusResult::accepted;
  return AudioFocusResult::unavailable;
}

unsigned AirplaySessionSetup::dispatch_command(std::span<const std::uint8_t> bytes,
                                               ControlChannel channel) {
  require_event_live();
  const auto index = static_cast<unsigned>(channel);
  if (index > 1) throw std::invalid_argument("invalid control channel");
  std::optional<ProtocolCoverageKey> observed;
  try {
    const auto command = decode_airplay_command(bytes);
    const ProtocolCoverageKey key =
        command.unsupported_fingerprint
            ? ProtocolCoverageKey{{}, *command.unsupported_fingerprint}
            : ProtocolCoverageKey{static_cast<unsigned>(command.kind), 0};
    command_coverage_.observe(key, index);
    observed = key;
    auto& count = command_diagnostics_.by_kind[static_cast<std::size_t>(command.kind)];
    ++count;
    unsigned status = 501;
    if (command.modes) {
      modes_.apply(*command.modes, std::chrono::steady_clock::now());
      // Numeric entities only; logged on change so repeated deltas stay bounded.
      auto summary = airplay_modes_summary(modes_);
      if (summary != modes_summary_) {
        modes_summary_ = std::move(summary);
        std::cout << "AirPlay modes " << modes_summary_ << " revision=" << modes_.revision
                  << std::endl;
      }
      const auto owner = airplay_resource_owner(modes_.resources[1]);
      if (owner != AirplayResourceOwner::unobserved) {
        for (const auto& stream : streams_)
          if (stream.audio)
            (void)stream.audio->set_media_resource_enabled(owner ==
                                                           AirplayResourceOwner::controller);
      }
      status = 200;
    } else if (command.kind == AirplayCommandKind::disable_bluetooth) {
      // The phone continues over Wi-Fi and no longer needs our Bluetooth link.
      // Acknowledge only: the manager still owns Bluetooth and reconnection.
      status = 200;
    } else if (command.kind == AirplayCommandKind::duck_audio ||
               command.kind == AirplayCommandKind::unduck_audio) {
      status = request_audio_focus(bytes) == AudioFocusResult::accepted ? 200 : 501;
      if (status == 501) ++command_diagnostics_.unavailable;
    } else {
      ++command_diagnostics_.unsupported;
    }
    if (status == 200) ++command_diagnostics_.accepted;
    // Fixed protocol categories only: never print arbitrary command names or
    // phone parameters. Bound repeated diagnostics logarithmically per kind.
    if (count <= 4 || (count & (count - 1)) == 0)
      std::cout << "AirPlay command category=" << airplay_command_name(command.kind)
                << " count=" << count << " status=" << status << std::endl;
    command_coverage_.result(key, index, status);
    return status;
  } catch (const std::invalid_argument&) {
    const auto key =
        observed.value_or(ProtocolCoverageKey{static_cast<unsigned>(AirplayCommandKind::count), 0});
    if (!observed) command_coverage_.observe(key, index);
    command_coverage_.result(key, index, 400);
    ++command_diagnostics_.malformed;
    return 400;
  }
}

unsigned AirplaySessionSetup::dispatch_event_request(const AirplayEventMessage& request) {
  require_event_live();
  if (request.response) throw std::invalid_argument("event response is not a request");
  const auto key = control_route_key(request.method, request.path);
  event_route_coverage_.observe(key, 1);
  unsigned status = 501;
  if (request.method == "POST" && request.path == "/command")
    status = dispatch_command(request.body, ControlChannel::event);
  else if (request.method == "POST" && request.path == "/feedback")
    status = 200;
  else if (request.method == "OPTIONS")
    status = request.body.empty() ? 200 : 400;
  else {
    const auto count = ++command_diagnostics_.unsupported_event_routes;
    if (count <= 4 || (count & (count - 1)) == 0)
      std::cout << "AirPlay unsupported event route count=" << count << std::endl;
  }
  event_route_coverage_.result(key, 1, status);
  return status;
}

std::vector<std::uint8_t> AirplaySessionSetup::respond(std::span<const std::uint8_t> request,
                                                       ConnectionDiagnostics* diagnostics,
                                                       std::uint64_t link_generation) {
  require_event_live();
  if (request.empty() || request.size() > 8192 || request.size() > UINT32_MAX) {
    throw std::invalid_argument("invalid SETUP body size");
  }
  preflight_binary_plist(request, {8192, 256, 32768, 12});
  Plist decoded;
  if (plist_from_bin(reinterpret_cast<const char*>(request.data()),
                     static_cast<std::uint32_t>(request.size()),
                     &decoded.value) != PLIST_ERR_SUCCESS ||
      !decoded.value || plist_get_node_type(decoded.value) != PLIST_DICT) {
    throw std::invalid_argument("invalid SETUP binary plist");
  }
  Plist reply{plist_new_dict()};
  const auto streams = plist_dict_get_item(decoded.value, "streams");
  if (!streams) {
    if (!peer_set_ || event_.number != 0 || timing_.listener >= 0 ||
        !plist_dict_get_item(decoded.value, "timingPort")) {
      throw std::invalid_argument("duplicate or incomplete session SETUP");
    }
    const auto phone_port = required_uint(decoded.value, "timingPort");
    if (phone_port == 0 || phone_port > 65535) {
      throw std::invalid_argument("invalid phone timing port");
    }
    bool ui = false, hevc = false, vehicle = false;
    if (const auto features = plist_dict_get_item(decoded.value, "features")) {
      if (plist_get_node_type(features) != PLIST_ARRAY || plist_array_get_size(features) > 32)
        throw std::invalid_argument("invalid feature offer");
      for (std::uint32_t i = 0; i < plist_array_get_size(features); ++i) {
        auto item = plist_array_get_item(features, i);
        if (plist_get_node_type(item) != PLIST_STRING)
          throw std::invalid_argument("invalid feature offer");
        char* name = nullptr;
        plist_get_string_val(item, &name);
        if (name) {
          const std::string_view feature(name);
          ui |= feature == "uiSync";
          hevc |= feature == "hevc";
          vehicle |= feature == "vehicleStateProtocol";
          free(name);
        }
      }
    }
    const bool agreed = native_ultra_ && ui && hevc;
    if (diagnostics)
      diagnostics->mark(ConnectionDiagnostics::Stage::setup_features,
                        (ui ? 1u : 0u) | (hevc ? 2u : 0u) | (vehicle ? 4u : 0u), link_generation);
    // Unpublished owners stay local until every allocation and encoding succeeds.
    Port timing, event;
    try {
      timing = bind_udp();
      event = listen_tcp(0);
      number(reply.value, "timingPort", timing.number);
      number(reply.value, "eventPort", event.number);
      plist_t enabled = plist_new_array();
      plist_array_append_item(enabled, plist_new_string("altScreen"));
      plist_array_append_item(enabled, plist_new_string("viewAreas"));
      if (agreed) {
        plist_array_append_item(enabled, plist_new_string("uiSync"));
        plist_array_append_item(enabled, plist_new_string("hevc"));
      }
      // The dual-mode owner's Standard /info omits Ultra vehicle resources.
      // Agree only services belonging to the selected capability profile.
      if (vehicle_state_ && vehicle && (!native_ultra_ || agreed))
        plist_array_append_item(enabled, plist_new_string("vehicleStateProtocol"));
      plist_dict_set_item(reply.value, "enabledFeatures", enabled);
      const auto bytes = encode(reply.value);
      timing_ = std::move(timing);
      event_ = std::move(event);
      peer_.sin6_port = htons(static_cast<std::uint16_t>(phone_port));
      next_timing_probe_ = std::chrono::steady_clock::now();
      ultra_agreed_ = agreed;
      negotiated_mode_ = agreed ? std::nullopt : std::optional{CarplayMode::standard};
      return bytes;
    } catch (...) {
      close_port(timing);
      close_port(event);
      throw;
    }
  }
  if (event_.number == 0 || plist_get_node_type(streams) != PLIST_ARRAY ||
      plist_array_get_size(streams) == 0) {
    throw std::invalid_argument("unsupported stream SETUP");
  }
  const auto offered_count = plist_array_get_size(streams);
  const auto active_count = static_cast<std::size_t>(std::count_if(
      streams_.begin(), streams_.end(), [](const Port& port) { return !port.retired; }));
  if (active_count > active_stream_limit || offered_count > active_stream_limit - active_count)
    throw std::invalid_argument("active stream SETUP limit");
  if (used_crypto_seeds_.size() > stream_history_limit ||
      offered_count > stream_history_limit - used_crypto_seeds_.size())
    throw std::invalid_argument("stream history exhausted; replace control session");
  if (plist_array_get_size(streams) > 1) {
    // Validate the complete offer before creating a socket or audio worker.
    std::set<std::pair<std::uint64_t, std::string>> offered;
    auto seeds = used_crypto_seeds_;
    for (std::uint32_t i = 0; i < plist_array_get_size(streams); ++i) {
      auto item = plist_array_get_item(streams, i);
      if (!item || plist_get_node_type(item) != PLIST_DICT)
        throw std::invalid_argument("invalid stream SETUP item");
      const auto type = required_uint(item, "type");
      if (type != 100 && type != 101 && type != 102 && type != 110 && type != 111 && type != 130)
        throw std::invalid_argument(unsupported_stream_description(item, type));
      const auto service = type == 130 ? service_uuid(item) : std::string{};
      if (!offered.emplace(type, service).second)
        throw std::invalid_argument("duplicate stream in SETUP batch");
      for (const auto& existing : streams_)
        if (!existing.retired && existing.type == type &&
            (type != 130 || existing.service_uuid == service))
          throw std::invalid_argument("duplicate active stream in SETUP batch");
      const auto seed = required_uint(item, type == 130 ? "seed" : "streamConnectionID");
      if (!seeds.insert(seed).second)
        throw std::invalid_argument("reused crypto seed in SETUP batch");
      if (type == 130 && service != iap_service && !data_services_.contains(service))
        throw std::invalid_argument("unregistered service in SETUP batch");
      if (service == ui_service && native_ultra_ && !ultra_agreed_)
        throw std::invalid_argument("UI service was not agreed by the phone");
      if (type == 100 || type == 101 || type == 102) {
        if (audio_sink_.empty() || !peer_set_)
          throw std::invalid_argument("audio output not configured");
        (void)audio_offer(item, type);
      }
    }
    const auto original_size = streams_.size();
    const auto original_mode = negotiated_mode_;
    try {
      Plist results{plist_new_array()};
      for (std::uint32_t i = 0; i < plist_array_get_size(streams); ++i) {
        Plist single{plist_new_dict()};
        auto items = plist_new_array();
        plist_array_append_item(items, plist_copy(plist_array_get_item(streams, i)));
        plist_dict_set_item(single.value, "streams", items);
        const auto bytes = respond(encode(single.value));
        Plist result;
        if (plist_from_bin(reinterpret_cast<const char*>(bytes.data()),
                           static_cast<std::uint32_t>(bytes.size()),
                           &result.value) != PLIST_ERR_SUCCESS)
          throw std::runtime_error("cannot collect SETUP batch response");
        auto entries = plist_dict_get_item(result.value, "streams");
        plist_array_append_item(results.value, plist_copy(plist_array_get_item(entries, 0)));
      }
      plist_dict_set_item(reply.value, "streams", results.value);
      results.value = nullptr;
      return encode(reply.value);
    } catch (...) {
      for (std::size_t i = original_size; i < streams_.size(); ++i)
        close_port(streams_[i]);
      streams_.resize(original_size);
      negotiated_mode_ = original_mode;
      // Failed unpublished allocations still retain crypto/ID reservations.
      // A retry receives fresh keys rather than restarting a cipher counter.
      throw;
    }
  }
  const auto stream = plist_array_get_item(streams, 0);
  if (!stream || plist_get_node_type(stream) != PLIST_DICT) {
    throw std::invalid_argument("invalid stream SETUP");
  }
  const auto type = required_uint(stream, "type");
  if (type != 100 && type != 101 && type != 102 && type != 110 && type != 111 && type != 130) {
    throw std::invalid_argument(unsupported_stream_description(stream, type));
  }
  if (shared_secret_.empty()) throw std::invalid_argument("missing SETUP secret");
  const auto service = type == 130 ? service_uuid(stream) : std::string{};
  const bool generic = type == 130 && service != iap_service;
  if (generic && !data_services_.contains(service))
    throw std::invalid_argument("unregistered data service");
  if (service == ui_service && native_ultra_ && !ultra_agreed_)
    throw std::invalid_argument("UI service was not agreed by the phone");
  for (const auto& existing : streams_) {
    if (!existing.retired && existing.type == type &&
        (type != 130 || (existing.service_uuid == service && (existing.data || existing.iap))))
      throw std::invalid_argument("duplicate stream service");
  }
  const auto seed = type == 130 ? required_uint(stream, "seed") : 0;
  if (type == 130 && used_crypto_seeds_.contains(seed))
    throw std::invalid_argument("DataStream seed already used in session");
  // Validate and construct codecs before allocating the listener.
  Port port;
  std::uint64_t connection_id = 0;
  if (type == 100 || type == 101 || type == 102) {
    const auto offer = audio_offer(stream, type);
    if (audio_sink_.empty() || !peer_set_)
      throw std::invalid_argument("audio output not configured");
    connection_id = required_uint(stream, "streamConnectionID");
    if (used_crypto_seeds_.contains(connection_id))
      throw std::invalid_argument("audio connection ID already used in session");
    auto playback = audio_playback_.lock();
    if (!playback) {
      audio_observation_route_ = audio_observation_ ? audio_observation_->identity() : 0;
      playback = std::make_shared<AudioPlayback>(
          audio_sink_, audio_device_, AudioOutput::Observer{}, audio_level_,
          audio_observation::Point{audio_observation_, 0, audio_observation_route_});
      audio_playback_ = playback;
    }
    CarplayAudioOptions options;
    options.observation = {audio_observation_, 0, audio_observation_route_};
    options.format = offer.format;
    options.media = offer.media;
    options.alternate = type == 101;
    options.playback = std::move(playback);
    options.microphone_port = offer.microphone_port;
    options.microphone_source = microphone_source_;
    options.microphone_device = microphone_device_;
    // Capture can emit input-key packets as soon as the worker starts. Reserve
    // its ID first, even if construction or response publication later fails.
    used_crypto_seeds_.insert(connection_id);
    port.audio = std::make_unique<CarplayMediaStream>(
        shared_secret_, connection_id, peer_, audio_sink_, offer.latency, audio_device_,
        audio_level_, AudioOutput::Observer{},
        type == 101 || !modes_.resources[1] ||
            airplay_resource_owner(modes_.resources[1]) == AirplayResourceOwner::controller,
        std::move(options));
  } else if (type == 110 || type == 111) {
    connection_id = required_uint(stream, "streamConnectionID");
    if (used_crypto_seeds_.contains(connection_id))
      throw std::invalid_argument("video connection ID already used in session");
    auto key = derive_airplay_video_key(shared_secret_, connection_id);
    try {
      used_crypto_seeds_.insert(connection_id);
      port.video = std::make_unique<AirplayVideoStream>(key);
    } catch (...) {
      OPENSSL_cleanse(key.data(), key.size());
      throw;
    }
    OPENSSL_cleanse(key.data(), key.size());
  } else {
    port.service_uuid = service;
    port.stream_id = generic ? next_data_id_ : 1;
    used_crypto_seeds_.insert(seed);
    if (generic)
      port.data = std::make_unique<AirplayDataStream>(shared_secret_, seed);
    else
      port.iap = std::make_unique<AirplayIapStream>(shared_secret_, seed);
  }
  if (!port.audio) {
    auto listener = listen_tcp(type);
    port.listener = listener.listener;
    port.number = listener.number;
  } else {
    port.number = port.audio->data_port();
  }
  port.type = type;
  port.connection_id = type == 130 ? seed : connection_id;
  try {
    Plist response_stream{plist_new_dict()};
    number(response_stream.value, "type", type);
    number(response_stream.value, "dataPort", port.number);
    if (port.audio) {
      if (type == 102) number(response_stream.value, "controlPort", port.audio->control_port());
      number(response_stream.value, "streamConnectionID", connection_id);
    } else if (type == 130) {
      number(response_stream.value, "streamID", port.stream_id);
    } else {
      number(response_stream.value, "streamConnectionID", connection_id);
    }
    Plist response_streams{plist_new_array()};
    plist_array_append_item(response_streams.value, response_stream.value);
    response_stream.value = nullptr;
    plist_dict_set_item(reply.value, "streams", response_streams.value);
    response_streams.value = nullptr;
    const auto bytes = encode(reply.value);
    // Retain seed reservations even after selective close; nonce zero is single-use.
    used_crypto_seeds_.insert(port.connection_id);
    streams_.push_back(std::move(port));
    if (service == ui_service) negotiated_mode_ = CarplayMode::ultra;
    if (generic) ++next_data_id_;
    return bytes;
  } catch (...) {
    close_port(port);
    throw;
  }
}

void AirplaySessionSetup::register_data_service(const std::string& service_uuid) {
  if (event_.number != 0 || data_services_.size() >= 8)
    throw std::invalid_argument("register services before session setup");
  const auto uuid = canonical_uuid(service_uuid);
  if (uuid == iap_service || !data_services_.insert(uuid).second)
    throw std::invalid_argument("duplicate data service registration");
}

void AirplaySessionSetup::enable_vehicle_state() {
  if (event_.number != 0 || vehicle_state_ ||
      !data_services_.contains("3E2F3C61-AAD0-42CB-A8AA-BF22186DA62E") ||
      !data_services_.contains("FF4A6720-F2BE-4F56-A3E1-DB3B4E37D634"))
    throw std::logic_error("vehicle-state consumers must precede SETUP");
  vehicle_state_ = true;
}

std::vector<AirplaySessionSetup::ServiceMessage> AirplaySessionSetup::take_service_messages() {
  auto result = std::move(pending_services_);
  pending_services_.clear();
  pending_service_bytes_ = 0;
  return result;
}

std::vector<AirplaySessionSetup::ServiceStatus> AirplaySessionSetup::service_status() const {
  std::vector<ServiceStatus> result;
  for (const auto& port : streams_) {
    if (port.type != 130 || port.stream_id < 2) continue;
    result.push_back({port.stream_id, port.service_uuid,
                      !port.data         ? ServiceState::closed
                      : port.client >= 0 ? ServiceState::connected
                                         : ServiceState::negotiated});
  }
  return result;
}

std::size_t AirplaySessionSetup::deliver_service_messages(
    std::uint64_t stream_id, const std::function<bool(const DataMessage&)>& accept,
    std::size_t limit) {
  std::size_t delivered = 0;
  for (auto i = pending_services_.begin(); i != pending_services_.end() && delivered < limit;) {
    if (i->stream_id != stream_id) {
      ++i;
      continue;
    }
    if (!accept(i->message)) break;
    pending_service_bytes_ -= i->message.payload.size() + 32;
    i = pending_services_.erase(i);
    ++delivered;
  }
  return delivered;
}

void AirplaySessionSetup::set_video_consumer(VideoConsumer consumer) {
  if (!streams_.empty() || !consumer || video_consumer_)
    throw std::logic_error("video consumer must be configured before stream SETUP");
  video_consumer_ = std::move(consumer);
}

std::vector<AirplaySessionSetup::VideoStatus> AirplaySessionSetup::video_status() const {
  std::vector<VideoStatus> result;
  for (const auto& port : streams_)
    if (port.video)
      result.push_back(
          {port.type,
           port.client >= 0
               ? ServiceState::connected
               : (port.accepted || port.retired ? ServiceState::closed : ServiceState::negotiated),
           port.pending_video.size(), port.pending_video_bytes, port.connection_id});
  return result;
}

std::optional<std::uint64_t> AirplaySessionSetup::video_connection_id(std::uint64_t type) const {
  for (const auto& port : streams_)
    if (port.type == type && port.video && !port.retired) return port.connection_id;
  return {};
}

void AirplaySessionSetup::close_iap(Port& port) {
  if (port.client >= 0) close(port.client);
  if (port.listener >= 0) close(port.listener);
  port.client = port.listener = -1;
  port.retired = true;
  if (port.iap) port.iap->retire();
  // Keep parser counters for the connection summary. No socket can feed this
  // retired cipher or its partial package; a replacement needs a fresh seed.
}

void AirplaySessionSetup::close_video(Port& port) {
  if (port.client >= 0) close(port.client);
  if (port.listener >= 0) close(port.listener);
  port.client = port.listener = -1;
  port.retired = true;
  port.pending_video.clear();
  port.pending_video_bytes = 0;
  port.video_blocked_since.reset();
  if (port.video) port.video->retire();
  // Keep the parser/counters and negotiated type as a closed tombstone.
}

bool AirplaySessionSetup::deliver_video(Port& port) {
  if (!video_consumer_ || !port.video || port.client < 0) return true;
  try {
    for (unsigned n = 0; n < 32 && !port.pending_video.empty(); ++n) {
      auto& packet = port.pending_video.front();
      const auto charge = packet.data.size() + 128;
      if (!video_consumer_(port.type, packet)) {
        const auto now = std::chrono::steady_clock::now();
        if (!port.video_blocked_since) port.video_blocked_since = now;
        if (now - *port.video_blocked_since >= std::chrono::seconds(5))
          throw std::runtime_error("video consumer stalled");
        return false;
      }
      port.pending_video_bytes -= charge;
      port.pending_video.pop_front();
      port.video_blocked_since.reset();
    }
    return port.pending_video.empty();
  } catch (const std::exception&) {
    std::cout << "AirPlay video consumer closed type=" << port.type << std::endl;
    close_video(port);
    return false;
  }
}

bool AirplaySessionSetup::pause_service_input(std::uint64_t stream_id, bool paused) {
  for (auto& port : streams_) {
    if (port.stream_id != stream_id || !port.data) continue;
    port.input_paused = paused;
    return true;
  }
  return false;
}

bool AirplaySessionSetup::send_service_message(std::uint64_t stream_id,
                                               const DataMessage& message) {
  for (auto& port : streams_) {
    if (port.stream_id != stream_id || !port.data) continue;
    if (port.client < 0 || port.data->failed()) return false;
    if (message.payload.size() > AirplayDataStream::max_package - 32)
      throw std::invalid_argument("outbound service message too large");
    const auto plain = message.payload.size() + 32;
    const auto framed = plain + ((plain + 16383) / 16384) * 18;
    if (framed > port.outbound.available()) return false;
    try {
      const auto bytes = port.data->encode(message);
      port.outbound.append(bytes);
    } catch (...) {
      (void)close_service(stream_id);
      throw;
    }
    return true;
  }
  return false;
}

bool AirplaySessionSetup::close_service(std::uint64_t stream_id) {
  for (auto& port : streams_) {
    if (port.stream_id != stream_id || !port.data) continue;
    const auto uuid = port.service_uuid;
    close_port(port);
    // Preserve a tombstone so later requests cannot reuse its ID or seed.
    port.type = 130;
    port.stream_id = stream_id;
    port.service_uuid = uuid;
    port.retired = true;
    for (auto i = pending_services_.begin(); i != pending_services_.end();) {
      if (i->stream_id == stream_id) {
        pending_service_bytes_ -= i->message.payload.size() + 32;
        i = pending_services_.erase(i);
      } else
        ++i;
    }
    return true;
  }
  return false;
}

void AirplaySessionSetup::teardown_services(std::span<const std::uint8_t> request) {
  if (request.empty() || request.size() > 8192)
    throw std::invalid_argument("invalid selective teardown size");
  preflight_binary_plist(request, {8192, 256, 32768, 12});
  Plist root;
  if (plist_from_bin(reinterpret_cast<const char*>(request.data()),
                     static_cast<std::uint32_t>(request.size()),
                     &root.value) != PLIST_ERR_SUCCESS ||
      !root.value || plist_get_node_type(root.value) != PLIST_DICT)
    throw std::invalid_argument("invalid selective teardown plist");
  auto items = plist_dict_get_item(root.value, "streams");
  if (!items || plist_get_node_type(items) != PLIST_ARRAY || plist_array_get_size(items) == 0 ||
      plist_array_get_size(items) > 8)
    throw std::invalid_argument("invalid selective teardown streams");
  std::set<std::size_t> selected;
  for (std::uint32_t i = 0; i < plist_array_get_size(items); ++i) {
    auto item = plist_array_get_item(items, i);
    if (!item || plist_get_node_type(item) != PLIST_DICT)
      throw std::invalid_argument("invalid selective teardown item");
    const auto type = required_uint(item, "type");
    if (type != 100 && type != 101 && type != 102 && type != 110 && type != 111 && type != 130)
      throw std::invalid_argument("unsupported selective teardown type");
    const auto id = type == 130 ? required_uint(item, "streamID") : 0;
    const bool connection = plist_dict_get_item(item, "streamConnectionID") != nullptr;
    const auto connection_id = connection ? required_uint(item, "streamConnectionID") : 0;
    const auto found = std::find_if(streams_.begin(), streams_.end(), [&](const Port& port) {
      return !port.retired && port.type == type && (type != 130 || port.stream_id == id) &&
             (!connection || port.connection_id == connection_id);
    });
    if (found == streams_.end() ||
        !selected.insert(static_cast<std::size_t>(found - streams_.begin())).second)
      throw std::invalid_argument("unknown or repeated teardown stream");
  }
  // Validate the whole selection first: malformed requests cannot partly close it.
  for (const auto index : selected) {
    auto& port = streams_[index];
    if (port.data) {
      (void)close_service(port.stream_id);
    } else if (port.video) {
      close_video(port);
    } else {
      const auto type = port.type;
      const auto id = port.stream_id;
      const auto connection = port.connection_id;
      const auto uuid = port.service_uuid;
      close_port(port);
      port.type = type;
      port.stream_id = id;
      port.connection_id = connection;
      port.service_uuid = uuid;
      port.retired = true;
      if (type == 130) {
        pending_iap_.clear();
        pending_iap_bytes_ = 0;
      }
    }
  }
}

bool AirplaySessionSetup::start_record() {
  if (timing_.listener < 0 || (event_.listener < 0 && event_.client < 0)) return false;
  recording_ = true;
  return true;
}

std::size_t AirplaySessionSetup::video_frames() const {
  std::size_t result = 0;
  for (const auto& port : streams_)
    if (port.video) result += port.video->frames();
  return result;
}

std::size_t AirplaySessionSetup::video_configs() const {
  std::size_t result = 0;
  for (const auto& port : streams_)
    if (port.video) result += port.video->configs();
  return result;
}

std::size_t AirplaySessionSetup::iap_messages() const {
  std::size_t result = 0;
  for (const auto& port : streams_)
    if (port.iap) result += port.iap->messages();
  return result;
}

std::vector<std::vector<std::uint8_t>> AirplaySessionSetup::take_iap_messages() {
  pending_iap_bytes_ = 0;
  auto result = std::move(pending_iap_);
  pending_iap_.clear();
  return result;
}

void AirplaySessionSetup::attach_iap_engine(std::shared_ptr<Iap2SessionEngine> engine) {
  if (!engine || !engine->established() || iap_engine_) {
    throw std::invalid_argument("invalid iAP2 session attachment");
  }
  iap_engine_ = std::move(engine);
}

std::vector<ControlMessage> AirplaySessionSetup::take_iap_controls() {
  auto result = std::move(pending_controls_);
  pending_controls_.clear();
  return result;
}

void AirplaySessionSetup::queue_iap_frames(const Iap2SessionOutput& output) {
  if (!output.bluetooth.empty() || pending_iap_frames_.size() + output.wifi.size() > 64) {
    throw std::invalid_argument("iAP2 session output exceeds limit");
  }
  std::size_t size = pending_iap_frame_bytes_;
  for (const auto& frame : output.wifi) {
    if (frame.size() > 60000 || frame.size() > 65536 - size) {
      throw std::invalid_argument("iAP2 output queue is full");
    }
    size += frame.size();
  }
  pending_iap_frame_bytes_ = size;
  for (const auto& notice : output.unhandled) {
    // These are diagnostic notices only. Retaining unknown phone parameters or
    // ending a healthy transport when its observer is idle serves no consumer.
    if (pending_controls_.size() == 64) {
      pending_controls_.erase(pending_controls_.begin());
      if (iap_control_notice_drops_ != std::numeric_limits<std::size_t>::max())
        ++iap_control_notice_drops_;
    }
    pending_controls_.push_back({notice.identifier, {}});
  }
  pending_iap_frames_.insert(pending_iap_frames_.end(), output.wifi.begin(), output.wifi.end());
}

void AirplaySessionSetup::flush_iap_frames() {
  while (event_.client >= 0 && event_channel_ && !pending_iap_frames_.empty() &&
         event_outbound_.size() < 32768) {
    const auto& frame = pending_iap_frames_.front();
    if (!send_iap_message(frame)) break;
    pending_iap_frame_bytes_ -= frame.size();
    pending_iap_frames_.erase(pending_iap_frames_.begin());
  }
}

bool AirplaySessionSetup::send_iap_message(std::span<const std::uint8_t> message) {
  if (event_.client < 0 || !event_channel_) return false;
  if (message.empty() || message.size() > 60000)
    throw std::invalid_argument("invalid outbound iAP message size");
  try {
    auto framed = event_channel_->send_iap_message(message, event_outbound_.available());
    if (framed.empty()) return false;
    event_outbound_.append(framed);
    return true;
  } catch (...) {
    retire_event();
    throw;
  }
}

void AirplaySessionSetup::retire_event() noexcept {
  recording_ = false;
  modes_ = {};
  modes_summary_.clear();
  for (const auto& stream : streams_)
    if (stream.audio) (void)stream.audio->set_media_resource_enabled(false);
  if (event_.client >= 0) close(event_.client);
  if (event_.listener >= 0) close(event_.listener);
  event_.client = event_.listener = -1;
  event_channel_.reset();
  event_outbound_.clear();
}

void AirplaySessionSetup::require_event_live() const {
  if (event_.accepted && event_.client < 0)
    throw std::runtime_error("authenticated event connection retired; replace control session");
}

std::optional<AirplayClockSample> AirplaySessionSetup::timing_sample() const {
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  return timing_clock_.sample(static_cast<std::uint64_t>(now));
}

std::uint64_t AirplaySessionSetup::host_ntp_now() {
  const auto monotonic_ns = [] {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
  };
  const auto before = monotonic_ns();
  const auto wall = AirplayTiming::ntp_now();
  const auto after = monotonic_ns();
  // The phone synchronizes to the times in our timing replies. Wall time can
  // be corrected by hundreds of milliseconds (systemd-timesyncd's first sync
  // lands about 34 s after boot), which the phone would see as a phase jump.
  // Anchor wall time once per connection, then advance only monotonically.
  if (!ntp_anchor_) ntp_anchor_ = std::pair{wall, after};
  const auto ntp = AirplayTiming::ntp_at(ntp_anchor_->first, ntp_anchor_->second, after);
  // Diagnostic only: corrections still observed on the wall clock.
  if (const auto correction = host_clock_monitor_.observe({wall, before, after})) {
    std::cout << "host timing clock correction diagnostic delta_ns=" << correction->delta_ns
              << " age_ns=" << correction->age_ns
              << " previous_read_bracket_ns=" << correction->previous_read_bracket_ns
              << " read_bracket_ns=" << correction->read_bracket_ns << " observed_mono_ns=" << after
              << '\n'
              << std::flush;
  }
  return ntp;
}

void AirplaySessionSetup::drain() {
  require_event_live();
  if (timing_.listener >= 0 && peer_set_ &&
      std::chrono::steady_clock::now() >= next_timing_probe_) {
    const auto origin = host_ntp_now();
    const auto packet = AirplayTiming::request(origin);
    const auto before = std::chrono::steady_clock::now();
    if (sendto(timing_.listener, packet.data(), packet.size(), 0,
               reinterpret_cast<const sockaddr*>(&peer_),
               sizeof(peer_)) == static_cast<ssize_t>(packet.size())) {
      timing_clock_.sent(
          origin, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 before.time_since_epoch())
                                                 .count()));
    }
    const auto scheduled_at = std::chrono::steady_clock::now();
    next_timing_probe_ =
        scheduled_at +
        std::chrono::nanoseconds(timing_clock_.probe_interval_ns(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(scheduled_at.time_since_epoch())
                .count())));
  }
  auto pump = [this](Port& port) {
    if (port.audio) return;
    if (port.listener < 0 && port.client < 0) return;
    if (port.type == 0 && &port == &timing_) {
      std::array<std::uint8_t, 256> packet{};
      for (int count = 0; count < 8; ++count) {
        sockaddr_in6 sender{};
        socklen_t sender_length = sizeof(sender);
        const auto received = recvfrom(port.listener, packet.data(), packet.size(), 0,
                                       reinterpret_cast<sockaddr*>(&sender), &sender_length);
        const auto received_at = std::chrono::steady_clock::now();
        if (received <= 0) break;
        if (sender_length != sizeof(sender) || sender.sin6_port != peer_.sin6_port ||
            sender.sin6_scope_id != peer_.sin6_scope_id ||
            !std::equal(std::begin(sender.sin6_addr.s6_addr), std::end(sender.sin6_addr.s6_addr),
                        std::begin(peer_.sin6_addr.s6_addr)))
          continue;
        observed_bytes_ += static_cast<std::size_t>(received);
        if (received == 32 && packet[0] == 0x80 && packet[1] == 0xd2) {
          const auto receive_time = host_ntp_now();
          const auto response = AirplayTiming::reply(
              {packet.data(), static_cast<std::size_t>(received)}, receive_time, host_ntp_now());
          (void)sendto(port.listener, response.data(), response.size(), 0,
                       reinterpret_cast<const sockaddr*>(&sender), sender_length);
        } else if (timing_clock_.receive({packet.data(), static_cast<std::size_t>(received)},
                                         static_cast<std::uint64_t>(
                                             std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 received_at.time_since_epoch())
                                                 .count()))) {
          ++timing_replies_;
        }
      }
      return;
    }
    if (port.retired) return;
    if (port.client < 0 && (!port.data || !port.accepted)) {
      port.client = accept4(port.listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (port.client >= 0) {
        port.accepted = true;
        if (&port == &event_ || port.data || port.video || port.iap) {
          close(port.listener);
          port.listener = -1;  // Each cipher generation permits one accepted TCP connection.
        }
        ++accepted_connections_;
        std::cout << "AirPlay stream connected type=" << port.type << std::endl;
        if (&port == &event_) {
          event_channel_ = std::make_unique<AirplayEventChannel>(shared_secret_);
          event_outbound_.clear();
        }
      }
    }
    if (!deliver_video(port)) return;
    if (port.client >= 0 && !port.input_paused) {
      std::array<std::uint8_t, 4096> bytes{};
      for (int iteration = 0; iteration < 8; ++iteration) {
        const auto count = recv(port.client, bytes.data(), bytes.size(), 0);
        if (count > 0) {
          observed_bytes_ += static_cast<std::size_t>(count);
          if (&port == &event_ && event_channel_) {
            try {
              const auto messages =
                  event_channel_->receive({bytes.data(), static_cast<std::size_t>(count)});
              for (const auto& message : messages) {
                if (message.response) {
                  continue;
                }
                ++event_requests_;
                const auto status = dispatch_event_request(message);
                auto response = event_channel_->reply(message, status);
                if (response.size() > event_outbound_.available()) {
                  throw std::invalid_argument("event output queue is full");
                }
                event_outbound_.append(response);
              }
            } catch (const std::exception& error) {
              std::cout << "AirPlay event stream rejected: " << error.what() << std::endl;
              retire_event();
              break;
            }
          }
          if (port.video) {
            try {
              auto packets = port.video->feed({bytes.data(), static_cast<std::size_t>(count)});
              if (video_sink_) {
                for (const auto& packet : packets) {
                  try {
                    video_sink_(port.type, packet);
                  } catch (const std::exception&) {
                    video_sink_ = {};
                    std::cout << "AirPlay video sink disabled after output error" << std::endl;
                    break;
                  }
                }
              }
              if (video_consumer_) {
                // One parser read can finish an 8 MiB packet plus small records.
                // Pause before another read while any of these remain retained.
                constexpr std::size_t limit = 8 * 1024 * 1024 + 16384;
                for (auto& packet : packets) {
                  const auto charge = packet.data.size() + 128;
                  if (port.pending_video.size() >= 64 || charge > limit - port.pending_video_bytes)
                    throw std::invalid_argument("video inbox is full");
                  port.pending_video_bytes += charge;
                  port.pending_video.push_back(std::move(packet));
                }
                if (!deliver_video(port)) break;
              }
            } catch (const std::exception& error) {
              std::cout << "AirPlay video stream rejected type=" << port.type << ": "
                        << error.what() << std::endl;
              close_video(port);
              break;
            }
          }
          if (port.data) {
            try {
              auto messages = port.data->feed({bytes.data(), static_cast<std::size_t>(count)});
              for (auto& message : messages) {
                const auto charge = message.payload.size() + 32;
                if (pending_services_.size() >= 64 ||
                    charge > service_queue_limit - pending_service_bytes_)
                  throw std::invalid_argument("service inbox is full");
                pending_services_.push_back(
                    {port.stream_id, port.service_uuid, std::move(message)});
                pending_service_bytes_ += charge;
              }
            } catch (const std::exception&) {
              std::cout << "AirPlay data service rejected stream=" << port.stream_id << std::endl;
              (void)close_service(port.stream_id);
              break;
            }
          }
          if (port.iap) {
            try {
              const auto messages = port.iap->feed({bytes.data(), static_cast<std::size_t>(count)});
              for (const auto& message : messages) {
                if (iap_engine_) {
                  auto output = iap_engine_->receive_wifi(message, Iap2SessionEngine::Clock::now());
                  queue_iap_frames(output);
                } else {
                  if (pending_iap_.size() >= 64 ||
                      message.size() > 1024 * 1024 - pending_iap_bytes_) {
                    throw std::invalid_argument("iAP inbox is full");
                  }
                  pending_iap_bytes_ += message.size();
                  pending_iap_.push_back(message);
                }
              }
            } catch (const std::exception& error) {
              std::cout << "AirPlay iAP stream rejected: " << error.what() << std::endl;
              close_iap(port);
              break;
            }
          }
          continue;
        }
        if (count == 0 ||
            (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
          std::cout << "AirPlay stream closed type=" << port.type
                    << " reason=" << (count == 0 ? "peer EOF" : "socket error")
                    << " configs=" << (port.video ? port.video->configs() : 0)
                    << " frames=" << (port.video ? port.video->frames() : 0) << std::endl;
          if (port.data) {
            (void)close_service(port.stream_id);
            break;
          }
          if (port.video)
            close_video(port);
          else if (port.iap)
            close_iap(port);
          else {
            close(port.client);
            port.client = -1;
          }
          if (&port == &event_) {
            retire_event();
          }
        }
        break;
      }
    }
  };
  pump(timing_);
  pump(event_);
  // Event encryption counters cannot restart with this session's keys.
  // EOF, malformed framing or a failed write requires a fresh control owner.
  require_event_live();
  if (event_channel_) {
    const auto completions = event_channel_->take_completions();
    if (!completions.empty()) {
      const auto stats = event_channel_->diagnostics();
      const auto total = stats.accepted + stats.rejected + stats.timed_out + stats.evicted;
      if (total <= 4 || (total & (total - 1)) == 0)
        std::cout << "AirPlay event completion accepted=" << stats.accepted
                  << " rejected=" << stats.rejected << " timedOut=" << stats.timed_out
                  << " evicted=" << stats.evicted << " unmatched=" << stats.unmatched
                  << " pending=" << stats.pending << std::endl;
    }
  }
  for (auto& stream : streams_) {
    pump(stream);
    if (stream.data && stream.client >= 0 && !stream.outbound.empty()) {
      const auto pending = stream.outbound.pending();
      const auto count =
          send(stream.client, pending.data(), pending.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
      if (count > 0)
        stream.outbound.consume(static_cast<std::size_t>(count));
      else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        (void)close_service(stream.stream_id);
    }
  }
  if (iap_engine_ && iap_engine_->wifi_active() && event_.client >= 0) {
    queue_iap_frames(iap_engine_->tick(Iap2SessionEngine::Clock::now()));
  }
  flush_iap_frames();
  if (event_.client >= 0 && !event_outbound_.empty()) {
    const auto pending = event_outbound_.pending();
    const auto count =
        send(event_.client, pending.data(), pending.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (count > 0) {
      event_outbound_.consume(static_cast<std::size_t>(count));
    } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      retire_event();
    }
  }
  require_event_live();
}

bool AirplaySessionSetup::show_cluster_map(const std::string& display_uuid) {
  // Ultra's signed layout owns its dynamic content. The legacy alt-screen
  // deep link is only for Standard, including a dual-capability fallback.
  if (ultra_agreed_) return false;
  if (!recording_ || event_.client < 0 || !event_channel_ || event_outbound_.size() > 32768)
    return false;
  const auto ready = std::any_of(streams_.begin(), streams_.end(), [](const Port& port) {
    return port.type == 111 && port.client >= 0 && port.video && port.video->frames() > 0;
  });
  if (!ready) return false;
  const auto bytes = event_channel_->show_ui(display_uuid, "maps:/car/instrumentcluster/map");
  event_outbound_.append(bytes);
  return true;
}

std::size_t AirplaySessionSetup::flush_audio(std::optional<std::uint16_t> resume_seq) {
  std::size_t accepted = 0;
  for (const auto& stream : streams_)
    if (stream.audio && stream.type != 101 && stream.audio->request_flush(resume_seq)) ++accepted;
  return accepted;
}

bool AirplaySessionSetup::request_ui(const std::string& url) {
  if (!recording_ || event_.client < 0 || !event_channel_ || event_outbound_.size() > 32768)
    return false;
  event_outbound_.append(event_channel_->request_ui(url));
  return true;
}

bool AirplaySessionSetup::send_media_button(std::uint8_t report) {
  if (!recording_ || event_.client < 0 || !event_channel_ || event_outbound_.size() > 32768)
    return false;
  const auto bytes = event_channel_->send_media_button(report);
  if (bytes.empty()) return false;
  event_outbound_.append(bytes);
  return true;
}

bool AirplaySessionSetup::send_limited_ui(bool limited) {
  if (!recording_ || event_.client < 0 || !event_channel_ || event_outbound_.size() > 32768)
    return false;
  event_outbound_.append(event_channel_->set_limited_ui(limited));
  return true;
}

bool AirplaySessionSetup::send_night_mode(bool night) {
  if (!recording_ || event_.client < 0 || !event_channel_ || event_outbound_.size() > 32768)
    return false;
  event_outbound_.append(event_channel_->set_night_mode(night));
  return true;
}

bool AirplaySessionSetup::send_touch(const TouchInput& touch) {
  if (event_.client < 0 || !event_channel_ || event_outbound_.size() > 32768) return false;
  const auto bytes = event_channel_->send_touch(touch);
  event_outbound_.append(bytes);
  return true;
}

bool AirplaySessionSetup::send_cluster_button(std::uint8_t report) {
  if (!recording_ || event_.client < 0 || !event_channel_ || event_outbound_.size() > 32768)
    return false;
  const auto bytes = event_channel_->send_cluster_button(report);
  if (bytes.empty()) return false;
  event_outbound_.append(bytes);
  return true;
}
bool AirplaySessionSetup::request_video_refresh(std::uint64_t type, const std::string& uuid) {
  if (type != 110 && type != 111) throw std::invalid_argument("invalid video refresh stream");
  if (!recording_ || event_.client < 0 || !event_channel_ || event_outbound_.size() > 32768)
    return false;
  const bool ready = std::any_of(streams_.begin(), streams_.end(), [&](const Port& port) {
    return port.type == type && port.client >= 0 && port.video && port.video->frames() > 0;
  });
  if (!ready) return false;
  const auto bytes = event_channel_->force_key_frame(uuid);
  event_outbound_.append(bytes);
  return true;
}

void AirplaySessionSetup::append_poll_descriptors(std::vector<pollfd>& descriptors) const {
  const auto append = [&](const Port& port) {
    const int fd = port.client >= 0 ? port.client : port.listener;
    if (fd < 0) return;
    short events = (port.input_paused || !port.pending_video.empty()) ? 0 : POLLIN;
    if (port.data && port.accepted && port.client < 0) return;
    if (port.client >= 0 &&
        ((&port == &event_ && !event_outbound_.empty()) || !port.outbound.empty()))
      events |= POLLOUT;
    if (events) descriptors.push_back({fd, events, 0});
  };
  append(timing_);
  append(event_);
  for (const auto& stream : streams_)
    append(stream);
}

}  // namespace headunit
