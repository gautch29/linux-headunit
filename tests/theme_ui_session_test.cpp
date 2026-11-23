// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_session.hpp>
#include <headunit/binary_plist.hpp>
#include <headunit/ultra_ui_envelope.hpp>
#include <plist/plist.h>
#include <array>
#include <future>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
using namespace headunit;
using namespace headunit::theme::script;
using namespace std::chrono_literals;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
const std::string display_id = "aabbccdd-0000-0000-0000-000000000001";
using Dict = PlistValue::Dictionary;
using Array = PlistValue::Array;
UiWorker::Configuration config() {
  UiWorker::Configuration c;
  c.displays = {{"Center_Display"}};
  c.routes = {{display_id, "Center_Display"}};
  c.resources = [](std::string_view) -> std::string {
    return R"(
    CP.DPLuaTask.create():run(function()
      while true do
        local m=CP.receiveMessage()
        if m then assert(CP.sendMessage(m)) end
        coroutine.yield()
      end
    end)
    return {}
  )";
  };
  return c;
}
std::uint64_t number(plist_t p, const char* key) {
  std::uint64_t n = 0;
  plist_get_uint_val(plist_dict_get_item(p, key), &n);
  return n;
}
struct Peer {
  int fd = -1;
  std::uint64_t id = 0, seed = 0, nonce = 0;
  ~Peer() {
    if (fd >= 0) close(fd);
  }
  void send_value(std::int64_t value) {
    const auto payload = encode_ultra_ui_envelope(
        {display_id, encode_binary_plist(PlistValue{Dict{{"value", PlistValue{value}}}})});
    std::vector<std::uint8_t> plain(32 + payload.size());
    for (unsigned i = 0; i < 4; ++i)
      plain[i] = std::uint8_t(plain.size() >> ((3 - i) * 8));
    plain[4] = 'a';
    plain[5] = 's';
    plain[6] = 'y';
    plain[7] = 'n';
    plain[16] = 'c';
    plain[17] = 'o';
    plain[18] = 'm';
    plain[19] = 'm';
    std::copy(payload.begin(), payload.end(), plain.begin() + 32);
    const std::array<std::uint8_t, 2> aad{std::uint8_t(plain.size()),
                                          std::uint8_t(plain.size() >> 8)};
    std::array<char, 8> iv{};
    for (unsigned i = 0; i < 8; ++i)
      iv[i] = char(nonce >> (8 * i));
    ++nonce;
    const std::array<std::uint8_t, 32> secret{};
    const auto key = hap_hkdf_sha512(secret, "DataStream-Salt" + std::to_string(seed),
                                     "DataStream-Output-Encryption-Key");
    const auto sealed = hap_encrypt(key, {iv.data(), iv.size()}, plain, aad);
    std::vector<std::uint8_t> wire(aad.begin(), aad.end());
    wire.insert(wire.end(), sealed.begin(), sealed.end());
    CHECK(send(fd, wire.data(), wire.size(), 0) == ssize_t(wire.size()));
  }
};
void open(AirplaySessionSetup& session, Peer& peer, std::uint64_t seed,
          const char* service = ultra_ui_service) {
  const auto request = encode_binary_plist(
      PlistValue{Dict{{"streams", PlistValue{Array{PlistValue{Dict{
                                      {"type", PlistValue{std::int64_t{130}}},
                                      {"seed", PlistValue{std::int64_t(seed)}},
                                      {"clientTypeUUID", PlistValue{std::string(service)}}}}}}}}});
  const auto bytes = session.respond(request);
  plist_t reply = nullptr;
  CHECK(plist_from_bin(reinterpret_cast<const char*>(bytes.data()), bytes.size(), &reply) ==
        PLIST_ERR_SUCCESS);
  const auto stream = plist_array_get_item(plist_dict_get_item(reply, "streams"), 0);
  peer.id = number(stream, "streamID");
  peer.seed = seed;
  const auto port = number(stream, "dataPort");
  plist_free(reply);
  peer.fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(peer.fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  CHECK(connect(peer.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  session.drain();
}
template <class F>
void until(F f) {
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (!f()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::sleep_for(1ms);
  }
}
int main() try {
  struct Timing {
    int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    ~Timing() {
      if (fd >= 0) ::close(fd);
    }
  } timing;
  CHECK(timing.fd >= 0);
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  CHECK(::bind(timing.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  socklen_t address_size = sizeof(address);
  CHECK(::getsockname(timing.fd, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
  const auto timing_port = std::int64_t(ntohs(address.sin6_port));
  CHECK(timing_port > 0);
  AirplaySessionSetup socket;
  std::promise<void> release, entered;
  auto gate = release.get_future().share();
  unsigned factories = 0;
  UiSession owner([&] {
    auto c = config();
    if (++factories == 1)
      c.configure = [&](Channel&) {
        entered.set_value();
        gate.wait();
      };
    if (factories == 3)
      c.resources = [](std::string_view) -> std::string {
        return "error(\"fixture failure\")";
      };
    return c;
  });
  bool rejected = false;
  try {
    socket.enable_native_ultra();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  CHECK(rejected);
  owner.attach(socket);
  constexpr const char* other_service = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
  socket.register_data_service(other_service);
  socket.set_video_consumer([](auto, auto&) { return true; });
  socket.enable_native_ultra();
  const std::array<std::uint8_t, 32> secret{};
  socket.set_secret(secret);
  socket.set_peer(address);
  const auto offer = [&](bool hevc) {
    Array features{PlistValue{std::string("uiSync")}};
    if (hevc) features.push_back(PlistValue{std::string("hevc")});
    return encode_binary_plist(PlistValue{
        Dict{{"timingPort", PlistValue{timing_port}}, {"features", PlistValue{features}}}});
  };
  for (const auto feature_case : {0, 1, 2, 3}) {
    AirplaySessionSetup standard;
    unsigned started = 0;
    UiSession unused([&] {
      ++started;
      return config();
    });
    unused.attach(standard);
    standard.register_data_service(other_service);
    standard.set_video_consumer([](auto, auto&) { return true; });
    standard.enable_native_ultra();
    standard.set_peer(address);
    standard.set_secret(secret);
    Dict initial{{"timingPort", PlistValue{timing_port}}};
    if (feature_case != 0) {
      Array features;
      if (feature_case == 2) features.push_back(PlistValue{std::string("hevc")});
      if (feature_case == 3) features.push_back(PlistValue{std::string("uiSync")});
      initial.emplace("features", PlistValue{std::move(features)});
    }
    const auto response = standard.respond(encode_binary_plist(PlistValue{std::move(initial)}));
    CHECK(standard.negotiated_mode() == CarplayMode::standard);
    unused.pump(standard);
    CHECK(started == 0 && !unused.status());
    plist_t reply = nullptr;
    CHECK(plist_from_bin(reinterpret_cast<const char*>(response.data()), response.size(), &reply) ==
          PLIST_ERR_SUCCESS);
    CHECK(plist_array_get_size(plist_dict_get_item(reply, "enabledFeatures")) == 2);
    plist_free(reply);
    Peer unexpected;
    rejected = false;
    try {
      open(standard, unexpected, 900);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(standard.negotiated_mode() == CarplayMode::standard && standard.service_status().empty());
    Peer valid;
    open(standard, valid, 900, other_service);
    unused.pump(standard);
    CHECK(standard.negotiated_mode() == CarplayMode::standard && started == 0);
  }
  const auto setup = socket.respond(offer(true));
  CHECK(!socket.negotiated_mode());
  owner.pump(socket);
  CHECK(factories == 0 && !owner.status());
  plist_t setup_root = nullptr;
  CHECK(plist_from_bin(reinterpret_cast<const char*>(setup.data()), setup.size(), &setup_root) ==
        PLIST_ERR_SUCCESS);
  auto enabled = plist_dict_get_item(setup_root, "enabledFeatures");
  CHECK(plist_array_get_size(enabled) == 4);
  char* feature = nullptr;
  plist_get_string_val(plist_array_get_item(enabled, 2), &feature);
  CHECK(feature && std::string(feature) == "uiSync");
  plist_mem_free(feature);
  plist_get_string_val(plist_array_get_item(enabled, 3), &feature);
  CHECK(feature && std::string(feature) == "hevc");
  plist_mem_free(feature);
  plist_free(setup_root);
  Peer other;
  open(socket, other, 122, other_service);
  owner.pump(socket);
  CHECK(!socket.negotiated_mode() && factories == 0 && !owner.status());
  Peer first;
  open(socket, first, 123);
  owner.pump(socket);
  CHECK(socket.negotiated_mode() == CarplayMode::ultra);
  std::vector<pollfd> initial;
  socket.append_poll_descriptors(initial);
  CHECK(entered.get_future().wait_for(3s) == std::future_status::ready);
  // A full worker mailbox retains the next transport message and pauses only
  // its socket. Stop must still return before slow startup finishes.
  bool bounded = true;
  for (unsigned i = 0; i < 66; ++i) {
    first.send_value(i);
    socket.drain();
    owner.pump(socket);
  }
  bounded &= owner.status()->queued_input == 64;
  std::vector<pollfd> paused;
  socket.append_poll_descriptors(paused);
  bounded &= paused.size() + 1 == initial.size();
  // The owner must not consume the retained 65th and 66th message twice.
  unsigned pending = 0;
  socket.deliver_service_messages(first.id, [&](const DataMessage&) {
    ++pending;
    return false;
  });
  bounded &= pending == 1;
  release.set_value();
  CHECK(bounded);
  until([&] {
    socket.drain();
    owner.pump(socket);
    return owner.status()->received == 66;
  });
  CHECK(owner.stream_id() == first.id && factories == 1);
  until([&] {
    socket.drain();
    owner.pump(socket);
    return owner.status()->sent == 66;
  });
  socket.drain();
  timeval timeout{2, 0};
  CHECK(setsockopt(first.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
  const auto output_key =
      hap_hkdf_sha512(secret, "DataStream-Salt123", "DataStream-Input-Encryption-Key");
  for (unsigned index = 0; index < 66; ++index) {
    std::array<std::uint8_t, 2> header{};
    CHECK(recv(first.fd, header.data(), 2, MSG_WAITALL) == 2);
    const auto encrypted_size =
        static_cast<std::size_t>(header[0]) + (static_cast<std::size_t>(header[1]) << 8) + 16;
    std::vector<std::uint8_t> encrypted(encrypted_size);
    CHECK(recv(first.fd, encrypted.data(), encrypted.size(), MSG_WAITALL) ==
          ssize_t(encrypted.size()));
    std::array<char, 8> nonce{};
    nonce[0] = char(index);
    const auto clear = hap_decrypt(output_key, {nonce.data(), nonce.size()}, encrypted, header);
    CHECK(clear && clear->size() > 32);
    CHECK((*clear)[16] == 'c' && (*clear)[17] == 'm' && (*clear)[18] == 'n' && (*clear)[19] == 'd');
    const auto expected = encode_ultra_ui_envelope(
        {display_id,
         encode_binary_plist(PlistValue{Dict{{"value", PlistValue{std::int64_t(index)}}}})});
    CHECK(std::vector<std::uint8_t>(clear->begin() + 32, clear->end()) == expected);
  }
  // EOF must stop/reap the old VM even with no subsequent UI message.
  close(first.fd);
  first.fd = -1;
  until([&] {
    socket.drain();
    owner.pump(socket);
    return !owner.status();
  });
  Peer second;
  open(socket, second, 124);
  owner.pump(socket);
  CHECK(second.id != first.id && factories == 2);
  until([&] {
    socket.drain();
    owner.pump(socket);
    return owner.status()->state == UiWorker::State::running;
  });
  close(second.fd);
  second.fd = -1;
  until([&] {
    socket.drain();
    owner.pump(socket);
    return !owner.status();
  });
  Peer bad;
  open(socket, bad, 125);
  owner.pump(socket);
  // The failure reason is published while the worker is still tearing down;
  // the service closes once it has finished. Wait for both.
  until([&] {
    socket.drain();
    owner.pump(socket);
    return !owner.failure_reason().empty() && !owner.stream_id();
  });
  CHECK(!owner.stream_id() &&
        socket.service_status().back().state == AirplaySessionSetup::ServiceState::closed);
  Peer fresh;
  open(socket, fresh, 126);
  owner.pump(socket);
  until([&] {
    socket.drain();
    owner.pump(socket);
    return owner.status()->state == UiWorker::State::running;
  });
  owner.stop();
  CHECK(!owner.stream_id());
  until([&] { return owner.finished(); });
  owner.pump(socket);
  CHECK(factories == 4);
  std::cout << "UI socket ownership, retained ingress, EOF and fresh generation passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
