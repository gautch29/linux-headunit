// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_session.hpp>
#include <headunit/theme/camera_state.hpp>
#include <headunit/binary_plist.hpp>
#include <headunit/airplay_info.hpp>
#include <array>
#include <chrono>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
using namespace headunit;
namespace v = headunit::theme::vehicle;
namespace op = headunit::theme::opack;
using Bytes = std::vector<std::uint8_t>;
using Dict = PlistValue::Dictionary;
using Array = PlistValue::Array;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
const auto secret = std::array<std::uint8_t, 32>{};
std::string nonce(std::uint64_t n) {
  std::string s(8, 0);
  for (unsigned i = 0; i < 8; ++i)
    s[i] = char(n >> (8 * i));
  return s;
}
std::uint64_t integer(const Dict& d, const std::string& key) {
  return std::get<std::int64_t>(d.at(key).value);
}
struct Peer {
  int fd = -1;
  std::uint64_t seed = 0, id = 0, send_nonce = 0, read_nonce = 0;
  ~Peer() {
    if (fd >= 0) close(fd);
  }
  void send_request(const std::string& name, std::optional<op::Value> values = {}) {
    const auto payload = v::encode_outgoing({name, 1, values, {}, {}}, 0);
    Bytes plain(32 + payload.size());
    for (unsigned i = 0; i < 4; ++i)
      plain[i] = std::uint8_t(plain.size() >> ((3 - i) * 8));
    plain[4] = 's';
    plain[5] = 'y';
    plain[6] = 'n';
    plain[7] = 'c';
    plain[16] = 'c';
    plain[17] = 'o';
    plain[18] = 'm';
    plain[19] = 'm';
    plain[27] = 42;
    std::copy(payload.begin(), payload.end(), plain.begin() + 32);
    Bytes header{std::uint8_t(plain.size()), std::uint8_t(plain.size() >> 8)};
    const auto key = hap_hkdf_sha512(secret, "DataStream-Salt" + std::to_string(seed),
                                     "DataStream-Output-Encryption-Key");
    const auto sealed = hap_encrypt(key, nonce(send_nonce++), plain, header);
    header.insert(header.end(), sealed.begin(), sealed.end());
    CHECK(send(fd, header.data(), header.size(), 0) == ssize_t(header.size()));
  }
  Bytes read_message() {
    std::array<std::uint8_t, 2> header{};
    CHECK(recv(fd, header.data(), 2, MSG_WAITALL) == 2);
    Bytes sealed(static_cast<std::size_t>(header[0]) + (static_cast<std::size_t>(header[1]) << 8) +
                 16);
    CHECK(recv(fd, sealed.data(), sealed.size(), MSG_WAITALL) == ssize_t(sealed.size()));
    const auto key = hap_hkdf_sha512(secret, "DataStream-Salt" + std::to_string(seed),
                                     "DataStream-Input-Encryption-Key");
    auto clear = hap_decrypt(key, nonce(read_nonce++), sealed, header);
    CHECK(clear && clear->size() >= 32);
    return *clear;
  }
};
void open(AirplaySessionSetup& setup, Peer& p, const char* uuid, std::uint64_t seed) {
  const auto request = encode_binary_plist(PlistValue{Dict{
      {"streams",
       PlistValue{Array{PlistValue{Dict{{"type", PlistValue{std::int64_t(130)}},
                                        {"seed", PlistValue{std::int64_t(seed)}},
                                        {"clientTypeUUID", PlistValue{std::string(uuid)}}}}}}}}});
  auto reply = decode_binary_plist(setup.respond(request));
  const auto& stream = std::get<Dict>(
      std::get<Array>(std::get<Dict>(reply.value).at("streams").value).front().value);
  p.id = integer(stream, "streamID");
  p.seed = seed;
  p.fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(p.fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(integer(stream, "dataPort"));
  CHECK(connect(p.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  timeval timeout{2, 0};
  CHECK(setsockopt(p.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
}
void until(auto f) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!f()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
int main() try {
  AirplayIdentity identity{};
  identity.name = "Bench";
  identity.model = "Independent";
  identity.manufacturer = "Independent";
  identity.device_id = "02:11:22:33:44:55";
  identity.source_version = "950.7.1";
  identity.main_display_id = "BBBBBBBB-BBBB-4BBB-8BBB-BBBBBBBBBBBB";
  identity.cluster_display_id = "CCCCCCCC-CCCC-4CCC-8CCC-CCCCCCCCCCCC";
  identity.features = 0x0000006144540380ULL;
  identity.flags = 4;
  v::Bench info_bench(v::BenchProfile::combined);
  info_bench.refresh(1000);
  const auto base_info =
      make_airplay_info(identity, {1920, 720, 60, 155, 58}, {1920, 720, 60, 200, 75}, true,
                        "Center_Display", "Cluster_Display", true);
  (void)v::add_server_info(base_info, info_bench.service(), 1000);

  AirplaySessionSetup setup;
  v::Session owner(v::BenchProfile::combined);
  bool rejected = false;
  try {
    setup.enable_vehicle_state();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  CHECK(rejected);
  owner.attach(setup);
  setup.set_secret(secret);
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  address.sin6_port = htons(12345);
  setup.set_peer(address);
  const auto offer = encode_binary_plist(PlistValue{
      Dict{{"timingPort", PlistValue{std::int64_t(12345)}},
           {"features", PlistValue{Array{PlistValue{std::string("vehicleStateProtocol")}}}}}});
  const auto initial = decode_binary_plist(setup.respond(offer));
  CHECK(std::get<Array>(std::get<Dict>(initial.value).at("enabledFeatures").value).back() ==
        PlistValue{std::string("vehicleStateProtocol")});
  Peer normal, high;
  open(setup, normal, v::normal_service, 123);
  open(setup, high, v::high_service, 124);
  until([&] {
    setup.drain();
    owner.pump(setup, 1000);
    return owner.status().channels == 2;
  });
  normal.send_request("configRequest");
  until([&] {
    setup.drain();
    owner.pump(setup, 1000);
    if (!owner.failure_reason().empty()) throw std::runtime_error(owner.failure_reason());
    return owner.status().received == 1;
  });
  setup.drain();
  const auto receipt = normal.read_message();
  CHECK(receipt.size() == 32 && receipt[4] == 'r' && receipt[27] == 42);
  auto response = normal.read_message();
  CHECK(response[4] == 'a');
  auto config = v::decode_incoming(std::span(response).subspan(32)).command;
  CHECK(config.name == "configResponse" && config.values);
  CHECK(owner.status().requests.configuration == 1 && owner.status().requests.other == 0);
  high.send_request("writeRequest",
                    op::Value{op::Dictionary{{op::Value{std::int64_t(202)}, op::Value{true}}}});
  until([&] {
    setup.drain();
    owner.pump(setup, 1001);
    return owner.status().writes == 1;
  });
  setup.drain();
  CHECK(owner.status().simulated_comfort_lighting);
  (void)high.read_message();
  response = high.read_message();
  CHECK((v::decode_incoming(std::span(response).subspan(32)).command.values ==
         op::Value{op::Dictionary{{op::Value{std::int64_t(202)}, op::Value{true}}}}));
  CHECK(setup.close_service(normal.id));
  owner.pump(setup, 1002);
  CHECK(owner.status().channels == 1);
  CHECK(owner.status().received == 2 && owner.status().requests.configuration == 1);
  CHECK(setup.close_service(high.id));
  owner.pump(setup, 1003);
  CHECK(owner.status().channels == 0);
  CHECK(owner.status().received == 2 && owner.status().writes == 1);
  Peer again;
  open(setup, again, v::normal_service, 125);
  until([&] {
    setup.drain();
    owner.pump(setup, 1004);
    return owner.status().channels == 1;
  });
  again.send_request("readRequest", op::Value{op::Array{op::Value{std::int64_t(202)}}});
  until([&] {
    setup.drain();
    owner.pump(setup, 1004);
    return owner.status().received == 3;
  });
  setup.drain();
  (void)again.read_message();
  response = again.read_message();
  CHECK((v::decode_incoming(std::span(response).subspan(32)).command.values ==
         op::Value{op::Dictionary{{op::Value{std::int64_t(202)}, op::Value{true}}}}));
  CHECK(owner.failure_reason().empty());
  CHECK(owner.status().requests.read == 1 && owner.status().requests.configuration == 1);
  CHECK(owner.coverage()->summary().channels[0].received == 2 &&
        owner.coverage()->summary().channels[1].writes == 1);
  const auto rows = owner.coverage()->resources();
  CHECK(rows.size() == 1 && rows.front().iid == 202 && rows.front().service_iid == 201 &&
        rows.front().service_type == 0x16000001 && rows.front().channels[0].reads == 1 &&
        rows.front().channels[1].confirmed_writes == 1 &&
        rows.front().channels[1].transferred_values == 1);
  {
    // A separate real encrypted transport session with synthetic acquisition:
    // registration and On/Off transfer evidence must identify both camera IIDs.
    const auto config = v::parse_configuration(
        "", "", R"({"version":1,"experimental":true,"stream_identifier":"camera",
      "general":{"on":{"binding":"camera.visible","max_age_ms":151,"writable":false,"initial":false}}})");
    const auto c = *config.camera;
    auto state = std::make_shared<v::CameraStateSnapshot>();
    auto provider = v::camera_state_provider(v::simulated_provider(config), c, state);
    AirplaySessionSetup camera_setup;
    v::Session camera_owner(config, provider);
    camera_owner.attach(camera_setup);
    camera_setup.set_secret(secret);
    camera_setup.set_peer(address);
    (void)camera_setup.respond(offer);
    Peer camera_normal, camera_high;
    open(camera_setup, camera_normal, v::normal_service, 201);
    open(camera_setup, camera_high, v::high_service, 202);
    until([&] {
      camera_setup.drain();
      camera_owner.pump(camera_setup, 0);
      return camera_owner.status().channels == 2;
    });
    camera_high.send_request("registerRequest", op::Value{std::string("*")});
    until([&] {
      camera_setup.drain();
      camera_owner.pump(camera_setup, 0);
      return camera_owner.status().received == 1;
    });
    camera_setup.drain();
    (void)camera_high.read_message();
    const auto initial_camera = camera_high.read_message();
    CHECK(v::decode_incoming(std::span(initial_camera).subspan(32)).command.name ==
          "registerResponse");
    const auto registration_status = camera_owner.status();
    for (const auto& resource : *registration_status.camera_resources)
      CHECK(resource.registrations == 1 && resource.responses_off == 1 && resource.updates_on == 0);
    const auto generation = state->begin(10000000);
    CHECK(state->observe(generation, true, 12000000, 13000000));
    until([&] {
      camera_setup.drain();
      camera_owner.pump(camera_setup, 13);
      pollfd ready{camera_normal.fd, POLLIN, 0};
      return poll(&ready, 1, 0) > 0 && (ready.revents & POLLIN);
    });
    auto packet = camera_normal.read_message();
    auto notify = v::decode_incoming(std::span(packet).subspan(32)).command;
    CHECK(notify.name == "updateNotify");
    const auto both = [&](bool on) {
      return op::Value{op::Dictionary{{op::Value{c.input_stream_on_iid}, op::Value{on}},
                                      {op::Value{c.camera_general_on_iid}, op::Value{on}}}};
    };
    CHECK(notify.values == both(true));
    const auto on_status = camera_owner.status();
    for (const auto& resource : *on_status.camera_resources)
      CHECK(resource.updates_on == 1 && resource.updates_off == 0);
    CHECK(state->observe(generation, false, 0, 14000000));
    until([&] {
      camera_setup.drain();
      camera_owner.pump(camera_setup, 14);
      pollfd ready{camera_normal.fd, POLLIN, 0};
      return poll(&ready, 1, 0) > 0 && (ready.revents & POLLIN);
    });
    packet = camera_normal.read_message();
    notify = v::decode_incoming(std::span(packet).subspan(32)).command;
    CHECK(notify.name == "updateNotify" && notify.values == both(false));
    const auto off_status = camera_owner.status();
    for (const auto& resource : *off_status.camera_resources)
      CHECK(resource.updates_on == 1 && resource.updates_off == 1);
    CHECK(camera_owner.failure_reason().empty());
  }
  {
    // Explicit synthetic writable topology exercises the production encrypted
    // session seam. Parsed camera declarations remain read-only in production.
    auto config = v::parse_configuration(
        "", "", R"({"version":1,"experimental":true,"stream_identifier":"camera",
      "general":{"on":{"binding":"camera.visible","max_age_ms":151,"writable":false,"initial":false}}})");
    const auto c = *config.camera;
    for (auto& signal : config.signals)
      if (signal.policy.iid == c.camera_general_on_iid) signal.writable = true;
    auto field = [](op::Value& value, std::string_view name) -> op::Value& {
      for (auto& [key, item] : std::get<op::Dictionary>(value.data))
        if (key == op::Value{std::string(name)}) return item;
      throw std::runtime_error("fixture field absent");
    };
    for (auto& accessory : std::get<op::Array>(field(config.profile, "accessories").data))
      for (auto& service : std::get<op::Array>(field(accessory, "services").data))
        for (auto& characteristic : std::get<op::Array>(field(service, "characteristics").data))
          if (field(characteristic, "iid") == op::Value{c.camera_general_on_iid})
            field(characteristic, "writable") = op::Value{true};
    auto state = std::make_shared<v::CameraStateSnapshot>();
    std::uint64_t now_ns = 13000000;
    const auto generation = state->begin(10000000);
    CHECK(state->observe(generation, true, 12000000, now_ns));
    auto provider =
        v::camera_state_provider(v::simulated_provider(config), c, state, [&] { return now_ns; });
    AirplaySessionSetup confirmed_setup;
    v::Session confirmed_owner(config, provider);
    confirmed_owner.attach(confirmed_setup);
    confirmed_setup.set_secret(secret);
    confirmed_setup.set_peer(address);
    (void)confirmed_setup.respond(offer);
    Peer origin, sibling;
    open(confirmed_setup, origin, v::normal_service, 301);
    open(confirmed_setup, sibling, v::high_service, 302);
    until([&] {
      confirmed_setup.drain();
      confirmed_owner.pump(confirmed_setup, 13);
      return confirmed_owner.status().channels == 2;
    });
    origin.send_request(
        "writeRequest",
        op::Value{op::Dictionary{{op::Value{c.camera_general_on_iid}, op::Value{false}}}});
    now_ns = 14000000;
    until([&] {
      confirmed_setup.drain();
      confirmed_owner.pump(confirmed_setup, 14);
      return confirmed_owner.status().writes == 1;
    });
    confirmed_setup.drain();
    CHECK(origin.read_message().size() == 32);
    pollfd waiting{origin.fd, POLLIN, 0};
    CHECK(poll(&waiting, 1, 0) == 0);  // Receipt is not writeResponse.
    const auto request = *state->pending_off(generation, now_ns);
    CHECK(state->owner_off(generation, request.sequence, 15000000));
    CHECK(
        state->presented_off(generation, request.sequence, 1, 1, 1, 16000000, 17000000, 18000000));
    now_ns = 19000000;
    until([&] {
      confirmed_setup.drain();
      confirmed_owner.pump(confirmed_setup, 19);
      pollfd ready{origin.fd, POLLIN, 0};
      return poll(&ready, 1, 0) > 0 && (ready.revents & POLLIN);
    });
    const auto reply = origin.read_message();
    const auto command = v::decode_incoming(std::span(reply).subspan(32)).command;
    CHECK((command.name == "writeResponse" && command.transaction == 1 && !command.errors &&
           command.values ==
               op::Value{op::Dictionary{{op::Value{c.camera_general_on_iid}, op::Value{false}}}}));
    pollfd unrelated{sibling.fd, POLLIN, 0};
    CHECK(poll(&unrelated, 1, 0) == 0);
    // Losing only the origin retires its pending owner operation, even while
    // the other priority channel and all unrelated session consumers survive.
    origin.send_request(
        "writeRequest",
        op::Value{op::Dictionary{{op::Value{c.camera_general_on_iid}, op::Value{false}}}});
    now_ns = 20000000;
    until([&] {
      confirmed_setup.drain();
      confirmed_owner.pump(confirmed_setup, 20);
      return confirmed_owner.status().writes == 2;
    });
    const auto abandoned = *state->pending_off(generation, now_ns);
    CHECK(confirmed_setup.close_service(origin.id));
    now_ns = 21000000;
    confirmed_owner.pump(confirmed_setup, 21);
    CHECK(confirmed_owner.status().channels == 1 &&
          state->off_result(generation, abandoned.sequence, now_ns)->status ==
              v::CameraStateSnapshot::OffStatus::retired);
    CHECK(!state->owner_off(generation, abandoned.sequence, 22000000));
    CHECK(confirmed_owner.failure_reason().empty());
  }
  std::cout << "Vehicle session: feature offer, encrypted dual channels, applied writes and "
               "reconnect passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
