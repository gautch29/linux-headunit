// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/iap2_link.hpp>
#include <headunit/iap2_message.hpp>
#include <headunit/iap2_handshake.hpp>
#include <headunit/iap2_identity.hpp>
#include <headunit/iap2_mfi_auth.hpp>
#include <headunit/iap2_reliable_link.hpp>
#include <headunit/iap2_wifi_handoff.hpp>
#include <headunit/iap2_session_engine.hpp>
#include <headunit/linux_mfi_bus.hpp>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr char profile_path[] = "/org/example/HeadunitProbe";
// A non-CarPlay service for testing BlueZ's Profile1 descriptor lifecycle.
constexpr char test_uuid[] = "f74ca3f2-49a4-4b65-9a2d-9f1b2574688c";
constexpr unsigned test_channel = 29;
constexpr char iap2_uuid[] = "00000000-deca-fade-deca-deafdecacaff";
constexpr unsigned iap2_channel = 3;
constexpr std::uint8_t initial_sequence = 99;

constexpr char profile_xml[] = R"xml(
<node>
  <interface name="org.bluez.Profile1">
    <method name="Release"/>
    <method name="NewConnection">
      <arg type="o" direction="in"/>
      <arg type="h" direction="in"/>
      <arg type="a{sv}" direction="in"/>
    </method>
    <method name="RequestDisconnection">
      <arg type="o" direction="in"/>
    </method>
  </interface>
</node>)xml";

struct Profile {
  GDBusConnection* bus = nullptr;
  GMainLoop* loop = nullptr;
  int transport = -1;
  guint watch = 0;
  bool registered = false;
  bool iap2_test = false;
  headunit::LinkFrameReader reader;
  std::shared_ptr<headunit::Iap2SessionEngine> engine;
  headunit::Iap2Identity identity;
  std::optional<headunit::Iap2WifiHandoff> handoff;
  std::unique_ptr<headunit::LinuxMfiBus> mfi_bus;
  std::unique_ptr<headunit::MfiCoprocessor> mfi_chip;
  headunit::LinkHandshake::Clock::time_point last_sync_sent{};
  unsigned sync_attempts = 0;
};

bool write_bytes(int fd, std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

void close_transport(Profile& profile, bool preserve_handoff = false) {
  if (profile.watch) {
    g_source_remove(profile.watch);
    profile.watch = 0;
  }
  if (profile.transport >= 0) {
    close(profile.transport);
    profile.transport = -1;
  }
  profile.reader = headunit::LinkFrameReader{};
  if (!preserve_handoff || !profile.engine || !profile.engine->handoff_sent())
    profile.engine.reset();
  profile.sync_attempts = 0;
}

gboolean transport_ready(gint fd, GIOCondition condition, gpointer data) {
  auto& profile = *static_cast<Profile*>(data);
  if ((condition & (G_IO_ERR | G_IO_NVAL)) || ((condition & G_IO_HUP) && !(condition & G_IO_IN))) {
    std::cout << "transport closed\n" << std::flush;
    profile.watch = 0;
    close_transport(profile, true);
    return G_SOURCE_REMOVE;
  }
  std::array<char, 4096> packet{};
  const ssize_t count = read(fd, packet.data(), packet.size());
  if (count > 0) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(packet.data());
    if (profile.iap2_test) {
      try {
        if (!profile.engine) throw std::runtime_error("iAP2 session missing");
        const bool was_established = profile.engine->established();
        const auto output = profile.engine->receive_bluetooth(
            {bytes, static_cast<std::size_t>(count)}, headunit::Iap2SessionEngine::Clock::now());
        for (const auto& frame : output.bluetooth) {
          if (!write_bytes(fd, frame)) throw std::runtime_error("iAP2 reply write failed");
        }
        if (!was_established && profile.engine->established()) {
          std::cout << "iAP2 link established\n";
        }
        for (const auto& message : output.unhandled) {
          std::cout << "iAP2 control id=" << message.identifier
                    << " parameter-bytes=" << message.parameters.size() << '\n';
        }
        if (profile.engine->handoff_sent()) {
          std::cout << "Wi-Fi handoff details sent (values redacted)\n";
        }
      } catch (const std::exception& error) {
        std::cerr << "iAP2 session failed: " << error.what() << '\n';
        profile.watch = 0;
        close_transport(profile);
        return G_SOURCE_REMOVE;
      }
    } else {
      for (const auto& frame : profile.reader.feed({bytes, static_cast<std::size_t>(count)})) {
        std::cout << "iAP2 link control=" << unsigned(frame.control)
                  << " seq=" << unsigned(frame.sequence)
                  << " ack=" << unsigned(frame.acknowledgement)
                  << " session=" << unsigned(frame.session)
                  << " payload-bytes=" << frame.payload.size() << '\n';
      }
    }
    std::cout << std::flush;
    return G_SOURCE_CONTINUE;
  }
  if (count < 0 && (errno == EAGAIN || errno == EINTR)) return G_SOURCE_CONTINUE;
  profile.watch = 0;
  close_transport(profile, true);
  return G_SOURCE_REMOVE;
}

void profile_method(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                    GVariant* arguments, GDBusMethodInvocation* invocation, gpointer data) {
  auto& profile = *static_cast<Profile*>(data);
  const std::string_view name(method);
  if (name == "Release") {
    profile.registered = false;
    close_transport(profile);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    g_main_loop_quit(profile.loop);
    return;
  }
  if (name == "RequestDisconnection") {
    close_transport(profile, true);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (name == "NewConnection") {
    if (profile.transport >= 0) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                 "probe already has a transport");
      return;
    }
    const char* device = nullptr;
    gint handle = -1;
    GVariant* properties = nullptr;
    g_variant_get(arguments, "(&oh@a{sv})", &device, &handle, &properties);
    g_variant_unref(properties);
    GDBusMessage* message = g_dbus_method_invocation_get_message(invocation);
    GUnixFDList* descriptors = g_dbus_message_get_unix_fd_list(message);
    GError* error = nullptr;
    const int transport = descriptors ? g_unix_fd_list_get(descriptors, handle, &error) : -1;
    if (transport < 0) {
      const std::string detail = error ? error->message : "missing transport descriptor";
      if (error) g_error_free(error);
      g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                 detail.c_str());
      return;
    }
    profile.transport = transport;
    if (profile.iap2_test) {
      profile.engine = std::make_shared<headunit::Iap2SessionEngine>(
          initial_sequence, profile.identity, profile.mfi_chip.get(), profile.handoff);
      const auto marker = headunit::LinkHandshake::sync_preamble();
      if (!write_bytes(transport, marker)) {
        close_transport(profile);
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                                   "cannot send iAP2 sync marker");
        return;
      }
      profile.last_sync_sent = headunit::LinkHandshake::Clock::now();
      profile.sync_attempts = 1;
      std::cout << "iAP2 sync marker sent\n" << std::flush;
    }
    profile.watch =
        g_unix_fd_add(transport, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
                      transport_ready, &profile);
    std::cout << "transport accepted from BlueZ device object " << device << '\n' << std::flush;
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotSupported",
                                             "unknown profile method");
}

const GDBusInterfaceVTable profile_methods{profile_method, nullptr, nullptr, {nullptr}};

gboolean stop_after_timeout(gpointer data) {
  g_main_loop_quit(static_cast<Profile*>(data)->loop);
  return G_SOURCE_REMOVE;
}

gboolean retry_handshake(gpointer data) {
  auto& profile = *static_cast<Profile*>(data);
  if (!profile.iap2_test || profile.transport < 0 || !profile.engine) {
    return G_SOURCE_CONTINUE;
  }
  const auto now = headunit::LinkHandshake::Clock::now();
  if (profile.engine->handshake_state() == headunit::LinkHandshakeState::waiting_for_sync &&
      now >= profile.last_sync_sent &&
      now - profile.last_sync_sent >= std::chrono::milliseconds(200)) {
    if (profile.sync_attempts >= 10) {
      std::cerr << "iAP2 sync response timed out\n";
      close_transport(profile);
      return G_SOURCE_CONTINUE;
    }
    const auto marker = headunit::LinkHandshake::sync_preamble();
    if (!write_bytes(profile.transport, marker)) {
      std::cerr << "iAP2 sync retry failed\n";
      close_transport(profile);
      return G_SOURCE_CONTINUE;
    }
    profile.last_sync_sent = now;
    ++profile.sync_attempts;
  }
  const auto output = profile.engine->tick(now);
  for (const auto& frame : output.bluetooth) {
    if (!write_bytes(profile.transport, frame)) {
      std::cerr << "iAP2 frame retry failed\n";
      close_transport(profile);
      break;
    }
  }
  return G_SOURCE_CONTINUE;
}

void check_dbus(GVariant* result, GError* error, const char* operation) {
  if (result) {
    g_variant_unref(result);
    return;
  }
  const std::string detail = error ? error->message : "unknown D-Bus error";
  if (error) g_error_free(error);
  throw std::runtime_error(std::string(operation) + ": " + detail);
}

std::array<std::uint8_t, 6> adapter_address(GDBusConnection* bus) {
  GError* error = nullptr;
  GVariant* result = g_dbus_connection_call_sync(
      bus, "org.bluez", "/org/bluez/hci0", "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", "org.bluez.Adapter1", "Address"), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  if (!result) {
    const std::string detail = error ? error->message : "adapter address unavailable";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  GVariant* wrapped = nullptr;
  g_variant_get(result, "(@v)", &wrapped);
  GVariant* value = g_variant_get_variant(wrapped);
  const std::string address = g_variant_get_string(value, nullptr);
  g_variant_unref(value);
  g_variant_unref(wrapped);
  g_variant_unref(result);

  unsigned parts[6]{};
  if (std::sscanf(address.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &parts[0], &parts[1], &parts[2],
                  &parts[3], &parts[4], &parts[5]) != 6) {
    throw std::runtime_error("invalid BlueZ adapter address");
  }
  return {static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
          static_cast<std::uint8_t>(parts[2]), static_cast<std::uint8_t>(parts[3]),
          static_cast<std::uint8_t>(parts[4]), static_cast<std::uint8_t>(parts[5])};
}

headunit::Iap2WifiConfig load_wifi_config(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("cannot open private Wi-Fi config");
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & 077) != 0 ||
      (info.st_uid != getuid() && info.st_uid != 0) || info.st_size > 512) {
    close(fd);
    throw std::runtime_error("Wi-Fi config must be a private regular file");
  }
  std::string contents;
  std::array<char, 513> buffer{};
  for (;;) {
    const auto count = read(fd, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      close(fd);
      throw std::runtime_error("cannot read Wi-Fi config");
    }
    if (count == 0) break;
    contents.append(buffer.data(), static_cast<std::size_t>(count));
    if (contents.size() > 512) {
      close(fd);
      throw std::runtime_error("Wi-Fi config is too large");
    }
  }
  close(fd);

  headunit::Iap2WifiConfig config;
  bool has_ssid = false;
  bool has_passphrase = false;
  bool has_channel = false;
  std::istringstream lines(contents);
  std::string line;
  while (std::getline(lines, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      throw std::runtime_error("invalid Wi-Fi config line");
    }
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "ssid" && !has_ssid) {
      config.ssid = value;
      has_ssid = true;
    } else if (key == "passphrase" && !has_passphrase) {
      config.passphrase = value;
      has_passphrase = true;
    } else if (key == "channel" && !has_channel) {
      std::size_t consumed = 0;
      const auto parsed = std::stoul(value, &consumed);
      if (consumed != value.size() || parsed == 0 || parsed > 255) {
        throw std::runtime_error("invalid Wi-Fi channel");
      }
      config.channel = static_cast<std::uint8_t>(parsed);
      has_channel = true;
    } else {
      throw std::runtime_error("unknown or repeated Wi-Fi config key");
    }
  }
  if (!has_ssid || !has_passphrase || !has_channel) {
    throw std::runtime_error("incomplete Wi-Fi config");
  }
  return config;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc == 3 && std::string_view(argv[1]) == "--check-wifi-config") {
    const headunit::Iap2WifiHandoff handoff(load_wifi_config(argv[2]));
    (void)handoff;
    std::cout << "private Wi-Fi configuration valid\n";
    return 0;
  }
  if (argc != 2 && argc != 3 && argc != 5) {
    throw std::invalid_argument(
        "usage: headunit-rfcomm-probe SECONDS [--iap2-test [--wifi-config PATH]]");
  }
  const bool iap2_test = argc >= 3 && std::string_view(argv[2]) == "--iap2-test";
  if (argc >= 3 && !iap2_test) {
    throw std::invalid_argument("unknown RFCOMM probe option");
  }
  if (argc == 5 && std::string_view(argv[3]) != "--wifi-config") {
    throw std::invalid_argument("unknown RFCOMM probe option");
  }
  char* end = nullptr;
  const long seconds = std::strtol(argv[1], &end, 10);
  if (!end || *end != '\0' || seconds < 1 || seconds > 120) {
    throw std::invalid_argument("probe duration must be 1..120 seconds");
  }
  GError* error = nullptr;
  Profile profile;
  profile.iap2_test = iap2_test;
  profile.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (!profile.bus) {
    const std::string detail = error ? error->message : "system bus unavailable";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  if (iap2_test) {
    profile.identity.bluetooth_address = adapter_address(profile.bus);
    profile.identity.messages_sent = {0x5703};
    profile.identity.messages_received = {0x5702};
    profile.mfi_bus = std::make_unique<headunit::LinuxMfiBus>("/dev/i2c-1", 0x10);
    profile.mfi_chip = std::make_unique<headunit::MfiCoprocessor>(*profile.mfi_bus);
    if (argc == 5) {
      profile.handoff.emplace(load_wifi_config(argv[4]));
    }
  }
  profile.loop = g_main_loop_new(nullptr, FALSE);
  GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(profile_xml, &error);
  if (!node) {
    const std::string detail = error ? error->message : "invalid profile XML";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  const guint object_id = g_dbus_connection_register_object(
      profile.bus, profile_path, node->interfaces[0], &profile_methods, &profile, nullptr, &error);
  if (!object_id) {
    const std::string detail = error ? error->message : "cannot export profile";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(
      &options, "{sv}", "Name",
      g_variant_new_string(iap2_test ? "Wireless iAP" : "Headunit transport probe"));
  g_variant_builder_add(&options, "{sv}", "Role", g_variant_new_string("server"));
  g_variant_builder_add(&options, "{sv}", "Channel",
                        g_variant_new_uint16(iap2_test ? iap2_channel : test_channel));
  g_variant_builder_add(&options, "{sv}", "RequireAuthentication", g_variant_new_boolean(TRUE));
  GVariant* result = g_dbus_connection_call_sync(
      profile.bus, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile",
      g_variant_new("(os@a{sv})", profile_path, iap2_test ? iap2_uuid : test_uuid,
                    g_variant_builder_end(&options)),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  check_dbus(result, error, "BlueZ profile registration failed");
  profile.registered = true;
  std::cout << "test RFCOMM profile registered on channel "
            << (iap2_test ? iap2_channel : test_channel) << '\n'
            << std::flush;
  g_timeout_add(50, retry_handshake, &profile);
  g_timeout_add_seconds(static_cast<guint>(seconds), stop_after_timeout, &profile);
  g_main_loop_run(profile.loop);
  close_transport(profile);
  if (profile.registered) {
    result = g_dbus_connection_call_sync(
        profile.bus, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile",
        g_variant_new("(o)", profile_path), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    check_dbus(result, error, "BlueZ profile removal failed");
  }
  g_dbus_connection_unregister_object(profile.bus, object_id);
  g_dbus_node_info_unref(node);
  g_main_loop_unref(profile.loop);
  g_object_unref(profile.bus);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
