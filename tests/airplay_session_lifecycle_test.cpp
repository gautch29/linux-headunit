// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_session_setup.hpp"
#include "headunit/binary_plist.hpp"
#include "headunit/iap2_parameter.hpp"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cerrno>
#include <vector>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <iostream>
#include <stdexcept>

namespace {
using namespace headunit;
using Value = PlistValue;
using Dict = Value::Dictionary;
using Array = Value::Array;
Value number(std::int64_t value) {
  return Value{value};
}
Value video(unsigned type, unsigned id) {
  return Value{Dict{{"type", number(type)}, {"streamConnectionID", number(id)}}};
}
Value iap(unsigned seed) {
  return Value{Dict{{"type", number(130)}, {"seed", number(seed)}}};
}
std::vector<std::uint8_t> streams(Array list) {
  return encode_binary_plist(Value{Dict{{"streams", Value{std::move(list)}}}});
}
std::vector<std::uint8_t> command(std::string type, Dict params = {}) {
  return encode_binary_plist(
      Value{Dict{{"type", Value{std::move(type)}}, {"params", Value{std::move(params)}}}});
}
void check(bool valid, const char* reason) {
  if (!valid) throw std::runtime_error(reason);
}
std::uint16_t data_port(const std::vector<std::uint8_t>& response) {
  const auto parsed = decode_binary_plist(response);
  const auto& list = std::get<Array>(std::get<Dict>(parsed.value).at("streams").value);
  return static_cast<std::uint16_t>(
      std::get<std::int64_t>(std::get<Dict>(list[0].value).at("dataPort").value));
}
struct Peer {
  int fd;
  explicit Peer(std::uint16_t port) : fd(socket(AF_INET, SOCK_STREAM, 0)) {
    check(fd >= 0, "cannot create isolated stream peer");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      close(fd);
      throw std::runtime_error("cannot connect isolated stream peer");
    }
  }
  ~Peer() {
    if (fd >= 0) close(fd);
  }
  void eof() {
    close(fd);
    fd = -1;
  }
};
template <class F>
void rejects(F&& call) {
  try {
    call();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid lifecycle request accepted");
}
// Process-local syscall fault injection. No physical owner or fake allocator.
std::size_t descriptors() {
  std::size_t count = 0;
  for (int fd = 0; fd < 1024; ++fd)
    if (fcntl(fd, F_GETFD) >= 0) ++count;
  return count;
}
class DescriptorPressure {
 public:
  DescriptorPressure() {
    check(getrlimit(RLIMIT_NOFILE, &original_) == 0, "cannot read descriptor limit");
    held_.reserve(1024);
    anchor_ = dup(STDERR_FILENO);
    check(anchor_ >= 0, "cannot create pressure anchor");
    try {
      int highest = anchor_;
      for (int fd = 0; fd < 1024; ++fd)
        if (fcntl(fd, F_GETFD) >= 0) highest = fd;
      check(static_cast<rlim_t>(highest + 16) < original_.rlim_cur,
            "fixture requires spare descriptors");
      const rlimit reduced{static_cast<rlim_t>(highest + 16), original_.rlim_max};
      check(setrlimit(RLIMIT_NOFILE, &reduced) == 0, "cannot reduce descriptor limit");
      active_ = true;
      for (;;) {
        const int fd = dup(anchor_);
        if (fd < 0) {
          check(errno == EMFILE, "unexpected pressure failure");
          break;
        }
        held_.push_back(fd);
      }
      check(!held_.empty(), "pressure created no fixture descriptors");
      close(held_.back());
      held_.pop_back();  // Exactly one allocation may succeed before EMFILE.
    } catch (...) {
      restore();
      throw;
    }
  }
  ~DescriptorPressure() { restore(); }
  DescriptorPressure(const DescriptorPressure&) = delete;
  DescriptorPressure& operator=(const DescriptorPressure&) = delete;

 private:
  void restore() noexcept {
    if (active_) (void)setrlimit(RLIMIT_NOFILE, &original_);
    active_ = false;
    for (const int fd : held_)
      close(fd);
    held_.clear();
    if (anchor_ >= 0) close(anchor_);
    anchor_ = -1;
  }
  rlimit original_{};
  int anchor_ = -1;
  bool active_ = false;
  std::vector<int> held_;
};
void allocation_failures() {
  AirplaySessionSetup setup;
  const std::string uuid = "07D9F906-8D64-4B54-A808-A20BCA2C51C2";
  setup.register_data_service(uuid);
  sockaddr_in6 peer{};
  peer.sin6_family = AF_INET6;
  peer.sin6_addr = in6addr_loopback;
  peer.sin6_port = htons(9);
  setup.set_peer(peer);
  const std::array<std::uint8_t, 32> secret{};
  setup.set_secret(secret);
  const auto initial = encode_binary_plist(Value{Dict{{"timingPort", number(9)}}});
  const auto before = descriptors();
  bool failed = false;
  {
    DescriptorPressure pressure;
    try {
      (void)setup.respond(initial);
    } catch (const std::runtime_error&) {
      failed = true;
    }
  }
  check(failed && descriptors() == before && !setup.start_record(),
        "failed initial allocation leaked a socket or RECORD authority");
  (void)setup.respond(initial);
  check(setup.start_record(), "initial allocation failure prevented clean retry");
  // Keep a healthy main video while repeated generic/cluster batches fail.
  (void)setup.respond(streams({video(110, 400)}));
  const auto healthy_descriptors = descriptors();
  for (unsigned attempt = 0; attempt < 10; ++attempt) {
    const unsigned seed = 500 + attempt;
    const Value service{
        Dict{{"type", number(130)}, {"seed", number(seed)}, {"clientTypeUUID", Value{uuid}}}};
    const auto batch = streams({service, video(111, 600 + attempt)});
    failed = false;
    {
      DescriptorPressure pressure;
      try {
        (void)setup.respond(batch);
      } catch (const std::runtime_error&) {
        failed = true;
      }
    }
    check(failed && descriptors() == healthy_descriptors && setup.service_status().empty() &&
              setup.video_status().size() == 1 && setup.recording(),
          "unpublished batch leaked sockets or disturbed a healthy owner");
    rejects([&] { (void)setup.respond(streams({service})); });
    rejects([&] { (void)setup.respond(streams({video(111, 600 + attempt)})); });
  }
  // Keys from both entries of each rolled-back allocation remain reserved.
  // Reservations still reject key reuse, but no longer consume active slots.
  (void)setup.respond(streams({iap(800), video(111, 801)}));
  check(descriptors() == healthy_descriptors + 2,
        "retired reservations consumed active stream capacity");
  const auto last = decode_binary_plist(setup.respond(streams({Value{
      Dict{{"type", number(130)}, {"seed", number(900)}, {"clientTypeUUID", Value{uuid}}}}})));
  const auto& results = std::get<Array>(std::get<Dict>(last.value).at("streams").value);
  check(std::get<std::int64_t>(std::get<Dict>(results[0].value).at("streamID").value) == 12,
        "rolled-back generic stream ID was reused");
  setup.teardown_services(streams({video(111, 801)}));
  (void)setup.respond(streams({video(111, 901)}));
  rejects([&] { (void)setup.respond(streams({video(111, 801)})); });
}
void active_capacity() {
  AirplaySessionSetup setup;
  std::array<std::string, 8> services;
  for (unsigned index = 0; index < services.size(); ++index) {
    services[index] = "07D9F906-8D64-4B54-A808-A20BCA2C51C" + std::to_string(index);
    setup.register_data_service(services[index]);
  }
  sockaddr_in6 peer{};
  peer.sin6_family = AF_INET6;
  peer.sin6_addr = in6addr_loopback;
  peer.sin6_port = htons(9);
  setup.set_peer(peer);
  setup.set_secret(std::array<std::uint8_t, 32>{});
  setup.set_audio_sink("fakesink");
  (void)setup.respond(encode_binary_plist(Value{Dict{{"timingPort", number(9)}}}));
  (void)setup.respond(streams({video(110, 1), video(111, 2), iap(3)}));
  for (unsigned index = 0; index < services.size(); ++index)
    (void)setup.respond(streams({Value{Dict{{"type", number(130)},
                                            {"seed", number(4 + index)},
                                            {"clientTypeUUID", Value{services[index]}}}}}));
  const Value music{Dict{{"type", number(102)},
                         {"streamConnectionID", number(12)},
                         {"audioFormat", number(0x800000)},
                         {"audioType", Value{std::string("media")}}}};
  const Value voice{Dict{{"type", number(100)},
                         {"streamConnectionID", number(13)},
                         {"audioFormat", number(0x40000000)},
                         {"audioType", Value{std::string("speechRecognition")}}}};
  (void)setup.respond(streams({music}));
  const auto full_descriptors = descriptors();
  rejects([&] { (void)setup.respond(streams({voice})); });
  check(descriptors() == full_descriptors && setup.video_connection_id(111) == 2 &&
            setup.media_status().size() == 1,
        "over-capacity admission disturbed existing streams or allocated sockets");
  setup.teardown_services(streams({video(111, 2)}));
  (void)setup.respond(streams({voice}));
  check(setup.media_status().size() == 2 && setup.media_controls().size() == 1,
        "selective retirement did not release active capacity");
}
// Small synthetic image signatures exercise ownership, not image decoding.
void artwork_channel_retirement() {
  AirplaySessionSetup setup;
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  address.sin6_port = htons(9);
  setup.set_peer(address);
  const std::array<std::uint8_t, 32> secret{};
  setup.set_secret(secret);
  (void)setup.respond(encode_binary_plist(Value{Dict{{"timingPort", number(9)}}}));
  check(setup.start_record(), "artwork fixture RECORD failed");
  (void)setup.respond(streams({video(110, 80)}));
  Iap2Identity identity;
  identity.supports_now_playing = true;
  auto engine = std::make_shared<Iap2SessionEngine>(99, identity);
  const auto now = Iap2SessionEngine::Clock::now();
  engine->receive_bluetooth(Iap2SessionEngine::sync_preamble(), now);
  engine->receive_bluetooth(encode_link_frame({0xc0, 135, 99, 0, {1}}), now);
  setup.attach_iap_engine(engine);
  Peer original(data_port(setup.respond(streams({iap(81)}))));
  std::uint8_t sequence = 136;
  auto wire = [&](std::uint64_t seed, std::uint64_t counter, std::uint8_t session,
                  const std::vector<std::uint8_t>& payload) {
    const auto link = encode_link_frame({0x40, sequence++, 100, session, payload});
    std::vector<std::uint8_t> package(32 + link.size());
    for (unsigned index = 0; index < 4; ++index) {
      package[index] = static_cast<std::uint8_t>(package.size() >> ((3 - index) * 8));
    }
    package[16] = 'c';
    package[17] = 'o';
    package[18] = 'm';
    package[19] = 'm';
    std::copy(link.begin(), link.end(), package.begin() + 32);
    const auto key = hap_hkdf_sha512(secret, "DataStream-Salt" + std::to_string(seed),
                                     "DataStream-Output-Encryption-Key");
    std::array<char, 8> nonce{};
    for (unsigned index = 0; index < nonce.size(); ++index) {
      nonce[index] = static_cast<char>(counter >> (index * 8));
    }
    const std::array<std::uint8_t, 2> length{static_cast<std::uint8_t>(package.size()),
                                             static_cast<std::uint8_t>(package.size() >> 8)};
    auto sealed = hap_encrypt(key, {nonce.data(), nonce.size()}, package, length);
    std::vector<std::uint8_t> result(length.begin(), length.end());
    result.insert(result.end(), sealed.begin(), sealed.end());
    return result;
  };
  auto deliver = [&](const Peer& peer, const std::vector<std::uint8_t>& bytes) {
    // The isolated sender does not batch these tiny records with Nagle.
    const int no_delay = 1;
    check(setsockopt(peer.fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) == 0,
          "cannot configure isolated artwork peer");
    setup.drain();
    check(send(peer.fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(bytes.size()),
          "cannot send synthetic artwork record");
    setup.drain();
  };
  auto metadata = [](const std::vector<Iap2Parameter>& item,
                     const std::vector<Iap2Parameter>& playback = {}) {
    std::vector<Iap2Parameter> groups{{0, encode_iap2_parameters(item)}};
    if (!playback.empty()) groups.push_back({1, encode_iap2_parameters(playback)});
    return encode_control_message({0x5001, encode_iap2_parameters(groups)});
  };
  const auto first_item =
      metadata({{1, {'A', 0}}, {12, {'a', 0}}, {26, {7}}}, {{0, {2}}, {1, {0, 0, 0, 10}}});
  const auto old_record = wire(81, 0, 10, first_item);
  deliver(original, old_record);
  const std::vector<std::uint8_t> picture{7,  0xc0, 0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0,
                                          13, 'I',  'H',  'D', 'R', 0,   0,  0,  1,  0,  0, 0, 1};
  deliver(original, wire(81, 1, 12, picture));
  const auto leased = engine->now_playing()->snapshot();
  check(leased.artwork && leased.artwork->size() == 24 && leased.status == "paused",
        "initial synthetic artwork was not selected");
  std::vector<std::uint8_t> first{7, 0x80};
  first.insert(first.end(), picture.begin() + 2, picture.begin() + 14);
  deliver(original, wire(81, 2, 12, first));
  // EOF retires the channel/cipher, while the authenticated logical iAP link
  // and independent metadata remain. A fresh seed is mandatory for reopening.
  original.eof();
  setup.drain();
  rejects([&] { (void)setup.respond(streams({iap(81)})); });
  check(engine->established() && engine->now_playing()->snapshot().artwork == leased.artwork &&
            setup.recording(),
        "classic EOF changed the independent logical media session");
  Peer successor(data_port(setup.respond(streams({iap(82)}))));
  const auto replacement = metadata({{1, {'B', 0}}, {26, {7}}});
  deliver(successor, wire(82, 0, 10, replacement));
  auto current = engine->now_playing()->snapshot();
  check(current.title == "B" && current.artist.empty() && !current.position_ms &&
            current.status == "paused" && !current.artwork,
        "successor item retained old item state or invented playback");
  std::vector<std::uint8_t> tail{7, 0x40};
  tail.insert(tail.end(), picture.begin() + 14, picture.end());
  deliver(successor, wire(82, 1, 12, tail));
  check(!engine->now_playing()->snapshot().artwork,
        "old assembly completed against a successor item reusing its ID");
  // Malformed metadata cannot partly replace B or discard B's valid assembly.
  deliver(successor, wire(82, 2, 12, first));
  deliver(successor, wire(82, 3, 10, metadata({{1, {'C', 0}}, {12, {0xff}}})));
  check(engine->now_playing_rejections() == 1 && engine->now_playing()->snapshot().title == "B",
        "malformed successor metadata changed the selected item");
  deliver(successor, wire(82, 4, 12, tail));
  current = engine->now_playing()->snapshot();
  check(current.artwork && current.artwork != leased.artwork &&
            *current.artwork == *leased.artwork && current.title == "B" &&
            current.status == "paused" && !current.position_ms && setup.recording() &&
            setup.video_status().front().state == AirplaySessionSetup::ServiceState::negotiated,
        "fresh cover recovery damaged old backing or unrelated session state");
  const auto before_replay = engine->coverage();
  const auto messages_before_replay = setup.iap_messages();
  check(messages_before_replay == 8 && before_replay.delivered_sessions[10] == 3 &&
            before_replay.delivered_sessions[12] == 5,
        "encrypted artwork fixture did not traverse the production dispatch path");
  // A valid record sealed with the old channel key is not authority on the
  // successor socket. No bytes reach the logical link or replace its artwork.
  deliver(successor, old_record);
  check(setup.iap_messages() == messages_before_replay && engine->coverage() == before_replay &&
            engine->now_playing()->snapshot().artwork == current.artwork && leased.title == "A" &&
            *leased.artwork == *current.artwork && engine->established() && setup.recording(),
        "retired channel key acquired successor metadata authority");
}

}  // namespace

int main() try {
  using namespace headunit;
  artwork_channel_retirement();
  allocation_failures();
  active_capacity();
  AirplaySessionSetup setup;
  const std::string uuid = "07D9F906-8D64-4B54-A808-A20BCA2C51C2";
  setup.register_data_service(uuid);
  sockaddr_in6 peer{};
  peer.sin6_family = AF_INET6;
  peer.sin6_addr = in6addr_loopback;
  peer.sin6_port = htons(9);  // Isolated timing peer; no physical phone.
  setup.set_peer(peer);
  const std::array<std::uint8_t, 32> secret{};
  setup.set_secret(secret);
  const auto initial = decode_binary_plist(
      setup.respond(encode_binary_plist(Value{Dict{{"timingPort", number(9)}}})));
  const auto event_port =
      std::get<std::int64_t>(std::get<Dict>(initial.value).at("eventPort").value);
  const auto response =
      decode_binary_plist(setup.respond(streams({video(110, 50), video(111, 51), iap(52)})));
  check(std::get<Array>(std::get<Dict>(response.value).at("streams").value).size() == 3,
        "batch SETUP lost stream results");
  check(setup.start_record() && setup.start_record() && setup.recording(),
        "RECORD is not idempotent");
  const Value ui{
      Dict{{"type", number(130)}, {"seed", number(53)}, {"clientTypeUUID", Value{uuid}}}};
  rejects([&] { setup.respond(streams({ui, video(999, 54)})); });
  check(setup.service_status().empty() && setup.video_status().size() == 2,
        "malformed later stream partially allocated the batch");
  Value same_key = ui;
  std::get<Dict>(same_key.value)["seed"] = number(51);
  rejects([&] { setup.respond(streams({same_key})); });
  check(setup.service_status().empty(), "DataStream reused a video key namespace");

  const auto mode = command(
      "modesChanged",
      {{"resources", Value{Array{Value{Dict{{"resourceID", number(1)}, {"entity", number(1)}}}}}},
       {"appStates",
        Value{Array{Value{Dict{
            {"appStateID", number(1)}, {"entity", number(0)}, {"speechMode", number(-1)}}}}}}});
  check(setup.dispatch_command(mode) == 200 && setup.modes().revision == 1 &&
            setup.modes().apps[0] == AirplayAppMode{0, -1},
        "main dispatcher did not consume observed modes");
  AirplayEventMessage event{"POST", "/command", "RTSP/1.0", "1", mode, false};
  check(setup.dispatch_event_request(event) == 200 && setup.modes().revision == 2,
        "event and main command contracts differ");
  event.body = command("modesChanged", {{"resources", Value{true}}});
  check(setup.dispatch_event_request(event) == 400 && setup.modes().revision == 2,
        "malformed command changed mode observations");
  check(setup.dispatch_command(command("disableBluetooth")) == 200,
        "disableBluetooth acknowledgement lost");
  check(setup.dispatch_command(command("futureCommand")) == 501 &&
            setup.dispatch_command(command("duckAudio")) == 501,
        "unsupported or unavailable command reported success");
  event.path = "/unknown";
  check(setup.dispatch_event_request(event) == 501, "unknown event route reported success");
  event.path = "/feedback";
  check(setup.dispatch_event_request(event) == 200, "feedback keepalive lost");
  const auto stats = setup.command_diagnostics();
  check(stats.accepted == 3 && stats.unsupported == 1 && stats.unavailable == 1 &&
            stats.malformed == 1 && stats.unsupported_event_routes == 1,
        "content-free command diagnostics lost outcomes");

  const auto& initial_command_coverage = setup.command_coverage();
  const auto mode_category = static_cast<unsigned>(AirplayCommandKind::modes_changed);
  check(initial_command_coverage.known()[mode_category][0].received == 1 &&
            initial_command_coverage.known()[mode_category][1].received == 1 &&
            initial_command_coverage.known()[static_cast<unsigned>(AirplayCommandKind::count)][1]
                    .bad_request == 1,
        "command coverage merged channels or lost malformed results");
  for (unsigned i = 0; i < 49; ++i) {
    event.path = "/command";
    event.body = command("lateCommand" + std::to_string(i));
    check(setup.dispatch_event_request(event) == 501, "unknown command changed dispatch policy");
    event.path = "/late/" + std::to_string(i);
    check(setup.dispatch_event_request(event) == 501, "unknown route changed dispatch policy");
  }
  check(setup.command_coverage().evictions() == 18 &&
            setup.event_route_coverage().evictions() == 18 &&
            setup.command_coverage().known()[mode_category][0].ok == 1 &&
            setup.command_coverage().known()[mode_category][1].ok == 1 &&
            setup.command_coverage().errors() == 0 && setup.event_route_coverage().errors() == 0 &&
            setup.recording(),
        "late protocol coverage displaced known counts or disturbed healthy RECORD");

  rejects([&] {
    setup.teardown_services(
        streams({video(111, 51), Value{Dict{{"type", number(130)}, {"streamID", number(999)}}}}));
  });
  check(setup.video_status()[1].state == AirplaySessionSetup::ServiceState::negotiated,
        "malformed teardown closed an earlier selection");
  setup.teardown_services(streams({video(110, 50)}));
  check(setup.video_status()[0].state == AirplaySessionSetup::ServiceState::closed &&
            setup.video_status()[1].state == AirplaySessionSetup::ServiceState::negotiated &&
            setup.recording(),
        "selective video retirement changed unrelated transport authority");
  rejects([&] { setup.respond(streams({video(110, 50)})); });
  const auto replacement_video = setup.respond(streams({video(110, 60)}));
  setup.teardown_services(
      streams({video(111, 51), Value{Dict{{"type", number(130)}, {"streamID", number(1)}}}}));
  check(setup.video_status().back().state == AirplaySessionSetup::ServiceState::negotiated &&
            setup.recording(),
        "classic iAP close also closed the replacement video or RECORD");
  std::get<Dict>(same_key.value)["seed"] = number(52);
  rejects([&] { setup.respond(streams({same_key})); });
  (void)setup.respond(streams({ui}));

  // Even a diagnostic video sink has one accepted TCP owner. EOF closes the
  // transport and permits a new SETUP with fresh keys, retaining other streams.
  {
    Peer video_peer(data_port(replacement_video));
    setup.drain();
    video_peer.eof();
    setup.drain();
    check(setup.video_status().back().state == AirplaySessionSetup::ServiceState::closed &&
              setup.recording(),
          "video EOF without a decoder retained an active old transport");
    rejects([&] { setup.respond(streams({video(110, 60)})); });
    (void)setup.respond(streams({video(110, 70)}));
  }
  {
    Peer classic(data_port(setup.respond(streams({iap(61)}))));
    const auto key =
        hap_hkdf_sha512(secret, "DataStream-Salt61", "DataStream-Output-Encryption-Key");
    const std::array<std::uint8_t, 2> length{};
    const std::array<char, 8> nonce{};
    auto sealed = hap_encrypt(key, {nonce.data(), nonce.size()}, {}, length);
    sealed.back() ^= 1;
    std::vector<std::uint8_t> wire(length.begin(), length.end());
    wire.insert(wire.end(), sealed.begin(), sealed.end());
    check(send(classic.fd, wire.data(), wire.size(), MSG_NOSIGNAL) ==
              static_cast<ssize_t>(wire.size()),
          "cannot send malformed iAP record");
    setup.drain();
    rejects([&] {
      setup.teardown_services(
          streams({Value{Dict{{"type", number(130)}, {"streamID", number(1)}}}}));
    });
    rejects([&] { setup.respond(streams({iap(61)})); });
    (void)setup.respond(streams({iap(62)}));
    check(setup.video_status().back().state == AirplaySessionSetup::ServiceState::negotiated &&
              setup.recording(),
          "classic iAP authentication retirement also retired healthy video/RECORD");
  }

  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  check(fd >= 0, "cannot create isolated event peer");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<std::uint16_t>(event_port));
  check(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "cannot connect event peer");
  setup.drain();
  check(setup.start_record(), "event listener retirement broke existing RECORD");
  close(fd);
  bool retired = false;
  for (unsigned i = 0; i < 4 && !retired; ++i)
    try {
      setup.drain();
    } catch (const std::runtime_error&) {
      retired = true;
    }
  check(retired, "event EOF permitted a new cipher owner with old keys");
  check(setup.modes().revision == 0 && !setup.modes().observed_at && !setup.modes().resources[0],
        "terminal event generation retained resource authority");
  bool dead_command = false;
  try {
    (void)setup.dispatch_command(mode);
  } catch (const std::runtime_error&) {
    dead_command = true;
  }
  check(dead_command && setup.modes().revision == 0,
        "retained session reference revived retired resource modes");
  bool dead_setup = false;
  try {
    (void)setup.respond(streams({video(110, 70)}));
  } catch (const std::runtime_error&) {
    dead_setup = true;
  }
  check(dead_setup && !setup.start_record(),
        "retained session reopened a retired event generation");
  std::cout << "real socket batch setup, common commands, selective retirement, key isolation and "
               "event EOF passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
