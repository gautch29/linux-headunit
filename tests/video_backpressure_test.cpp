// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/airplay_session_setup.hpp>
#include <headunit/binary_plist.hpp>
#include <plist/plist.h>
#include <array>
#include <iostream>
#include <thread>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
using namespace headunit;
using Dict = PlistValue::Dictionary;
using Array = PlistValue::Array;
using namespace std::chrono_literals;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
struct Peer {
  int fd = -1;
  ~Peer() {
    if (fd >= 0) close(fd);
  }
};
void open(AirplaySessionSetup& session, Peer& peer, std::uint64_t type) {
  auto request = encode_binary_plist(PlistValue{
      Dict{{"streams", PlistValue{Array{PlistValue{
                           Dict{{"type", PlistValue{std::int64_t(type)}},
                                {"streamConnectionID", PlistValue{std::int64_t(type)}}}}}}}}});
  const auto reply = session.respond(request);
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
std::vector<std::uint8_t> packets(std::uint64_t type, unsigned count, bool config = true,
                                  unsigned first = 0) {
  const std::array<std::uint8_t, 32> secret{};
  const auto key = derive_airplay_video_key(secret, type);
  std::vector<std::uint8_t> wire;
  if (config) {
    std::array<std::uint8_t, 128> header{};
    header[0] = 1;
    header[4] = 1;
    header[6] = 2;
    wire.insert(wire.end(), header.begin(), header.end());
    wire.push_back(42);
  }
  for (unsigned n = first; n < first + count; ++n) {
    std::array<std::uint8_t, 128> header{};
    header[0] = 20;
    for (unsigned i = 0; i < 8; ++i)
      header[8 + i] = std::uint8_t(std::uint64_t(n + 1) >> (8 * i));
    std::array<char, 8> nonce{};
    for (unsigned i = 0; i < 8; ++i)
      nonce[i] = char(std::uint64_t(n) >> (8 * i));
    const std::array<std::uint8_t, 4> body{std::uint8_t(n), std::uint8_t(n >> 8), 17, 19};
    auto sealed = hap_encrypt(key, {nonce.data(), nonce.size()}, body, header);
    wire.insert(wire.end(), header.begin(), header.end());
    wire.insert(wire.end(), sealed.begin(), sealed.end());
  }
  return wire;
}
void send_all(int fd, const std::vector<std::uint8_t>& bytes) {
  CHECK(send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) == ssize_t(bytes.size()));
}
template <class F>
void until(F f, std::chrono::seconds limit = 3s) {
  auto end = std::chrono::steady_clock::now() + limit;
  while (!f()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::sleep_for(1ms);
  }
}
void cluster_startup_ownership() {
  constexpr const char* display = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
  const std::array<std::uint8_t, 32> secret{};
  for (const bool native : {false, true}) {
    AirplaySessionSetup session;
    session.set_secret(secret);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    address.sin6_port = htons(12345);
    session.set_peer(address);
    unsigned received = 0;
    session.set_video_consumer([&](auto, auto&) {
      ++received;
      return true;
    });
    session.register_data_service("07D9F906-8D64-4B54-A808-A20BCA2C51C2");
    if (native) session.enable_native_ultra();
    const auto response = session.respond(encode_binary_plist(PlistValue{Dict{
        {"timingPort", PlistValue{std::int64_t{12345}}},
        {"features",
         PlistValue{Array{PlistValue{std::string("uiSync")}, PlistValue{std::string("hevc")}}}}}}));
    const auto decoded = decode_binary_plist(response);
    const auto& fields = std::get<Dict>(decoded.value);
    Peer event;
    event.fd = socket(AF_INET6, SOCK_STREAM, 0);
    CHECK(event.fd >= 0);
    address.sin6_port = htons(std::get<std::int64_t>(fields.at("eventPort").value));
    CHECK(connect(event.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    session.drain();
    CHECK(session.start_record());
    Peer video;
    open(session, video, 111);
    send_all(video.fd, packets(111, 1));
    until([&] {
      session.drain();
      return received == 2;
    });
    CHECK(session.recording());
    CHECK(session.show_cluster_map(display) == !native);
    session.drain();
    std::array<std::uint8_t, 2> length{};
    if (native) {
      // Even with a live event channel and accepted cluster frames, Ultra
      // must not receive an unsolicited legacy map selection.
      CHECK(recv(event.fd, length.data(), length.size(), MSG_DONTWAIT) == -1);
      CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    } else {
      timeval timeout{2, 0};
      CHECK(setsockopt(event.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
      CHECK(recv(event.fd, length.data(), length.size(), MSG_WAITALL) == 2);
      std::vector<std::uint8_t> sealed((unsigned(length[1]) << 8) + length[0] + 16);
      CHECK(recv(event.fd, sealed.data(), sealed.size(), MSG_WAITALL) == ssize_t(sealed.size()));
      const auto key = hap_hkdf_sha512(secret, "Events-Salt", "Events-Write-Encryption-Key");
      const std::array<char, 8> nonce{};
      const auto plain = hap_decrypt(key, {nonce.data(), nonce.size()}, sealed, length);
      CHECK(plain);
      const std::string text(plain->begin(), plain->end());
      const auto body = text.find("\r\n\r\n");
      CHECK(body != text.npos);
      const auto command = decode_binary_plist(std::span(*plain).subspan(body + 4));
      const auto& values = std::get<Dict>(command.value);
      CHECK(std::get<std::string>(values.at("type").value) == "showUI");
      const auto& params = std::get<Dict>(values.at("params").value);
      CHECK(std::get<std::string>(params.at("uuid").value) == display);
      CHECK(std::get<std::string>(params.at("url").value) == "maps:/car/instrumentcluster/map");
    }
    close(video.fd);
    video.fd = -1;
    until([&] {
      session.drain();
      return session.video_status().front().state == AirplaySessionSetup::ServiceState::closed;
    });
    CHECK(!session.show_cluster_map(display));
  }
}
int main() try {
  cluster_startup_ownership();
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
  bool blocked = true, fault = false;
  std::vector<AirplayVideoPacket> main, cluster;
  std::size_t observed = 0;
  session.set_video_sink([&](auto, const auto&) { ++observed; });
  session.set_video_consumer([&](std::uint64_t type, AirplayVideoPacket& packet) {
    if (type == 110 && fault) throw std::runtime_error("fixture consumer failure");
    if (type == 110 && blocked) return false;
    (type == 110 ? main : cluster).push_back(std::move(packet));
    return true;
  });
  Peer first, second;
  open(session, first, 110);
  open(session, second, 111);
  std::vector<pollfd> initial;
  session.append_poll_descriptors(initial);
  send_all(first.fd, packets(110, 300));
  until([&] {
    session.drain();
    return session.video_status()[0].retained_packets > 0;
  });
  const auto status = session.video_status()[0];
  const auto bytes = session.observed_bytes();
  CHECK(status.retained_packets <= 64 && status.retained_bytes <= 8 * 1024 * 1024 + 16384);
  for (unsigned n = 0; n < 5; ++n)
    session.drain();
  CHECK(session.observed_bytes() == bytes && main.empty());
  std::vector<pollfd> paused;
  session.append_poll_descriptors(paused);
  CHECK(paused.size() + 1 == initial.size());
  send_all(second.fd, packets(111, 1));
  until([&] {
    session.drain();
    return cluster.size() == 2;
  });  // Other screen is not stalled.
  blocked = false;
  until([&] {
    session.drain();
    return main.size() == 301;
  });
  CHECK(main[0].kind == AirplayVideoPacket::Kind::config &&
        main[0].data == std::vector<std::uint8_t>{42});
  for (unsigned n = 0; n < 300; ++n) {
    CHECK(main[n + 1].sender_ntp == n + 1);
    CHECK(main[n + 1].data ==
          std::vector<std::uint8_t>({std::uint8_t(n), std::uint8_t(n >> 8), 17, 19}));
  }
  CHECK(observed == 303 && session.video_status()[0].retained_packets == 0);
  fault = true;
  send_all(first.fd, packets(110, 1, false, 300));
  until([&] {
    session.drain();
    return session.video_status()[0].state == AirplaySessionSetup::ServiceState::closed;
  });
  CHECK(session.video_status()[1].state == AirplaySessionSetup::ServiceState::connected);
  close(second.fd);
  second.fd = -1;
  until([&] {
    session.drain();
    return session.video_status()[1].state == AirplaySessionSetup::ServiceState::closed;
  });
  // A stalled consumer expires even without another network event. It retains
  // neither packets nor an accept-ready listener after the bounded deadline.
  AirplaySessionSetup stalled;
  stalled.set_secret(secret);
  stalled.set_peer(address);
  (void)stalled.respond(
      encode_binary_plist(PlistValue{Dict{{"timingPort", PlistValue{std::int64_t{12345}}}}}));
  stalled.set_video_consumer([](auto, auto&) { return false; });
  Peer slow;
  open(stalled, slow, 110);
  send_all(slow.fd, packets(110, 1));
  until(
      [&] {
        stalled.drain();
        return stalled.video_status()[0].state == AirplaySessionSetup::ServiceState::closed;
      },
      7s);
  CHECK(stalled.video_status()[0].retained_packets == 0 &&
        stalled.video_status()[0].retained_bytes == 0);
  std::cout << "video backpressure tests passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
