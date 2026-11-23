// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_session_setup.hpp"
#include "headunit/airplay_event_channel.hpp"
#include "headunit/airplay_timing.hpp"

#include <plist/plist.h>

#include <cstdint>
#include <array>
#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {
struct Plist {
  plist_t value = nullptr;
  ~Plist() {
    if (value) plist_free(value);
  }
  Plist() = default;
  explicit Plist(plist_t item) : value(item) {}
  Plist(const Plist&) = delete;
  Plist& operator=(const Plist&) = delete;
  Plist(Plist&& other) noexcept : value(other.value) { other.value = nullptr; }
};

void check(bool condition) {
  if (!condition) throw std::runtime_error("AirPlay SETUP test failed");
}

std::vector<std::uint8_t> encode(plist_t item) {
  char* bytes = nullptr;
  std::uint32_t length = 0;
  check(plist_to_bin(item, &bytes, &length) == PLIST_ERR_SUCCESS);
  std::vector<std::uint8_t> result(bytes, bytes + length);
  plist_mem_free(bytes);
  return result;
}

Plist decode(const std::vector<std::uint8_t>& bytes) {
  Plist result;
  check(plist_from_bin(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::uint32_t>(bytes.size()),
                       &result.value) == PLIST_ERR_SUCCESS);
  return result;
}

std::uint64_t number(plist_t dictionary, const char* key) {
  auto item = plist_dict_get_item(dictionary, key);
  check(item && plist_get_node_type(item) == PLIST_UINT);
  std::uint64_t result = 0;
  plist_get_uint_val(item, &result);
  return result;
}

int connect_open(std::uint16_t port) {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  check(socket_fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check(connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  return socket_fd;
}

void connect_to(std::uint16_t port) {
  const int socket_fd = connect_open(port);
  close(socket_fd);
}
}  // namespace

int main() {
  headunit::AirplaySessionSetup setup;
  const std::string ui_service = "07D9F906-8D64-4B54-A808-A20BCA2C51C2";
  const std::string asset_service = "09A8AA2D-8932-4048-9492-6B658D42C2A3";
  setup.register_data_service(ui_service);
  setup.register_data_service(asset_service);
  check(!setup.start_record());
  const int phone_timing = socket(AF_INET6, SOCK_DGRAM, 0);
  check(phone_timing >= 0);
  timeval timeout{2, 0};
  check(setsockopt(phone_timing, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
  sockaddr_in6 phone_address{};
  phone_address.sin6_family = AF_INET6;
  phone_address.sin6_addr = in6addr_loopback;
  check(bind(phone_timing, reinterpret_cast<sockaddr*>(&phone_address), sizeof(phone_address)) ==
        0);
  socklen_t address_length = sizeof(phone_address);
  check(getsockname(phone_timing, reinterpret_cast<sockaddr*>(&phone_address), &address_length) ==
        0);
  setup.set_peer(phone_address);
  std::array<std::uint8_t, 32> pair_secret{};
  setup.set_secret(pair_secret);
  headunit::Iap2Identity identity;
  identity.bluetooth_address = {1, 2, 3, 4, 5, 6};
  auto engine = std::make_shared<headunit::Iap2SessionEngine>(99, identity);
  const auto now = headunit::Iap2SessionEngine::Clock::now();
  (void)engine->receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(), now);
  (void)engine->receive_bluetooth(headunit::encode_link_frame({0xc0, 135, 99, 0, {1}}), now);
  setup.attach_iap_engine(engine);
  bool malformed_rejected = false;
  try {
    setup.respond({});
  } catch (const std::invalid_argument&) {
    malformed_rejected = true;
  }
  check(malformed_rejected);
  Plist request{plist_new_dict()};
  plist_dict_set_item(request.value, "timingPort", plist_new_uint(ntohs(phone_address.sin6_port)));
  auto session = decode(setup.respond(encode(request.value)));
  const auto enabled = plist_dict_get_item(session.value, "enabledFeatures");
  check(enabled && plist_get_node_type(enabled) == PLIST_ARRAY &&
        plist_array_get_size(enabled) == 2);
  std::set<std::string> enabled_names;
  for (std::uint32_t index = 0; index < plist_array_get_size(enabled); ++index) {
    auto item = plist_array_get_item(enabled, index);
    check(item && plist_get_node_type(item) == PLIST_STRING);
    char* value = nullptr;
    plist_get_string_val(item, &value);
    check(value != nullptr);
    enabled_names.emplace(value);
    plist_mem_free(value);
  }
  check(enabled_names == std::set<std::string>{"altScreen", "viewAreas"});
  // The phone may send RECORD immediately after the session SETUP, before
  // allocating its individual video and iAP streams.
  check(setup.start_record() && setup.recording());
  bool duplicate_rejected = false;
  try {
    setup.respond(encode(request.value));
  } catch (const std::invalid_argument&) {
    duplicate_rejected = true;
  }
  check(duplicate_rejected);
  const auto event_port = number(session.value, "eventPort");
  const auto timing_port = number(session.value, "timingPort");
  check(event_port > 0 && event_port < 65536 && timing_port > 0 && timing_port < 65536);
  setup.drain();
  std::array<std::uint8_t, 32> probe{};
  sockaddr_in6 receiver_address{};
  socklen_t receiver_length = sizeof(receiver_address);
  check(recvfrom(phone_timing, probe.data(), probe.size(), 0,
                 reinterpret_cast<sockaddr*>(&receiver_address), &receiver_length) == 32);
  check(probe[0] == 0x80 && probe[1] == 0xd2);
  // Evaluate these sequentially; C++ does not order function arguments.
  const auto phone_received = headunit::AirplayTiming::ntp_now();
  const auto phone_transmitted = headunit::AirplayTiming::ntp_now();
  auto timing_reply = headunit::AirplayTiming::reply(probe, phone_received, phone_transmitted);
  check(sendto(phone_timing, timing_reply.data(), timing_reply.size(), 0,
               reinterpret_cast<sockaddr*>(&receiver_address), receiver_length) == 32);
  setup.drain();
  check(setup.timing_replies() == 1);
  check(!setup.timing_sample() && !setup.timing_failed());  // One exchange is not lock.
  check(sendto(phone_timing, timing_reply.data(), timing_reply.size(), 0,
               reinterpret_cast<const sockaddr*>(&receiver_address), receiver_length) == 32);
  setup.drain();
  check(setup.timing_replies() == 1);  // Replayed reply cannot count as synchronization.
  for (unsigned round = 0; round < 2; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    setup.drain();
    check(recvfrom(phone_timing, probe.data(), probe.size(), 0,
                   reinterpret_cast<sockaddr*>(&receiver_address), &receiver_length) == 32);
    const auto received_time = headunit::AirplayTiming::ntp_now();
    const auto transmitted_time = headunit::AirplayTiming::ntp_now();
    timing_reply = headunit::AirplayTiming::reply(probe, received_time, transmitted_time);
    check(sendto(phone_timing, timing_reply.data(), timing_reply.size(), 0,
                 reinterpret_cast<const sockaddr*>(&receiver_address), receiver_length) == 32);
    setup.drain();
  }
  const auto synchronized = setup.timing_sample();
  check(setup.timing_replies() == 3 && synchronized && !setup.timing_failed());
  check(synchronized->last_sync_ns <= synchronized->monotonic_ns);
  check(synchronized->uncertainty_ns <= 11000000);
  const std::array<char, 8> nonce{};

  int iap_client = -1;
  for (std::uint64_t type : {130, 111, 110}) {
    Plist body{plist_new_dict()};
    plist_t streams = plist_new_array();
    plist_t stream = plist_new_dict();
    plist_dict_set_item(stream, "type", plist_new_uint(type));
    if (type == 130) {
      plist_dict_set_item(stream, "seed", plist_new_uint(31));
    } else {
      plist_dict_set_item(stream, "streamConnectionID", plist_new_uint(type + 1));
    }
    plist_array_append_item(streams, stream);
    plist_dict_set_item(body.value, "streams", streams);
    auto reply = decode(setup.respond(encode(body.value)));
    auto response_streams = plist_dict_get_item(reply.value, "streams");
    check(response_streams && plist_array_get_size(response_streams) == 1);
    auto response_stream = plist_array_get_item(response_streams, 0);
    check(number(response_stream, "type") == type);
    if (type != 130) {
      check(number(response_stream, "streamConnectionID") == type + 1);
    }
    auto port = number(response_stream, "dataPort");
    check(port > 0 && port < 65536);
    // Retain the original classic iAP TCP owner for its messages. Closing it
    // and reconnecting on the same negotiated key is deliberately forbidden.
    if (type == 130)
      iap_client = connect_open(static_cast<std::uint16_t>(port));
    else
      connect_to(static_cast<std::uint16_t>(port));
    setup.drain();
  }
  check(setup.accepted_connections() == 3);
  const auto link_payload = headunit::encode_control_message({0x4300, {1}});
  const auto link_bytes = headunit::encode_link_frame({0x40, 136, 99, 10, link_payload});
  std::vector<std::uint8_t> package(32 + link_bytes.size());
  const auto package_size = package.size();
  for (unsigned index = 0; index < 4; ++index) {
    package[index] = static_cast<std::uint8_t>(package_size >> ((3 - index) * 8));
  }
  package[16] = 'c';
  package[17] = 'o';
  package[18] = 'm';
  package[19] = 'm';
  std::copy(link_bytes.begin(), link_bytes.end(), package.begin() + 32);
  const auto iap_key = headunit::hap_hkdf_sha512(pair_secret, "DataStream-Salt31",
                                                 "DataStream-Output-Encryption-Key");
  const std::array<std::uint8_t, 2> iap_length{static_cast<std::uint8_t>(package.size()),
                                               static_cast<std::uint8_t>(package.size() >> 8)};
  const auto sealed_iap =
      headunit::hap_encrypt(iap_key, {nonce.data(), nonce.size()}, package, iap_length);
  std::vector<std::uint8_t> iap_wire(iap_length.begin(), iap_length.end());
  iap_wire.insert(iap_wire.end(), sealed_iap.begin(), sealed_iap.end());
  check(send(iap_client, iap_wire.data(), iap_wire.size(), 0) ==
        static_cast<ssize_t>(iap_wire.size()));
  for (unsigned attempt = 0; attempt < 3; ++attempt)
    setup.drain();
  const auto controls = setup.take_iap_controls();
  check(controls.size() == 1 && controls[0].identifier == 0x4300 && engine->wifi_active());
  const int event_client = connect_open(static_cast<std::uint16_t>(event_port));
  check(setsockopt(event_client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
  setup.drain();
  check(setup.accepted_connections() == 4);
  std::array<std::uint8_t, 2> event_length{};
  check(recv(event_client, event_length.data(), event_length.size(), MSG_WAITALL) == 2);
  const auto ack_length =
      static_cast<std::size_t>(event_length[0]) | (static_cast<std::size_t>(event_length[1]) << 8);
  std::vector<std::uint8_t> event_sealed(ack_length + 16);
  check(recv(event_client, event_sealed.data(), event_sealed.size(), MSG_WAITALL) ==
        static_cast<ssize_t>(event_sealed.size()));
  const auto event_key =
      headunit::hap_hkdf_sha512(pair_secret, "Events-Salt", "Events-Write-Encryption-Key");
  const auto ack_clear =
      headunit::hap_decrypt(event_key, {nonce.data(), nonce.size()}, event_sealed, event_length);
  check(ack_clear && ack_clear->size() > 20 && (*ack_clear)[0] == 'P');
  const std::array<std::uint8_t, 4> outbound_iap{0xff, 0x5a, 1, 2};
  check(setup.send_iap_message(outbound_iap));
  setup.drain();
  check(recv(event_client, event_length.data(), event_length.size(), MSG_WAITALL) == 2);
  const auto manual_length =
      static_cast<std::size_t>(event_length[0]) | (static_cast<std::size_t>(event_length[1]) << 8);
  event_sealed.resize(manual_length + 16);
  check(recv(event_client, event_sealed.data(), event_sealed.size(), MSG_WAITALL) ==
        static_cast<ssize_t>(event_sealed.size()));
  auto second_nonce = nonce;
  second_nonce[0] = 1;
  const auto manual_clear = headunit::hap_decrypt(
      event_key, {second_nonce.data(), second_nonce.size()}, event_sealed, event_length);
  check(manual_clear && manual_clear->size() > 20 && (*manual_clear)[0] == 'P');

  // Leave a modest event command queued. A maximum iAP command no longer fits
  // once its actual framing overhead is counted, but must remain retryable.
  const std::vector<std::uint8_t> queued_iap(4500, 0x5a);
  check(setup.send_iap_message(queued_iap));
  check(!setup.send_iap_message(std::vector<std::uint8_t>(60000, 0x6b)));
  check(setup.send_iap_message(outbound_iap));
  headunit::AirplayEventChannel expected_event(pair_secret);
  (void)expected_event.send_iap_message(outbound_iap);
  (void)expected_event.send_iap_message(outbound_iap);
  auto expected_commands = expected_event.send_iap_message(queued_iap);
  const auto expected_small = expected_event.send_iap_message(outbound_iap);
  expected_commands.insert(expected_commands.end(), expected_small.begin(), expected_small.end());
  setup.drain();
  std::vector<std::uint8_t> actual_commands(expected_commands.size());
  check(recv(event_client, actual_commands.data(), actual_commands.size(), MSG_WAITALL) ==
        static_cast<ssize_t>(actual_commands.size()));
  check(actual_commands == expected_commands);
  check(setup.recording() && setup.start_record());
  const auto service_request = [&](const std::string& uuid, std::uint64_t seed) {
    Plist body{plist_new_dict()};
    auto streams = plist_new_array();
    auto stream = plist_new_dict();
    plist_dict_set_item(stream, "type", plist_new_uint(130));
    plist_dict_set_item(stream, "seed", plist_new_uint(seed));
    plist_dict_set_item(stream, "clientTypeUUID", plist_new_string(uuid.c_str()));
    plist_array_append_item(streams, stream);
    plist_dict_set_item(body.value, "streams", streams);
    return encode(body.value);
  };
  const auto reject_service = [&](const std::string& uuid, std::uint64_t seed) {
    bool rejected = false;
    try {
      (void)setup.respond(service_request(uuid, seed));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected);
  };
  reject_service(ui_service, 31);  // Classic iAP reserves this seed too.
  reject_service("00000000-0000-0000-0000-000000000000", 40);
  reject_service("bad", 40);
  const auto open_service = [&](const std::string& uuid, std::uint64_t seed) {
    auto answer = decode(setup.respond(service_request(uuid, seed)));
    auto stream = plist_array_get_item(plist_dict_get_item(answer.value, "streams"), 0);
    const auto id = number(stream, "streamID");
    const auto before = setup.service_status();
    check(!before.empty() && before.back().stream_id == id && before.back().service_uuid == uuid &&
          before.back().state == headunit::AirplaySessionSetup::ServiceState::negotiated);
    const auto fd = connect_open(static_cast<std::uint16_t>(number(stream, "dataPort")));
    check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    setup.drain();
    check(setup.service_status().back().state ==
          headunit::AirplaySessionSetup::ServiceState::connected);
    return std::pair{id, fd};
  };
  const auto [ui_id, ui_fd] = open_service(ui_service, 40);
  reject_service(ui_service, 41);     // One active stream per registered service.
  reject_service(asset_service, 40);  // No cross-service key/nonce reuse.
  const auto [asset_id, asset_fd] = open_service(asset_service, 41);
  check(ui_id != asset_id && ui_id != 1 && asset_id != 1);
  // Independently construct phone-direction encrypted APTransport packets.
  const auto phone_packet = [&](std::uint64_t seed, std::uint8_t tag) {
    std::vector<std::uint8_t> plain(35);
    plain[3] = 35;
    plain[4] = 's';
    plain[5] = 'y';
    plain[6] = 'n';
    plain[7] = 'c';
    plain[15] = 7;
    plain[16] = 'c';
    plain[17] = 'm';
    plain[18] = 'n';
    plain[19] = 'd';
    plain[27] = tag;
    plain[32] = tag;
    plain[33] = 2;
    plain[34] = 3;
    const std::array<std::uint8_t, 2> aad{35, 0};
    const auto key = headunit::hap_hkdf_sha512(
        pair_secret, "DataStream-Salt" + std::to_string(seed), "DataStream-Output-Encryption-Key");
    const auto sealed = headunit::hap_encrypt(key, {nonce.data(), nonce.size()}, plain, aad);
    std::vector<std::uint8_t> wire(aad.begin(), aad.end());
    wire.insert(wire.end(), sealed.begin(), sealed.end());
    return wire;
  };
  const auto ui_wire = phone_packet(40, 9);
  const auto asset_wire = phone_packet(41, 10);
  check(send(ui_fd, ui_wire.data(), 7, 0) == 7);
  setup.drain();
  check(setup.take_service_messages().empty());
  check(send(asset_fd, asset_wire.data(), asset_wire.size(), 0) ==
        static_cast<ssize_t>(asset_wire.size()));
  check(send(ui_fd, ui_wire.data() + 7, ui_wire.size() - 7, 0) ==
        static_cast<ssize_t>(ui_wire.size() - 7));
  for (unsigned attempt = 0; attempt < 3; ++attempt)
    setup.drain();
  const auto service_messages = setup.take_service_messages();
  check(service_messages.size() == 2);
  for (const auto& received : service_messages) {
    check(received.message.kind == headunit::DataMessageKind::synchronous);
    check(received.message.group == 7 && received.message.type == 0x636d6e64);
    const bool ui = received.stream_id == ui_id;
    check(received.service_uuid == (ui ? ui_service : asset_service));
    check(received.message.payload ==
          std::vector<std::uint8_t>{static_cast<std::uint8_t>(ui ? 9 : 10), 2, 3});
    headunit::DataMessage ack;
    ack.kind = headunit::DataMessageKind::reply;
    ack.group = received.message.group;
    ack.type = received.message.type;
    ack.token = received.message.token;
    check(setup.send_service_message(received.stream_id, ack));
  }
  setup.drain();
  for (const auto& item : {std::pair{ui_fd, 40}, std::pair{asset_fd, 41}}) {
    std::array<std::uint8_t, 50> wire{};
    check(recv(item.first, wire.data(), wire.size(), MSG_WAITALL) == 50);
    const auto key =
        headunit::hap_hkdf_sha512(pair_secret, "DataStream-Salt" + std::to_string(item.second),
                                  "DataStream-Input-Encryption-Key");
    const auto clear = headunit::hap_decrypt(key, {nonce.data(), nonce.size()},
                                             std::span(wire).subspan(2), std::span(wire).first(2));
    check(clear && clear->size() == 32 && (*clear)[4] == 'r' && (*clear)[7] == 'y' &&
          (*clear)[15] == 7 && (*clear)[27] == (item.second == 40 ? 9 : 10));
  }
  const auto teardown = [&](std::initializer_list<std::uint64_t> ids) {
    Plist body{plist_new_dict()};
    auto streams = plist_new_array();
    for (const auto id : ids) {
      auto stream = plist_new_dict();
      plist_dict_set_item(stream, "type", plist_new_uint(130));
      plist_dict_set_item(stream, "streamID", plist_new_uint(id));
      plist_array_append_item(streams, stream);
    }
    plist_dict_set_item(body.value, "streams", streams);
    setup.teardown_services(encode(body.value));
  };
  bool partial_rejected = false;
  try {
    teardown({ui_id, 999});
  } catch (const std::invalid_argument&) {
    partial_rejected = true;
  }
  check(partial_rejected && setup.send_service_message(ui_id, {}));
  teardown({ui_id});
  check(setup.service_status().front().stream_id == ui_id &&
        setup.service_status().front().service_uuid == ui_service &&
        setup.service_status().front().state ==
            headunit::AirplaySessionSetup::ServiceState::closed);
  check(!setup.close_service(ui_id));
  check(!setup.send_service_message(ui_id, {}));
  reject_service(ui_service, 40);     // Closed seeds stay reserved.
  reject_service(asset_service, 42);  // Other service remains active.
  check(setup.send_service_message(asset_id, {}));
  const auto [new_ui_id, new_ui_fd] = open_service(ui_service, 42);
  check(new_ui_id != ui_id && new_ui_id != asset_id);
  auto corrupt = phone_packet(42, 11);
  corrupt.back() ^= 1;
  check(send(new_ui_fd, corrupt.data(), corrupt.size(), 0) == static_cast<ssize_t>(corrupt.size()));
  setup.drain();
  check(!setup.send_service_message(new_ui_id, {}));
  check(setup.service_status().back().state == headunit::AirplaySessionSetup::ServiceState::closed);
  check(setup.take_service_messages().empty());
  check(setup.send_service_message(asset_id, {}));  // Fault is isolated.
  reject_service(ui_service, 42);                   // Auth failure also keeps its seed reserved.
  close(new_ui_fd);
  const auto [eof_id, eof_fd] = open_service(ui_service, 43);
  close(eof_fd);
  setup.drain();
  check(!setup.send_service_message(eof_id, {}));
  check(setup.service_status().back().stream_id == eof_id &&
        setup.service_status().back().service_uuid == ui_service &&
        setup.service_status().back().state == headunit::AirplaySessionSetup::ServiceState::closed);
  reject_service(ui_service, 43);
  // Unsupported audio diagnostics cannot leak arbitrary strings or keys.
  for (const auto* category : {"speechRecognition", "private-secret\nspoofed-log"}) {
    Plist request{plist_new_dict()};
    auto streams = plist_new_array();
    auto stream = plist_new_dict();
    plist_dict_set_item(stream, "type", plist_new_uint(100));
    plist_dict_set_item(stream, "audioFormat", plist_new_uint(1234));
    plist_dict_set_item(stream, "audioType", plist_new_string(category));
    plist_dict_set_item(stream, "streamConnectionID", plist_new_string("private-secret"));
    plist_dict_set_item(stream, "dataPort", plist_new_uint(4567));
    plist_array_append_item(streams, stream);
    plist_dict_set_item(request.value, "streams", streams);
    bool rejected = false;
    try {
      (void)setup.respond(encode(request.value));
    } catch (const std::invalid_argument& error) {
      const std::string message = error.what();
      rejected = message.find("unsupported stream type 100 audioFormat=1234") == 0 &&
                 message.find("microphoneDataPortPresent=1") != std::string::npos &&
                 message.find("private-secret") == std::string::npos &&
                 message.find("4567") == std::string::npos && message.size() < 512;
      check(message.find(std::string("audioType=") +
                         (std::string_view(category) == "speechRecognition" ? "speechRecognition"
                                                                            : "other")) !=
            std::string::npos);
    }
    check(rejected);
  }

  // Media audio is opt-in and its UDP ports are allocated only for the
  // supported AAC-LC format. A failed SETUP must not reserve stream 102.
  {
    headunit::AirplaySessionSetup media;
    media.set_peer(phone_address);
    media.set_secret(pair_secret);
    bool bad_audio_device = false;
    try {
      media.set_audio_sink("fakesink", "not-a-device");
    } catch (const std::invalid_argument&) {
      bad_audio_device = true;
    }
    check(bad_audio_device && media.media_status().empty());
    media.set_audio_sink("fakesink");
    Plist focus{plist_new_dict()};
    plist_dict_set_item(focus.value, "type", plist_new_string("duckAudio"));
    check(media.request_audio_focus(encode(focus.value)) ==
          headunit::AirplaySessionSetup::AudioFocusResult::unavailable);
    auto init = decode(media.respond(encode(request.value)));
    check(number(init.value, "eventPort") > 0);
    Plist audio_request{plist_new_dict()};
    auto streams = plist_new_array();
    auto item = plist_new_dict();
    plist_dict_set_item(item, "type", plist_new_uint(102));
    plist_dict_set_item(item, "streamConnectionID", plist_new_uint(91));
    plist_dict_set_item(item, "audioFormat", plist_new_uint(0x800000));
    plist_dict_set_item(item, "audioType", plist_new_string("media"));
    plist_array_append_item(streams, item);
    plist_dict_set_item(audio_request.value, "streams", streams);
    auto response = decode(media.respond(encode(audio_request.value)));
    auto accepted = plist_array_get_item(plist_dict_get_item(response.value, "streams"), 0);
    check(number(accepted, "type") == 102 && number(accepted, "dataPort") > 0 &&
          number(accepted, "controlPort") > 0 && media.media_status().size() == 1);
    check(media.request_audio_focus(encode(focus.value)) ==
          headunit::AirplaySessionSetup::AudioFocusResult::accepted);
    check(media.request_audio_focus(encode(focus.value)) ==
              headunit::AirplaySessionSetup::AudioFocusResult::accepted &&
          media.media_status()[0].focus_requested == 1);
    const auto focus_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (media.media_status()[0].focus_configured != 1 &&
           std::chrono::steady_clock::now() < focus_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(media.media_status()[0].focus_configured == 1 &&
          media.media_status()[0].phone_duck_gain == 1 &&
          media.media_status()[0].decoded_blocks == 0);
    auto bad_params = plist_new_dict();
    plist_dict_set_item(bad_params, "volume", plist_new_string("-12"));
    plist_dict_set_item(focus.value, "params", bad_params);
    bool malformed_focus = false;
    try {
      media.request_audio_focus(encode(focus.value));
    } catch (const std::invalid_argument&) {
      malformed_focus = true;
    }
    check(malformed_focus && media.media_status()[0].focus_requested == 1);
    plist_dict_set_item(focus.value, "type", plist_new_string("modesChanged"));
    check(media.request_audio_focus(encode(focus.value)) ==
          headunit::AirplaySessionSetup::AudioFocusResult::unhandled);
    bool duplicate_audio = false;
    try {
      media.respond(encode(audio_request.value));
    } catch (const std::invalid_argument&) {
      duplicate_audio = true;
    }
    check(duplicate_audio);
    const auto music_controls = media.media_controls().front();
    auto voice_request = [&](unsigned type, std::uint64_t id, std::uint64_t microphone_port) {
      Plist body{plist_new_dict()};
      auto voices = plist_new_array();
      auto voice = plist_new_dict();
      plist_dict_set_item(voice, "type", plist_new_uint(type));
      plist_dict_set_item(voice, "streamConnectionID", plist_new_uint(id));
      plist_dict_set_item(voice, "audioFormat", plist_new_uint(0x40000000));
      plist_dict_set_item(voice, "audioType",
                          plist_new_string(type == 100 ? "speechRecognition" : "default"));
      plist_dict_set_item(voice, "audioLatencyMs", plist_new_uint(80));
      plist_dict_set_item(voice, "dataPort", plist_new_uint(microphone_port));
      plist_array_append_item(voices, voice);
      plist_dict_set_item(body.value, "streams", voices);
      return body;
    };
    auto siri = voice_request(100, 92, 4567);
    auto voice_response = decode(media.respond(encode(siri.value)));
    auto voice_stream =
        plist_array_get_item(plist_dict_get_item(voice_response.value, "streams"), 0);
    check(number(voice_stream, "type") == 100 && number(voice_stream, "dataPort") > 0 &&
          number(voice_stream, "streamConnectionID") == 92 &&
          !plist_dict_get_item(voice_stream, "controlPort"));
    auto voice_status = media.media_status().back();
    check(voice_status.voice && voice_status.microphone_requested &&
          !voice_status.microphone_available && voice_status.microphone_sent == 0 &&
          voice_status.microphone_fault == "microphone not configured" &&
          voice_status.fault.empty() && music_controls->snapshot().available);
    auto alt = voice_request(101, 93, 0);
    (void)media.respond(encode(alt.value));
    check(media.media_status().size() == 3);
    Plist goodbye{plist_new_dict()};
    auto retiring = plist_new_array();
    auto retired_voice = plist_new_dict();
    plist_dict_set_item(retired_voice, "type", plist_new_uint(100));
    plist_dict_set_item(retired_voice, "streamConnectionID", plist_new_uint(92));
    plist_array_append_item(retiring, retired_voice);
    plist_dict_set_item(goodbye.value, "streams", retiring);
    check(media.media_controls().size() == 1);
    const auto shared_controls = media.media_controls().front();
    media.teardown_services(encode(goodbye.value));
    check(shared_controls == music_controls && music_controls->snapshot().available &&
          media.media_status().size() == 2);
    bool reused_voice = false;
    try {
      (void)media.respond(encode(siri.value));
    } catch (const std::invalid_argument&) {
      reused_voice = true;
    }
    check(reused_voice && music_controls->snapshot().available);
    auto bad_alt = voice_request(101, 94, 65536);
    bool rejected_alt = false;
    try {
      (void)media.respond(encode(bad_alt.value));
    } catch (const std::invalid_argument&) {
      rejected_alt = true;
    }
    check(rejected_alt && media.media_status().size() == 2);
    // Repeated Siri/guidance replacements reuse an active slot, never its key.
    // Keep music and alternate guidance alive through more than twelve SETUPs.
    for (std::uint64_t id = 100; id < 164; ++id) {
      auto replacement = voice_request(100, id, 4567);
      const auto reply = decode(media.respond(encode(replacement.value)));
      const auto entry = plist_array_get_item(plist_dict_get_item(reply.value, "streams"), 0);
      const auto voice_port = static_cast<std::uint16_t>(number(entry, "dataPort"));
      check(media.media_status().size() == 3 && media.media_controls().size() == 1 &&
            media.media_controls().front() == music_controls);
      plist_dict_set_item(retired_voice, "streamConnectionID", plist_new_uint(id));
      media.teardown_services(encode(goodbye.value));
      check(media.media_status().size() == 2 && music_controls->snapshot().available);
      const int released = socket(AF_INET6, SOCK_DGRAM, 0);
      check(released >= 0);
      sockaddr_in6 address{};
      address.sin6_family = AF_INET6;
      address.sin6_port = htons(voice_port);
      address.sin6_addr = in6addr_any;
      const int result = bind(released, reinterpret_cast<sockaddr*>(&address), sizeof(address));
      close(released);
      check(result == 0);
      bool reused = false;
      try {
        (void)media.respond(encode(replacement.value));
      } catch (const std::invalid_argument&) {
        reused = true;
      }
      check(reused && music_controls->snapshot().available);
    }
  }
  // Keep crypto history bounded independently of active capacity. At exhaustion,
  // reject the whole batch before allocation and leave the active video alive.
  {
    headunit::AirplaySessionSetup history;
    history.set_peer(phone_address);
    history.set_secret(pair_secret);
    (void)history.respond(encode(request.value));
    auto video_request = [](unsigned type, std::uint64_t id) {
      Plist body{plist_new_dict()};
      auto entries = plist_new_array();
      auto video = plist_new_dict();
      plist_dict_set_item(video, "type", plist_new_uint(type));
      plist_dict_set_item(video, "streamConnectionID", plist_new_uint(id));
      plist_array_append_item(entries, video);
      plist_dict_set_item(body.value, "streams", entries);
      return body;
    };
    const auto persistent = video_request(110, 1);
    (void)history.respond(encode(persistent.value));
    for (std::uint64_t id = 2; id < headunit::AirplaySessionSetup::stream_history_limit; ++id) {
      const auto replacement = video_request(111, id);
      (void)history.respond(encode(replacement.value));
      history.teardown_services(encode(replacement.value));
    }
    auto over_budget = video_request(111, 5000);
    const auto other = video_request(130, 5001);
    auto item = plist_copy(plist_array_get_item(plist_dict_get_item(other.value, "streams"), 0));
    plist_dict_set_item(item, "seed", plist_new_uint(5001));
    plist_array_append_item(plist_dict_get_item(over_budget.value, "streams"), item);
    bool exhausted = false;
    try {
      (void)history.respond(encode(over_budget.value));
    } catch (const std::invalid_argument& error) {
      exhausted = std::string_view(error.what()).starts_with("stream history exhausted");
    }
    check(exhausted && history.video_connection_id(110) == 1 && !history.video_connection_id(111));
    const auto final_slot = video_request(111, 5000);
    (void)history.respond(encode(final_slot.value));
    history.teardown_services(encode(final_slot.value));
    exhausted = false;
    try {
      (void)history.respond(encode(final_slot.value));
    } catch (const std::invalid_argument& error) {
      exhausted = std::string_view(error.what()).starts_with("stream history exhausted");
    }
    check(exhausted && history.video_connection_id(110) == 1 &&
          history.video_status().size() == headunit::AirplaySessionSetup::stream_history_limit);
    check(history.start_record());
  }
  // Event crypto belongs to this control generation. A reconnect cannot reuse
  // its secret with counters restarted at zero. Both RECORD/accept orders work.
  for (const bool record_first : {false, true}) {
    for (const bool bad_tag : {false, true}) {
      headunit::AirplaySessionSetup event_owner;
      event_owner.set_secret(pair_secret);
      event_owner.set_peer(phone_address);
      const auto opened = decode(event_owner.respond(encode(request.value)));
      const auto port = static_cast<std::uint16_t>(number(opened.value, "eventPort"));
      if (record_first) check(event_owner.start_record());
      const int client = connect_open(port);
      event_owner.drain();
      check(event_owner.accepted_connections() == 1);
      bool late_service = false;
      try {
        event_owner.register_data_service(ui_service);
      } catch (const std::invalid_argument&) {
        late_service = true;
      }
      check(late_service);
      if (!record_first) check(event_owner.start_record());
      if (bad_tag) {
        const std::array<std::uint8_t, 19> unauthenticated{1};
        check(send(client, unauthenticated.data(), unauthenticated.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(unauthenticated.size()));
      } else {
        check(shutdown(client, SHUT_RDWR) == 0);
      }
      bool retired = false;
      try {
        event_owner.drain();
      } catch (const std::runtime_error&) {
        retired = true;
      }
      check(retired && !event_owner.recording() && !event_owner.send_iap_message(outbound_iap));
      const int retry = socket(AF_INET, SOCK_STREAM, 0);
      check(retry >= 0);
      sockaddr_in retry_address{};
      retry_address.sin_family = AF_INET;
      retry_address.sin_port = htons(port);
      retry_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      check(connect(retry, reinterpret_cast<sockaddr*>(&retry_address), sizeof(retry_address)) < 0);
      close(retry);
      close(client);
      retired = false;
      try {
        event_owner.drain();
      } catch (const std::runtime_error&) {
        retired = true;
      }
      check(retired && event_owner.accepted_connections() == 1);
    }
  }
  // Unknown controls are numeric diagnostic notices. An idle observer must
  // neither retain their private parameters nor fill a fatal application queue.
  for (unsigned batch = 0; batch < 2; ++batch) {
    std::vector<std::uint8_t> payload;
    for (unsigned id = 0; id < 70; ++id) {
      const auto control = headunit::encode_control_message(
          {std::uint16_t(0x6000 + batch * 256 + id), {0xde, 0xad, 0xbe, 0xef}});
      payload.insert(payload.end(), control.begin(), control.end());
    }
    const auto frame =
        headunit::encode_link_frame({0x40, std::uint8_t(137 + batch), 99, 10, payload});
    std::vector<std::uint8_t> body(32 + frame.size());
    for (unsigned i = 0; i < 4; ++i)
      body[i] = std::uint8_t(body.size() >> ((3 - i) * 8));
    body[16] = 'c';
    body[17] = 'o';
    body[18] = 'm';
    body[19] = 'm';
    std::copy(frame.begin(), frame.end(), body.begin() + 32);
    auto counter = nonce;
    counter[0] = char(batch + 1);
    const std::array<std::uint8_t, 2> length{std::uint8_t(body.size()),
                                             std::uint8_t(body.size() >> 8)};
    const auto sealed =
        headunit::hap_encrypt(iap_key, {counter.data(), counter.size()}, body, length);
    std::vector<std::uint8_t> wire(length.begin(), length.end());
    wire.insert(wire.end(), sealed.begin(), sealed.end());
    check(send(iap_client, wire.data(), wire.size(), MSG_NOSIGNAL) == ssize_t(wire.size()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (engine->coverage().delivered_sessions[10] < batch + 2 &&
           std::chrono::steady_clock::now() < deadline)
      setup.drain();
    check(engine->coverage().delivered_sessions[10] == batch + 2 && setup.recording() &&
          engine->established());
  }
  const auto notices = setup.take_iap_controls();
  check(notices.size() == 64 && notices.front().identifier == 0x6100 &&
        notices.back().identifier == 0x613f && setup.iap_control_notice_drops() == 64 &&
        engine->coverage().notice_drops == 12);
  for (const auto& notice : notices)
    check(notice.parameters.empty());
  close(ui_fd);
  close(asset_fd);
  close(iap_client);
  close(event_client);
  close(phone_timing);
  return 0;
}
