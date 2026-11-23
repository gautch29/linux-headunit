// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/receiver_video.hpp>
#include <headunit/theme/gpu_display.hpp>
#include <headunit/binary_plist.hpp>
#include <plist/plist.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
using namespace headunit;
using namespace headunit::theme;
using namespace std::chrono_literals;
using Dict = PlistValue::Dictionary;
using Array = PlistValue::Array;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
std::uint64_t ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::uint64_t ntp(std::uint64_t elapsed) {
  return (std::uint64_t{900} << 32) + (elapsed / 1000000000 << 32) +
         ((elapsed % 1000000000) << 32) / 1000000000;
}
template <class F>
void until(F f) {
  const auto end = ns() + 5000000000ULL;
  while (!f()) {
    CHECK(ns() < end);
    std::this_thread::sleep_for(2ms);
  }
}
struct Peer {
  int fd = -1;
  ~Peer() {
    if (fd >= 0) close(fd);
  }
};
void open(AirplaySessionSetup& session, Peer& peer, std::uint64_t type, std::uint64_t id) {
  if (peer.fd >= 0) close(peer.fd);
  peer.fd = -1;
  auto request = encode_binary_plist(
      PlistValue{Dict{{"streams", PlistValue{Array{PlistValue{Dict{
                                      {"type", PlistValue{std::int64_t(type)}},
                                      {"streamConnectionID", PlistValue{std::int64_t(id)}}}}}}}}});
  auto reply = session.respond(request);
  plist_t root = nullptr;
  CHECK(plist_from_bin(reinterpret_cast<const char*>(reply.data()), reply.size(), &root) ==
        PLIST_ERR_SUCCESS);
  std::uint64_t port = 0;
  plist_get_uint_val(plist_dict_get_item(
                         plist_array_get_item(plist_dict_get_item(root, "streams"), 0), "dataPort"),
                     &port);
  plist_free(root);
  peer.fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(peer.fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  CHECK(connect(peer.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  session.drain();
}
void send_packet(int fd, std::uint64_t connection_id, const AirplayVideoPacket& packet,
                 std::uint64_t nonce) {
  std::array<std::uint8_t, 128> header{};
  header[4] = packet.kind == AirplayVideoPacket::Kind::config ? 1 : 0;
  header[6] = 2;
  const auto size = packet.data.size() + (header[4] ? 0 : 16);
  for (unsigned i = 0; i < 4; ++i)
    header[i] = std::uint8_t(size >> (i * 8));
  for (unsigned i = 0; i < 8; ++i)
    header[8 + i] = std::uint8_t(packet.timestamp >> (i * 8));
  std::vector<std::uint8_t> wire(header.begin(), header.end());
  if (header[4])
    wire.insert(wire.end(), packet.data.begin(), packet.data.end());
  else {
    const std::array<std::uint8_t, 32> secret{};
    std::array<char, 8> iv{};
    for (unsigned i = 0; i < 8; ++i)
      iv[i] = char(nonce >> (i * 8));
    const auto sealed = hap_encrypt(derive_airplay_video_key(secret, connection_id),
                                    {iv.data(), iv.size()}, packet.data, header);
    wire.insert(wire.end(), sealed.begin(), sealed.end());
  }
  CHECK(send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) == ssize_t(wire.size()));
}
int main() try {
  std::array<std::uint8_t, 32> key{};
  std::cin.read(reinterpret_cast<char*>(key.data()), key.size());
  CHECK(std::cin);
  AirplayVideoStream parser(key);
  std::vector<AirplayVideoPacket> packets;
  std::array<std::uint8_t, 4096> bytes{};
  while (std::cin.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || std::cin.gcount())
    for (auto& p : parser.feed(std::span(bytes).first(std::cin.gcount()))) {
      CHECK(packets.size() < 32);
      packets.push_back(std::move(p));
    }
  CHECK(!parser.partial() && packets.size() >= 10);
  AirplaySessionSetup session;
  const std::array<std::uint8_t, 32> secret{};
  session.set_secret(secret);
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  address.sin6_port = htons(12345);
  session.set_peer(address);
  (void)session.respond(
      encode_binary_plist(PlistValue{Dict{{"timingPort", PlistValue{std::int64_t{12345}}}}}));
  video::ReceiverVideo owner;
  owner.attach(session);
  auto queues = owner.claim();
  bool rejected = false;
  try {
    (void)owner.claim();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  CHECK(rejected);
  Peer main, cluster;
  open(session, main, 110, 110);
  open(session, cluster, 111, 111);
  CHECK(session.start_record());
  const auto origin = ns();
  auto source = [&]() -> std::optional<AirplayClockSample> {
    const auto at = ns();
    return AirplayClockSample{ntp(at - origin) + (std::uint64_t{2208988800} << 32), at, at, 1000};
  };
  script::Display spec{"native-receiver-fixture", 1280, 720};
  script::Runtime runtime(
      [](std::string_view) -> std::string { throw std::runtime_error("no resource"); }, spec);
  auto stream = runtime.add_video_input({"iPhoneVideo", 1280, 720});
  auto input = std::make_shared<video::NativeInput>(stream, queues[0], "v4l2slh265dec");
  // A second source uses the actual cluster router and its own hardware decoder.
  auto second_stream = runtime.add_video_input({"cluster", 1280, 720});
  auto second_input =
      std::make_shared<video::NativeInput>(second_stream, queues[1], "v4l2slh265dec");
  graphics::GpuDisplayOptions options;
  options.inputs = {input, second_input};
  if (std::getenv("HEADUNIT_THEME_TEST_TWO_PENDING")) options.window.max_pending_presentations = 2;
  if (std::getenv("HEADUNIT_THEME_TEST_DEFER_FENCE")) options.window.defer_scene_fence = true;
  bool retire_during_composition = false, retired_during_composition = false;
  options.slow_frame_threshold_ns = 1;
  options.slow_frame = [&](const graphics::GpuSlowFrameTrace&) {
    const auto token = stream->composition_token();
    if (!retire_during_composition || !token || !stream->prepared(*token)) return;
    retire_during_composition = false;
    session.teardown_services(encode_binary_plist(PlistValue{
        Dict{{"streams", PlistValue{Array{PlistValue{
                             Dict{{"type", PlistValue{std::int64_t(110)}},
                                  {"streamConnectionID", PlistValue{std::int64_t(110)}}}}}}}}}));
    owner.pump(session);
    retired_during_composition = true;
  };
  graphics::GpuDisplay display(runtime, spec, source, options);
  script::UiWorker::DisplayTurn turn;
  until([&] {
    turn = display.turn();
    return turn.allow_tasks;
  });
  runtime.tick(turn.pts);
  runtime.evaluate(R"(
    root=CP.DPLayer.create();root.frame=CP.DPRect(0,0,1280,720)
    movie=CP.DPVideoLayer.create('iPhoneVideo');movie.frame=CP.DPRect(0,0,640,720);movie:subscribe()
    second=CP.DPVideoLayer.create('cluster');second.frame=CP.DPRect(640,0,640,720);second:subscribe()
    root:addSublayer(movie);root:addSublayer(second)
    CP.setRootLayer('native-receiver-fixture',root)
  )");
  runtime.start_tasks(turn.pts);
  auto tick = [&] {
    session.drain();
    owner.pump(session);
    turn = display.turn();
    if (turn.allow_tasks) runtime.tick(turn.pts);
  };
  send_packet(main.fd, 110, packets[0], 0);
  send_packet(cluster.fd, 111, packets[0], 0);
  std::uint64_t frames = 0;
  for (std::size_t i = 1; i < packets.size(); ++i) {
    auto packet = packets[i];
    if (packet.kind != AirplayVideoPacket::Kind::frame) continue;
    // Fixture-only retiming. Production router preserves exact source timestamps.
    const auto due = ns() + 150000000;
    packet.timestamp = ntp(due - origin);
    packet.sender_ntp = packet.timestamp;
    send_packet(main.fd, 110, packet, frames);
    send_packet(cluster.fd, 111, packet, frames);
    ++frames;
    until([&] {
      tick();
      return input->stats().presented == frames && second_input->stats().presented == frames;
    });
    CHECK(ns() >= due && stream->current_pts() == second_stream->current_pts());
    CHECK(stream->current_pts() ==
          double(packet.timestamp >> 32) + double(packet.timestamp & 0xffffffffULL) / 4294967296.);
  }
  CHECK(frames >= 9 && input->stats().decoded == frames && second_input->stats().decoded == frames);
  close(cluster.fd);
  cluster.fd = -1;
  until([&] {
    tick();
    return !queues[1]->available() && !second_stream->current_pts();
  });
  CHECK(!owner.closed() && queues[0]->available() && !queues[1]->closed() && session.recording());
  auto packet = packets[1];
  const auto retime = [&] {
    packet.timestamp = ntp(ns() + 150000000 - origin);
    packet.sender_ntp = packet.timestamp;
  };
  retime();
  send_packet(main.fd, 110, packet, frames);
  until([&] {
    tick();
    return input->stats().presented == frames + 1;
  });
  CHECK(second_input->stats().presented == frames && !second_stream->current_pts());

  open(session, cluster, 111, 211);
  send_packet(cluster.fd, 211, packets[0], 0);
  retime();
  send_packet(cluster.fd, 211, packet, 0);
  until([&] {
    tick();
    return second_input->stats().presented == frames + 1;
  });
  CHECK(queues[1]->available() && !owner.closed() && session.recording());

  // Retire main after GPU composition with a real prepared source token.
  // The display must redraw without it, retain the healthy cluster's receipt,
  // and ignore older source receipts even with two deferred submissions.
  retire_during_composition = true;
  retime();
  send_packet(main.fd, 110, packet, frames + 1);
  send_packet(cluster.fd, 211, packet, 1);
  until([&] {
    tick();
    return retired_during_composition && !stream->current_pts() &&
           second_input->stats().presented == frames + 2;
  });
  CHECK(input->stats().presented == frames + 1 && input->stats().discarded > 0 &&
        !queues[0]->available() && queues[1]->available() && !owner.closed() &&
        session.recording());

  open(session, main, 110, 210);
  send_packet(main.fd, 210, packets[0], 0);
  retime();
  send_packet(main.fd, 210, packet, 0);
  until([&] {
    tick();
    return input->stats().presented == frames + 2;
  });
  CHECK(queues[0]->available() && !owner.closed() && session.recording() &&
        input->stats().generation_changes >= 2 && second_input->stats().generation_changes >= 2);
  owner.close();
  CHECK(queues[0]->closed() && queues[1]->closed());
  bool failed = false;
  try {
    (void)display.turn();
  } catch (const std::exception&) {
    failed = true;
  }
  CHECK(failed && !stream->current_pts() && !second_stream->current_pts());
  std::cout << "{\"nativePresented\":" << frames
            << ",\"receiverRouting\":true,\"twoSources\":true,\"actualWaylandFeedback\":true,"
               "\"syntheticClock\":true,\"mainAfterClusterEof\":true,\"freshClusterReopen\":true,"
               "\"clusterAfterMainTeardown\":true,\"freshMainReopen\":true,"
               "\"retiredDuringComposition\":true,\"controlRecordPreserved\":true,"
               "\"disconnectWithdrawn\":true}\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
