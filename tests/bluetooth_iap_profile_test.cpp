// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/mfi_coprocessor.hpp"
#include "headunit/bluetooth_iap_profile.hpp"
#include "headunit/iap2_parameter.hpp"
#include <string_view>
#include <cerrno>
#include <gio/gunixfdlist.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <source_location>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
void check(bool value, std::source_location where = std::source_location::current()) {
  if (!value)
    throw std::runtime_error("Bluetooth profile fixture failed at line " +
                             std::to_string(where.line()));
}
struct Bus final : headunit::MfiRegisterBus {
  bool simulated_authentication = false;
  void read(std::uint8_t reg, std::span<std::uint8_t> output) override {
    if (!simulated_authentication) throw std::runtime_error("No physical MFi in fixture");
    std::fill(output.begin(), output.end(), 0);
    if (reg == 0x02)
      output[0] = 3;
    else if (reg == 0x30)
      output[1] = 1;
    else if (reg == 0x31)
      output[0] = 0x5b;
    else if (reg == 0x10)
      output[0] = 0x10;
    else if (reg == 0x11)
      output[1] = 64;
    else if (reg == 0x12)
      std::fill(output.begin(), output.end(), 0x9b);
    else
      check(reg == 0x05);
  }
  void write(std::uint8_t, std::span<const std::uint8_t>) override {
    if (!simulated_authentication) throw std::runtime_error("No physical MFi in fixture");
  }
};
// Exact production Profile1 boundary on a private test D-Bus. No radio,
// production BlueZ name, phone identity, MFi device or credentials are used.
struct Bluez {
  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  GMainContext* context = g_main_context_new();
  GDBusConnection* connection = nullptr;
  std::thread thread;
  std::atomic<bool> stopping{false};
  std::mutex mutex;
  std::string destination;
  // Device1.ConnectProfile: fail immediately, or hold the reply.
  std::atomic<bool> hold_connect{false};
  std::atomic<unsigned> connect_calls{0};
  std::string connect_uuid;
  std::string connect_error = "org.bluez.Error.Failed";
  std::string connect_reason = "Host is down";
  GDBusMethodInvocation* held = nullptr;
  static void method(GDBusConnection*, const gchar* sender, const gchar*, const gchar*,
                     const gchar* name, GVariant* args, GDBusMethodInvocation* invocation,
                     gpointer data) {
    auto& owner = *static_cast<Bluez*>(data);
    if (std::string(name) == "ConnectProfile") {
      std::lock_guard lock(owner.mutex);
      const gchar* uuid = nullptr;
      g_variant_get(args, "(&s)", &uuid);
      owner.connect_uuid = uuid;
      ++owner.connect_calls;
      if (owner.hold_connect)
        owner.held = invocation;
      else
        g_dbus_method_invocation_return_dbus_error(invocation, owner.connect_error.c_str(),
                                                   owner.connect_reason.c_str());
    } else if (std::string(name) == "Get") {
      g_dbus_method_invocation_return_value(
          invocation, g_variant_new("(v)", g_variant_new_string("02:00:00:00:00:01")));
    } else {
      if (std::string(name) == "RegisterProfile") {
        std::lock_guard lock(owner.mutex);
        owner.destination = sender;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
    }
  }
  Bluez() {
    g_test_dbus_up(bus);
    g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);
    std::promise<void> ready;
    auto future = ready.get_future();
    thread = std::thread([&, signal = std::move(ready)]() mutable {
      try {
        g_main_context_push_thread_default(context);
        GError* error = nullptr;
        connection = g_dbus_connection_new_for_address_sync(
            g_test_dbus_get_bus_address(bus),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, &error);
        check(connection && !error);
        auto reply = g_dbus_connection_call_sync(
            connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "RequestName", g_variant_new("(su)", "org.bluez", 0), nullptr, G_DBUS_CALL_FLAGS_NONE,
            1000, nullptr, &error);
        check(reply && !error);
        g_variant_unref(reply);
        auto node = g_dbus_node_info_new_for_xml(
            "<node><interface name='org.freedesktop.DBus.Properties'><method name='Get'><arg "
            "type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' "
            "direction='out'/></method></interface><interface "
            "name='org.bluez.ProfileManager1'><method name='RegisterProfile'><arg type='o' "
            "direction='in'/><arg type='s' direction='in'/><arg type='a{sv}' "
            "direction='in'/></method><method name='UnregisterProfile'><arg type='o' "
            "direction='in'/></method></interface><interface name='org.bluez.Device1'><method "
            "name='ConnectProfile'><arg type='s' direction='in'/></method></interface></node>",
            &error);
        check(node && !error);
        static const GDBusInterfaceVTable vtable{method, nullptr, nullptr, {nullptr}};
        const auto adapter = g_dbus_connection_register_object(
            connection, "/org/bluez/hci0", node->interfaces[0], &vtable, this, nullptr, &error);
        check(adapter && !error);
        const auto manager = g_dbus_connection_register_object(
            connection, "/org/bluez", node->interfaces[1], &vtable, this, nullptr, &error);
        check(manager && !error);
        const auto device =
            g_dbus_connection_register_object(connection, "/org/bluez/hci0/dev_02_00_00_00_00_02",
                                              node->interfaces[2], &vtable, this, nullptr, &error);
        check(device && !error);
        signal.set_value();
        while (!stopping)
          g_main_context_iteration(context, TRUE);
        g_dbus_connection_unregister_object(connection, adapter);
        g_dbus_connection_unregister_object(connection, manager);
        g_dbus_connection_unregister_object(connection, device);
        if (held)
          g_dbus_method_invocation_return_dbus_error(held, "org.bluez.Error.Failed",
                                                     "fixture stopped");
        g_dbus_node_info_unref(node);
        g_main_context_pop_thread_default(context);
      } catch (...) {
        signal.set_exception(std::current_exception());
      }
    });
    check(future.wait_for(2s) == std::future_status::ready);
    future.get();
  }
  ~Bluez() {
    stopping = true;
    g_main_context_wakeup(context);
    thread.join();
    if (connection) {
      g_dbus_connection_close_sync(connection, nullptr, nullptr);
      g_object_unref(connection);
    }
    g_main_context_unref(context);
    g_test_dbus_stop(bus);
    g_object_unref(bus);
    g_unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
  }
  // BlueZ announces the phone's radio link (ACL) on its Device1 object.
  void announce_link_up() {
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}", "Connected", g_variant_new_boolean(TRUE));
    GError* error = nullptr;
    check(g_dbus_connection_emit_signal(
        connection, nullptr, "/org/bluez/hci0/dev_02_00_00_00_00_02",
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", "org.bluez.Device1", &changed, nullptr), &error));
    check(!error);
    g_main_context_wakeup(context);
  }
  void release_held() {
    std::lock_guard lock(mutex);
    if (!held) return;
    g_dbus_method_invocation_return_dbus_error(held, "org.bluez.Error.Failed", "late reply");
    held = nullptr;
    g_main_context_wakeup(context);
  }
  bool connect(headunit::BluetoothIapProfile& profile, int fd,
               const char* path = "/org/example/HeadunitIap") {
    std::string target;
    {
      std::lock_guard lock(mutex);
      target = destination;
    }
    auto pending = std::async(std::launch::async, [&, target] {
      auto fds = g_unix_fd_list_new();
      GError* error = nullptr;
      const int handle = g_unix_fd_list_append(fds, fd, &error);
      check(handle >= 0 && !error);
      auto properties = g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0);
      auto reply = g_dbus_connection_call_with_unix_fd_list_sync(
          connection, target.c_str(), path, "org.bluez.Profile1", "NewConnection",
          g_variant_new("(oh@a{sv})", "/org/bluez/hci0/dev_02_00_00_00_00_02", handle, properties),
          nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, fds, nullptr, nullptr, &error);
      const bool accepted = reply != nullptr;
      if (reply) g_variant_unref(reply);
      if (error) g_error_free(error);
      g_object_unref(fds);
      return accepted;
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (pending.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
      profile.pump();
      std::this_thread::sleep_for(1ms);
    }
    check(pending.wait_for(0ms) == std::future_status::ready);
    return pending.get();
  }
};
void socket_pair(std::array<int, 2>& sockets) {
  check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0);
}
void send_bytes(int fd, const std::vector<std::uint8_t>& bytes) {
  check(send(fd, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()));
}
// Synthetic classic page: the far end stays with the test, so the profile's
// descriptor is writable (link up) and its closure is observable.
struct Page {
  std::vector<int> far_ends;
  std::atomic<unsigned> opens{0};
  int result_errno = 0;  // Nonzero: fail to open with this errno.
  bool never_completes = false;
  bool refuses_connection = false;
  headunit::BluetoothIapProfile::PageOpener opener() {
    return [this](std::string_view path) {
      check(path == "/org/bluez/hci0/dev_02_00_00_00_00_02");
      ++opens;
      if (result_errno) {
        errno = result_errno;
        return -1;
      }
      std::array<int, 2> ends{};
      if (refuses_connection) {
        // A bound, non-listening loopback socket reserves the port. The
        // actual pending socket reports ECONNREFUSED through SO_ERROR.
        ends[1] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(ends[1] >= 0);
        far_ends.push_back(ends[1]);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(ends[1], reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t size = sizeof(address);
        check(getsockname(ends[1], reinterpret_cast<sockaddr*>(&address), &size) == 0);
        ends[0] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        check(ends[0] >= 0);
        check(connect(ends[0], reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 &&
              errno == EINPROGRESS);
        return ends[0];
      }
      if (never_completes) {
        // A pipe's read end never becomes writable: the page never answers.
        check(pipe(ends.data()) == 0);
        far_ends.push_back(ends[1]);
        return ends[0];
      }
      socket_pair(ends);
      far_ends.push_back(ends[1]);
      return ends[0];
    };
  }
  // True once the profile has closed the most recent page descriptor.
  bool last_closed() const {
    if (far_ends.empty()) return false;
    pollfd hangup{far_ends.back(), POLLIN, 0};
    if (poll(&hangup, 1, 0) != 1) return false;
    std::array<std::uint8_t, 8> scratch{};
    return recv(far_ends.back(), scratch.data(), scratch.size(), MSG_DONTWAIT) == 0;
  }
  ~Page() {
    for (const int fd : far_ends)
      close(fd);
  }
};
void wait_retired(headunit::BluetoothIapProfile& profile) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (profile.engine() && std::chrono::steady_clock::now() < deadline) {
    profile.pump();
    std::this_thread::sleep_for(2ms);
  }
  check(!profile.engine());
  std::vector<pollfd> descriptors;
  profile.append_poll_descriptors(descriptors);
  check(descriptors.empty());
}
}  // namespace
int main() {
  Bluez bluez;
  Bus bus;
  headunit::MfiCoprocessor chip(bus);
  {
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36});
    profile.start();
    std::array<int, 2> sockets;
    socket_pair(sockets);
    check(bluez.connect(profile, sockets[0]));
    check(fcntl(sockets[0], F_GETFL) & O_NONBLOCK);  // Same inherited open-file description.
    std::array<std::uint8_t, 4096> bytes{};
    check(recv(sockets[1], bytes.data(), bytes.size(), 0) == 6);
    auto engine = profile.engine();
    check(engine && engine->diagnostic_generation() == 1);
    send_bytes(sockets[1], headunit::Iap2SessionEngine::sync_preamble());
    std::vector<pollfd> descriptors;
    profile.append_poll_descriptors(descriptors);
    check(descriptors.size() == 1 && (descriptors[0].events & POLLIN));
    // RFCOMM readiness wakes the exact descriptor appended to the receiver poll,
    // rather than requiring the old unrelated 100 ms idle timeout.
    check(poll(descriptors.data(), descriptors.size(), 500) == 1 &&
          (descriptors[0].revents & POLLIN));
    profile.pump();
    check(engine->handshake_state() == headunit::LinkHandshakeState::waiting_for_syn_ack);
    // Synthetic protocol clock drives only the engine's existing retry bound;
    // the actual GLib timer must observe terminal failure and release its owner.
    const auto now = headunit::Iap2SessionEngine::Clock::now();
    for (int attempt = 1; attempt < 5; ++attempt)
      engine->tick(now + attempt * 500ms);
    bool timed_out = false;
    try {
      engine->tick(now + 2500ms);
    } catch (const std::runtime_error&) {
      timed_out = true;
    }
    check(timed_out);
    wait_retired(profile);
    close(sockets[0]);
    close(sockets[1]);
    engine.reset();

    // A full peer receive buffer must not block NewConnection/the shared control
    // owner. TX is retained until writable, then emits the intact sync marker.
    socket_pair(sockets);
    int small = 1024;
    check(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    check(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0);
    std::size_t filler = 0;
    bytes.fill(0xa5);
    for (;;) {
      const auto count = send(sockets[0], bytes.data(), bytes.size(), 0);
      if (count < 0) break;
      check(count > 0);
      filler += count;
    }
    check(filler && bluez.connect(profile, sockets[0]));
    descriptors.clear();
    profile.append_poll_descriptors(descriptors);
    check(descriptors.size() == 1 && (descriptors[0].events & POLLOUT));
    while (filler) {
      const auto count = recv(sockets[1], bytes.data(), std::min(filler, bytes.size()), 0);
      check(count > 0);
      filler -= count;
    }
    check(poll(descriptors.data(), descriptors.size(), 500) == 1 &&
          (descriptors[0].revents & POLLOUT));
    profile.pump();
    check(recv(sockets[1], bytes.data(), bytes.size(), 0) == 6);
    const auto marker = headunit::Iap2SessionEngine::sync_preamble();
    check(std::equal(marker.begin(), marker.end(), bytes.begin()));
    descriptors.clear();
    profile.append_poll_descriptors(descriptors);
    check(!(descriptors[0].events & POLLOUT));
    close(sockets[1]);
    wait_retired(profile);
    close(sockets[0]);

    // A peer that never drains TX hits the two-second local queue deadline and
    // leaves no descriptors/engine behind; a following generation still works.
    socket_pair(sockets);
    check(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    check(fcntl(sockets[0], F_SETFL, O_NONBLOCK) == 0);
    while (send(sockets[0], bytes.data(), bytes.size(), 0) > 0) {
    }
    check(bluez.connect(profile, sockets[0]));
    // Establish the reliable link while TX remains blocked: retirement below
    // must be its local TX deadline, not the SYN/marker retry bound.
    send_bytes(sockets[1], headunit::Iap2SessionEngine::sync_preamble());
    profile.pump();
    send_bytes(sockets[1], headunit::encode_link_frame({0xc0, 135, 99, 0, {1}}));
    profile.pump();
    check(profile.engine() && profile.engine()->established());
    auto retained = profile.engine();  // Actual control Connection shared ownership.
    wait_retired(profile);
    if (retained->established())
      throw std::runtime_error("Retained iAP engine survived fatal local TX retirement");
    check(retained->failed());
    auto rejects_late = [](const auto& action) {
      bool rejected = false;
      try {
        action();
      } catch (const std::runtime_error&) {
        rejected = true;
      }
      check(rejected);
    };
    rejects_late([&] {
      retained->receive_wifi(
          headunit::encode_link_frame(
              {0x40, 136, 99, 10, headunit::encode_control_message({0x1d00, {}})}),
          headunit::Iap2SessionEngine::Clock::now());
    });
    rejects_late([&] { retained->tick(headunit::Iap2SessionEngine::Clock::now()); });
    rejects_late(
        [&] { retained->send_control({0x4301, {3}}, headunit::Iap2SessionEngine::Clock::now()); });
    close(sockets[0]);
    close(sockets[1]);
    socket_pair(sockets);
    check(bluez.connect(profile, sockets[0]));
    check(profile.engine()->diagnostic_generation() == 4);
    check(profile.engine() != retained && !profile.engine()->failed());
    rejects_late([&] {
      retained->receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(),
                                  headunit::Iap2SessionEngine::Clock::now());
    });
    profile.release_session(profile.engine());
    close(sockets[0]);
    close(sockets[1]);

    // Non-socket/unsuitable descriptors and constructor/configuration exceptions
    // are rejected at the C callback boundary without retaining an occupied slot.
    const int invalid = open("/dev/null", O_RDWR);
    check(invalid >= 0);
    check(!bluez.connect(profile, invalid));
    check(!profile.engine());
    close(invalid);
    // Genuine live handoff eligibility retains the same engine after RFCOMM
    // closes. Synthetic MFi registers exercise the actual authentication state
    // machine; they do not establish a hardware/phone authentication result.
    bus.simulated_authentication = true;
    socket_pair(sockets);
    check(bluez.connect(profile, sockets[0]));
    check(recv(sockets[1], bytes.data(), bytes.size(), 0) == 6);
    send_bytes(sockets[1], headunit::Iap2SessionEngine::sync_preamble());
    profile.pump();
    send_bytes(sockets[1], headunit::encode_link_frame({0xc0, 135, 99, 0, {1}}));
    profile.pump();
    auto handoff = profile.engine();
    auto control = [&](std::uint8_t sequence, std::uint8_t acknowledgement,
                       const headunit::ControlMessage& message) {
      send_bytes(sockets[1],
                 headunit::encode_link_frame({0x40, sequence, acknowledgement, 10,
                                              headunit::encode_control_message(message)}));
      profile.pump();
    };
    control(136, 99, {0xaa00, {}});
    const std::array<headunit::Iap2Parameter, 1> challenge{
        {{0, std::vector<std::uint8_t>(32, 0x42)}}};
    control(137, 100, {0xaa02, headunit::encode_iap2_parameters(challenge)});
    control(138, 101, {0xaa05, {}});
    control(139, 101, {0x5702, {}});
    check(handoff && handoff->handoff_sent() && handoff->established());
    close(sockets[1]);
    const auto handoff_deadline = std::chrono::steady_clock::now() + 1s;
    do {
      profile.pump();
      descriptors.clear();
      profile.append_poll_descriptors(descriptors);
      if (descriptors.empty()) break;
      std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < handoff_deadline);
    check(descriptors.empty() && profile.engine() == handoff && !handoff->failed());
    handoff->receive_wifi(headunit::encode_link_frame({0x40, 140, 102, 0, {}}),
                          headunit::Iap2SessionEngine::Clock::now());
    check(handoff->wifi_active() && handoff->established());
    profile.release_session(handoff);
    check(handoff->failed() && !handoff->established() && !profile.engine());
    close(sockets[0]);
    bus.simulated_authentication = false;
    // A failing coprocessor callback is exercised through a real certificate request.
    socket_pair(sockets);
    check(bluez.connect(profile, sockets[0]));
    check(recv(sockets[1], bytes.data(), bytes.size(), 0) == 6);
    send_bytes(sockets[1], headunit::Iap2SessionEngine::sync_preamble());
    profile.pump();
    send_bytes(sockets[1], headunit::encode_link_frame({0xc0, 135, 99, 0, {1}}));
    profile.pump();
    send_bytes(sockets[1],
               headunit::encode_link_frame(
                   {0x40, 136, 99, 10, headunit::encode_control_message({0xaa00, {}})}));
    wait_retired(profile);
    close(sockets[0]);
    close(sockets[1]);
  }
  {
    headunit::Iap2Identity invalid_identity;
    invalid_identity.route_guidance = headunit::RouteGuidanceConfig{42, ""};
    headunit::BluetoothIapProfile invalid_profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                                  std::nullopt, invalid_identity);
    invalid_profile.start();
    std::array<int, 2> rejected;
    socket_pair(rejected);
    check(!bluez.connect(invalid_profile, rejected[0]));
    check(!invalid_profile.engine());
    std::vector<pollfd> empty;
    invalid_profile.append_poll_descriptors(empty);
    check(empty.empty());
    close(rejected[0]);
    close(rejected[1]);
  }

  // Real async callbacks classify known failures without leaking arbitrary
  // error names or bodies. Each new profile has its own immediate first page.
  for (const auto& [name, reason, expected] : std::array<std::array<const char*, 3>, 3>{
           {{"org.bluez.Error.InProgress", "br-connection-busy",
             "error=in_progress reason=br-connection-busy"},
            {"org.bluez.Error.Failed", "le-connection-abort-by-local",
             "error=failed reason=le-connection-abort-by-local"},
            {"private.example.DeviceIdentity", "private-phone-identity",
             "error=other_remote reason=other"}}}) {
    {
      std::lock_guard lock(bluez.mutex);
      bluez.connect_error = name;
      bluez.connect_reason = reason;
    }
    Page page;
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                          std::nullopt, {}, "/org/bluez/hci0/dev_02_00_00_00_00_02",
                                          page.opener());
    profile.start();
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (output.str().find(expected) == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
      profile.pump();
      profile.maintain_connection(true);
      std::this_thread::sleep_for(5ms);
    }
    std::cout.rdbuf(previous);
    check(output.str().find(expected) != std::string::npos);
    check(output.str().find("elapsed_ms=") != std::string::npos);
    check(output.str().find("native_errno=unavailable") != std::string::npos);
    check(output.str().find("private") == std::string::npos);
  }
  {
    std::lock_guard lock(bluez.mutex);
    bluez.connect_error = "org.bluez.Error.Failed";
    bluez.connect_reason = "Host is down";
    bluez.connect_calls = 0;
  }

  // Accessory-initiated reconnect: iOS does not reopen iAP after a CarPlay
  // session, so an idle receiver pages the selected phone's iAP service.
  {
    const std::string phone = "/org/bluez/hci0/dev_02_00_00_00_00_02";
    auto idle_for = [](headunit::BluetoothIapProfile& profile, std::chrono::milliseconds duration,
                       bool idle = true) {
      const auto until = std::chrono::steady_clock::now() + duration;
      while (std::chrono::steady_clock::now() < until) {
        profile.pump();
        profile.maintain_connection(idle);
        std::this_thread::sleep_for(5ms);
      }
    };
    Page page;
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                          std::nullopt, {}, phone, page.opener());
    profile.start();
    idle_for(profile, 500ms);
    check(page.opens == 1);
    check(bluez.connect_calls == 1);
    {
      std::lock_guard lock(bluez.mutex);
      check(bluez.connect_uuid == "00000000-deca-fade-deca-deafdecacafe");
    }
    // BlueZ answered (here: failed), so the page link is released.
    check(page.last_closed());
    idle_for(profile, 2000ms);
    check(bluez.connect_calls == 1);  // Failure backs off instead of paging continuously.
    // BlueZ delivers the requested channel to the client profile object.
    std::array<int, 2> sockets;
    socket_pair(sockets);
    check(bluez.connect(profile, sockets[0], "/org/example/HeadunitIapClient"));
    check(profile.engine() != nullptr);
    idle_for(profile, 5000ms);
    check(bluez.connect_calls == 1);  // An open transport is never paged again.
    // Retirement must end the shared RFCOMM connection even though BlueZ (here
    // sockets[0]) still holds its own descriptor; otherwise the link stays up.
    profile.release_session(profile.engine());
    wait_retired(profile);
    pollfd hangup{sockets[1], POLLIN, 0};
    check(poll(&hangup, 1, 500) == 1);
    std::array<std::uint8_t, 64> drained{};
    while (recv(sockets[1], drained.data(), drained.size(), MSG_DONTWAIT) > 0) {
    }
    check(recv(sockets[1], drained.data(), drained.size(), MSG_DONTWAIT) == 0);
    close(sockets[1]);
    close(sockets[0]);
    idle_for(profile, 2000ms, false);  // An active control session resets the policy.
    check(bluez.connect_calls == 1);
    bluez.hold_connect = true;
    idle_for(profile, 2500ms);
    check(bluez.connect_calls == 1);  // Later sessions retain the phone's grace period.
    idle_for(profile, 1000ms);
    check(bluez.connect_calls == 2);
  }
  {
    // The page bound must stay sane; a zero bound would retire every page.
    bool rejected = false;
    try {
      headunit::BluetoothIapProfile invalid(chip, {"Synthetic AP", "synthetic-password", 36},
                                            std::nullopt, {},
                                            "/org/bluez/hci0/dev_02_00_00_00_00_02", {}, 0ms);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected);
  }
  for (const int native_error : {EHOSTDOWN, EIO}) {
    // A page that cannot open never reaches BlueZ, so BlueZ cannot fall back
    // to a Low Energy connection; the usual backoff applies. Preserve the
    // actual errno even when the fixed classification can only say "other".
    const auto calls = bluez.connect_calls.load();
    Page page;
    page.result_errno = native_error;
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                          std::nullopt, {}, "/org/bluez/hci0/dev_02_00_00_00_00_02",
                                          page.opener());
    profile.start();
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < until) {
      profile.pump();
      profile.maintain_connection(true);
      std::this_thread::sleep_for(5ms);
    }
    std::cout.rdbuf(previous);
    // An unanswered page is retried after one second while the phone is
    // expected back; any other failure keeps the four-second backoff.
    check(page.opens == (native_error == EHOSTDOWN ? 2u : 1u));
    check(bluez.connect_calls == calls);
    const std::string expected_reason = native_error == EHOSTDOWN ? "page-timeout" : "other";
    check(output.str().find("error=page reason=" + expected_reason) != std::string::npos);
    check(output.str().find("native_errno=" + std::to_string(native_error) + "\n") !=
          std::string::npos);
  }
  {
    // Once the expected-reconnect window has passed, an unanswered page backs
    // off as for an absent phone instead of paging continuously.
    Page page;
    page.result_errno = EHOSTDOWN;
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                          std::nullopt, {}, "/org/bluez/hci0/dev_02_00_00_00_00_02",
                                          page.opener(), 20s, 0ms);
    profile.start();
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    const auto until = std::chrono::steady_clock::now() + 1500ms;
    while (std::chrono::steady_clock::now() < until) {
      profile.pump();
      profile.maintain_connection(true);
      std::this_thread::sleep_for(5ms);
    }
    std::cout.rdbuf(previous);
    check(page.opens == 1);
    check(output.str().find("retry_s=4 error=page reason=page-timeout") != std::string::npos);
  }
  {
    // When BlueZ reports the radio link up, iAP is requested at once instead
    // of waiting for the probe channel, which an iPhone can answer slowly.
    bluez.hold_connect = false;  // BlueZ answers this request at once.
    const auto calls = bluez.connect_calls.load();
    Page page;
    page.never_completes = true;
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                          std::nullopt, {}, "/org/bluez/hci0/dev_02_00_00_00_00_02",
                                          page.opener(), 20s);
    profile.start();
    for (int i = 0; i < 40 && page.opens == 0; ++i) {
      profile.pump();
      profile.maintain_connection(true);
      std::this_thread::sleep_for(5ms);
    }
    check(page.opens == 1);
    check(bluez.connect_calls == calls);
    bluez.announce_link_up();
    for (int i = 0; i < 200 && bluez.connect_calls == calls; ++i) {
      profile.pump();
      profile.maintain_connection(true);
      std::this_thread::sleep_for(5ms);
    }
    check(bluez.connect_calls == calls + 1);
    // BlueZ's reply (here a failure) releases the page descriptor.
    for (int i = 0; i < 100 && !page.last_closed(); ++i) {
      profile.pump();
      std::this_thread::sleep_for(5ms);
    }
    pollfd closed{page.far_ends.back(), POLLOUT, 0};
    check(poll(&closed, 1, 0) == 1 && (closed.revents & (POLLERR | POLLHUP)));
    check(page.opens == 1);
  }
  {
    // Preserve the actual asynchronous SO_ERROR result as well as the
    // immediate opener error. A refused page never requests the profile.
    const auto calls = bluez.connect_calls.load();
    Page page;
    page.refuses_connection = true;
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                          std::nullopt, {}, "/org/bluez/hci0/dev_02_00_00_00_00_02",
                                          page.opener());
    profile.start();
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    const auto until = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < until &&
           output.str().find("error=page reason=refused") == std::string::npos) {
      profile.pump();
      profile.maintain_connection(true);
      std::this_thread::sleep_for(5ms);
    }
    std::cout.rdbuf(previous);
    check(output.str().find("error=page reason=refused") != std::string::npos);
    check(output.str().find("native_errno=" + std::to_string(ECONNREFUSED) + "\n") !=
          std::string::npos);
    check(page.opens == 1 && bluez.connect_calls == calls);
  }
  {
    // A page that never answers is abandoned after its bound and closed.
    const auto calls = bluez.connect_calls.load();
    Page page;
    page.never_completes = true;
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                          std::nullopt, {}, "/org/bluez/hci0/dev_02_00_00_00_00_02",
                                          page.opener(), 300ms);
    profile.start();
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < until &&
           output.str().find("error=page reason=timeout") == std::string::npos) {
      profile.pump();
      profile.maintain_connection(true);
      std::this_thread::sleep_for(5ms);
    }
    std::cout.rdbuf(previous);
    check(output.str().find("error=page reason=timeout") != std::string::npos);
    check(output.str().find("native_errno=unavailable") != std::string::npos);
    check(page.opens == 1);
    check(bluez.connect_calls == calls);
    pollfd closed{page.far_ends.back(), POLLOUT, 0};
    check(poll(&closed, 1, 0) == 1 && (closed.revents & (POLLERR | POLLHUP)));
  }
  {
    // If the phone opens iAP itself while being paged, the page is dropped
    // instead of asking BlueZ for a second connection.
    const auto calls = bluez.connect_calls.load();
    Page page;
    page.never_completes = true;
    headunit::BluetoothIapProfile profile(chip, {"Synthetic AP", "synthetic-password", 36},
                                          std::nullopt, {}, "/org/bluez/hci0/dev_02_00_00_00_00_02",
                                          page.opener(), 300ms);
    profile.start();
    for (int i = 0; i < 40 && page.opens == 0; ++i) {
      profile.pump();
      profile.maintain_connection(true);
      std::this_thread::sleep_for(5ms);
    }
    check(page.opens == 1);
    std::array<int, 2> sockets;
    socket_pair(sockets);
    check(bluez.connect(profile, sockets[0], "/org/example/HeadunitIapClient"));
    check(profile.engine() != nullptr);
    // Acceptance must cancel an unanswered page immediately, before its
    // deadline can report a false reconnect failure for the active transport.
    pollfd closed{page.far_ends.back(), POLLOUT, 0};
    if (poll(&closed, 1, 0) != 1 || !(closed.revents & (POLLERR | POLLHUP)))
      throw std::runtime_error("Accepted transport retained a pending classic page");
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    for (int i = 0; i < 100; ++i) {
      profile.pump();
      profile.maintain_connection(false);
      std::this_thread::sleep_for(5ms);
    }
    std::cout.rdbuf(previous);
    check(output.str().find("error=page") == std::string::npos);
    check(bluez.connect_calls == calls);
    profile.release_session(profile.engine());
    wait_retired(profile);
    close(sockets[1]);
    close(sockets[0]);
  }
  // The profile above died with its request in flight; a late reply must not
  // reach the destroyed owner.
  bluez.release_held();
  for (int i = 0; i < 50; ++i) {
    g_main_context_iteration(nullptr, FALSE);
    std::this_thread::sleep_for(2ms);
  }
}
