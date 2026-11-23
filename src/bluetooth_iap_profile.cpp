// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/bluetooth_iap_profile.hpp"

#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace headunit {
namespace {
constexpr char profile_path[] = "/org/example/HeadunitIap";
constexpr char iap_uuid[] = "00000000-deca-fade-deca-deafdecacaff";
// The phone's own iAP service. Connecting to it as a client is how an
// accessory restarts iAP; BlueZ then delivers NewConnection as usual.
constexpr char client_path[] = "/org/example/HeadunitIapClient";
constexpr char phone_iap_uuid[] = "00000000-deca-fade-deca-deafdecacafe";
constexpr std::uint8_t initial_sequence = 99;
constexpr char profile_xml[] = R"xml(
<node><interface name="org.bluez.Profile1">
  <method name="Release"/>
  <method name="NewConnection">
    <arg type="o" direction="in"/><arg type="h" direction="in"/>
    <arg type="a{sv}" direction="in"/>
  </method>
  <method name="RequestDisconnection"><arg type="o" direction="in"/></method>
</interface></node>)xml";

void check_reply(GVariant* reply, GError* error, const char* action) {
  if (reply) {
    g_variant_unref(reply);
    return;
  }
  const std::string detail = error ? error->message : "unknown D-Bus error";
  if (error) g_error_free(error);
  throw std::runtime_error(std::string(action) + ": " + detail);
}

// Classify fixed protocol errors only. Remote names and message bodies may
// contain device identities; never echo an arbitrary daemon response.
const char* connect_error_category(GError* error) {
  auto* remote = g_dbus_error_get_remote_error(error);
  const char* category = "other_remote";
  if (remote) {
    if (g_str_equal(remote, "org.bluez.Error.InProgress"))
      category = "in_progress";
    else if (g_str_equal(remote, "org.bluez.Error.Failed"))
      category = "failed";
    else if (g_str_equal(remote, "org.bluez.Error.NotReady"))
      category = "not_ready";
    else if (g_str_equal(remote, "org.bluez.Error.NotAvailable"))
      category = "not_available";
    else if (g_str_equal(remote, "org.bluez.Error.NotSupported"))
      category = "not_supported";
    g_free(remote);
  } else {
    category = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
                       g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NO_REPLY)
                   ? "timeout"
                   : "local_error";
  }
  return category;
}

// Kernel L2CAP socket ABI (linux/bluetooth/l2cap.h), declared here so the
// receiver needs no libbluetooth headers.
constexpr int af_bluetooth = 31;
constexpr int btproto_l2cap = 0;
constexpr std::uint16_t sdp_psm = 1;
constexpr std::uint8_t bdaddr_bredr = 0;
struct ClassicL2capAddress {
  sa_family_t family;
  std::uint16_t psm;
  std::array<std::uint8_t, 6> address;
  std::uint16_t cid;
  std::uint8_t address_type;
};

// Pages a phone over classic Bluetooth only: an L2CAP connection to its SDP
// channel. Unlike Device1.ConnectProfile, a page timeout ends here instead of
// falling back to a Low Energy connection.
int open_classic_page(std::string_view device_path) {
  const auto start = device_path.rfind("/dev_");
  unsigned parts[6]{};
  const std::string name(device_path.substr(start == std::string_view::npos ? 0 : start + 5));
  if (start == std::string_view::npos || name.size() != 17 ||
      std::sscanf(name.c_str(), "%2x_%2x_%2x_%2x_%2x_%2x", &parts[0], &parts[1], &parts[2],
                  &parts[3], &parts[4], &parts[5]) != 6) {
    errno = EINVAL;
    return -1;
  }
  ClassicL2capAddress remote{};
  remote.family = af_bluetooth;
  remote.psm = htole16(sdp_psm);
  for (std::size_t i = 0; i < 6; ++i) {
    // BlueZ addresses are stored least-significant byte first.
    remote.address[i] = static_cast<std::uint8_t>(parts[5 - i]);
  }
  remote.address_type = bdaddr_bredr;
  const int fd = socket(af_bluetooth, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, btproto_l2cap);
  if (fd < 0) return -1;
  if (connect(fd, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) == 0 ||
      errno == EINPROGRESS) {
    return fd;
  }
  const int error = errno;
  close(fd);
  errno = error;
  return -1;
}

const char* page_error_reason(int error) {
  switch (error) {
    case EHOSTDOWN:
      return "page-timeout";
    case ETIMEDOUT:
      return "timeout";
    case ECONNREFUSED:
      return "refused";
    case EHOSTUNREACH:
    case ENETDOWN:
    case ENODEV:
      return "adapter-unavailable";
    case EACCES:
    case EPERM:
      return "permission";
    default:
      return "other";
  }
}

const char* connect_error_reason(GError* error) {
  g_dbus_error_strip_remote_error(error);
  for (const char* reason : {"br-connection-busy", "br-connection-page-timeout",
                             "br-connection-abort-by-local", "br-connection-abort-by-remote",
                             "br-connection-profile-unavailable", "le-connection-abort-by-local"}) {
    if (g_strcmp0(error->message, reason) == 0) return reason;
  }
  return "other";
}
}  // namespace

Iap2WifiConfig load_private_iap_wifi_config(const std::string& path) {
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
  Iap2WifiConfig config;
  bool ssid = false, passphrase = false, channel = false;
  std::istringstream lines(contents);
  std::string line;
  while (std::getline(lines, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos) throw std::runtime_error("invalid Wi-Fi config line");
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "ssid" && !ssid) {
      config.ssid = value;
      ssid = true;
    } else if (key == "passphrase" && !passphrase) {
      config.passphrase = value;
      passphrase = true;
    } else if (key == "channel" && !channel) {
      std::size_t consumed = 0;
      const auto parsed = std::stoul(value, &consumed);
      if (consumed != value.size() || parsed == 0 || parsed > 255) {
        throw std::runtime_error("invalid Wi-Fi channel");
      }
      config.channel = static_cast<std::uint8_t>(parsed);
      channel = true;
    } else {
      throw std::runtime_error("unknown or repeated Wi-Fi config key");
    }
  }
  if (!ssid || !passphrase || !channel) {
    throw std::runtime_error("incomplete Wi-Fi config");
  }
  return config;
}

BluetoothIapProfile::BluetoothIapProfile(MfiAuthenticator& chip, Iap2WifiConfig wifi,
                                         std::optional<Iap2CarplayStart> carplay,
                                         Iap2Identity identity, std::string selected_device,
                                         PageOpener page, std::chrono::milliseconds page_timeout,
                                         std::chrono::milliseconds expected_window)
    : chip_(chip),
      handoff_(std::move(wifi)),
      carplay_(std::move(carplay)),
      identity_(std::move(identity)),
      selected_device_(std::move(selected_device)),
      page_(page ? std::move(page) : PageOpener(open_classic_page)),
      page_timeout_(page_timeout),
      expected_window_(expected_window) {
  // An absent phone fails the page itself after the controller's 5.12 s page
  // timeout. This bound only catches a link that never finishes: an iPhone
  // can take several seconds to answer its first L2CAP request after a
  // disconnect, so it must comfortably exceed page plus channel setup.
  if (page_timeout_ < std::chrono::milliseconds(100) || page_timeout_ > std::chrono::seconds(60))
    throw std::invalid_argument("Bluetooth page timeout must be 0.1..60 s");
  if (expected_window_ < std::chrono::milliseconds(0) ||
      expected_window_ > std::chrono::minutes(10))
    throw std::invalid_argument("Bluetooth expected-reconnect window must be 0..600 s");
  if (carplay_) (void)make_carplay_start(*carplay_);
}

BluetoothIapProfile::~BluetoothIapProfile() {
  if (timer_) g_source_remove(timer_);
  if (device_signal_ && bus_) g_dbus_connection_signal_unsubscribe(bus_, device_signal_);
  close_page();
  if (connect_cancel_) {
    g_cancellable_cancel(connect_cancel_);
    g_object_unref(connect_cancel_);
  }
  close_transport(false);
  const auto unregister = [&](bool registered, const char* path) {
    if (!registered || !bus_) return;
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus_, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile",
        g_variant_new("(o)", path), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    if (reply) g_variant_unref(reply);
    if (error) g_error_free(error);
  };
  unregister(registered_, profile_path);
  unregister(client_registered_, client_path);
  if (client_object_id_ && bus_) g_dbus_connection_unregister_object(bus_, client_object_id_);
  if (object_id_ && bus_) g_dbus_connection_unregister_object(bus_, object_id_);
  if (node_) g_dbus_node_info_unref(node_);
  if (bus_) g_object_unref(bus_);
}

std::array<std::uint8_t, 6> BluetoothIapProfile::adapter_address() const {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      bus_, "org.bluez", "/org/bluez/hci0", "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", "org.bluez.Adapter1", "Address"), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  if (!reply) {
    const std::string detail = error ? error->message : "adapter address unavailable";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  GVariant* wrapped = nullptr;
  g_variant_get(reply, "(@v)", &wrapped);
  GVariant* value = g_variant_get_variant(wrapped);
  const std::string address = g_variant_get_string(value, nullptr);
  g_variant_unref(value);
  g_variant_unref(wrapped);
  g_variant_unref(reply);
  unsigned parts[6]{};
  if (std::sscanf(address.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &parts[0], &parts[1], &parts[2],
                  &parts[3], &parts[4], &parts[5]) != 6) {
    throw std::runtime_error("invalid Bluetooth adapter address");
  }
  return {static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
          static_cast<std::uint8_t>(parts[2]), static_cast<std::uint8_t>(parts[3]),
          static_cast<std::uint8_t>(parts[4]), static_cast<std::uint8_t>(parts[5])};
}

void BluetoothIapProfile::start() {
  if (bus_) throw std::runtime_error("Bluetooth profile already started");
  GError* error = nullptr;
  bus_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (!bus_) {
    const std::string detail = error ? error->message : "system bus unavailable";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  identity_.bluetooth_address = adapter_address();
  identity_.messages_sent = {0x5703};
  identity_.messages_received = {0x5702};
  if (carplay_) {
    identity_.supports_theme_assets = carplay_->installed_asset.has_value();
    identity_.messages_sent.push_back(0x4301);
    identity_.messages_received.push_back(0x4300);
  }
  node_ = g_dbus_node_info_new_for_xml(profile_xml, &error);
  if (!node_) {
    const std::string detail = error ? error->message : "invalid profile XML";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  static const GDBusInterfaceVTable methods{method_call, nullptr, nullptr, {nullptr}};
  object_id_ = g_dbus_connection_register_object(bus_, profile_path, node_->interfaces[0], &methods,
                                                 this, nullptr, &error);
  if (!object_id_) {
    const std::string detail = error ? error->message : "cannot export profile";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options, "{sv}", "Name", g_variant_new_string("Wireless iAP"));
  g_variant_builder_add(&options, "{sv}", "Role", g_variant_new_string("server"));
  g_variant_builder_add(&options, "{sv}", "Channel", g_variant_new_uint16(3));
  g_variant_builder_add(&options, "{sv}", "RequireAuthentication", g_variant_new_boolean(TRUE));
  GVariant* reply = g_dbus_connection_call_sync(
      bus_, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile",
      g_variant_new("(os@a{sv})", profile_path, iap_uuid, g_variant_builder_end(&options)), nullptr,
      G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  check_reply(reply, error, "BlueZ profile registration failed");
  registered_ = true;
  if (!selected_device_.empty()) {
    // Optional: without it the phone must reopen iAP itself, as before.
    client_object_id_ = g_dbus_connection_register_object(bus_, client_path, node_->interfaces[0],
                                                          &methods, this, nullptr, &error);
    GVariantBuilder client;
    g_variant_builder_init(&client, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&client, "{sv}", "Name", g_variant_new_string("Wireless iAP client"));
    g_variant_builder_add(&client, "{sv}", "Role", g_variant_new_string("client"));
    g_variant_builder_add(&client, "{sv}", "RequireAuthentication", g_variant_new_boolean(TRUE));
    reply = client_object_id_ ? g_dbus_connection_call_sync(
                                    bus_, "org.bluez", "/org/bluez", "org.bluez.ProfileManager1",
                                    "RegisterProfile",
                                    g_variant_new("(os@a{sv})", client_path, phone_iap_uuid,
                                                  g_variant_builder_end(&client)),
                                    nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error)
                              : nullptr;
    if (!client_object_id_) g_variant_builder_clear(&client);
    if (reply) {
      g_variant_unref(reply);
      client_registered_ = true;
      connect_cancel_ = g_cancellable_new();
      device_signal_ = g_dbus_connection_signal_subscribe(
          bus_, "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged",
          selected_device_.c_str(), "org.bluez.Device1", G_DBUS_SIGNAL_FLAGS_NONE, device_changed,
          this, nullptr);
    } else {
      std::cerr << "iAP accessory reconnect unavailable: "
                << (error ? error->message : "cannot export client profile") << '\n';
    }
    if (error) g_error_free(error);
  }
  timer_ = g_timeout_add(50, retry, this);
  std::cout << "independent Wireless iAP profile ready\n" << std::flush;
}

void BluetoothIapProfile::pump() {
  for (unsigned count = 0; count < 32; ++count) {
    if (!g_main_context_iteration(nullptr, FALSE)) break;
  }
}

void BluetoothIapProfile::append_poll_descriptors(std::vector<pollfd>& descriptors) const {
  if (transport_ < 0) return;
  const auto events = static_cast<short>(POLLIN | (pending_writes_.empty() ? 0 : POLLOUT));
  descriptors.push_back({transport_, events, 0});
}

void BluetoothIapProfile::update_watch() {
  const bool output = !pending_writes_.empty();
  if (watch_ && output == watch_output_) return;
  if (watch_) g_source_remove(watch_);
  watch_ = 0;
  if (transport_ < 0) return;
  watch_output_ = output;
  watch_ = g_unix_fd_add(
      transport_,
      static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | (output ? G_IO_OUT : 0)),
      transport_ready, this);
  if (!watch_) throw std::runtime_error("cannot watch iAP transport");
}

bool BluetoothIapProfile::queue_write(std::span<const std::uint8_t> bytes) {
  constexpr std::size_t maximum_bytes = 65536, maximum_frames = 64;
  if (transport_ < 0 || bytes.empty() || bytes.size() > maximum_bytes - pending_write_bytes_ ||
      pending_writes_.size() >= maximum_frames)
    return false;
  pending_writes_.emplace_back(bytes.begin(), bytes.end());
  pending_write_bytes_ += bytes.size();
  if (!write_started_) write_started_ = Iap2SessionEngine::Clock::now();
  if (!flush_writes()) return false;
  update_watch();
  return true;
}

bool BluetoothIapProfile::flush_writes() {
  unsigned attempts = 0;
  while (!pending_writes_.empty() && attempts++ < 128) {
    const auto& bytes = pending_writes_.front();
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count =
        send(transport_, bytes.data() + write_offset_, bytes.size() - write_offset_, flags);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    if (count <= 0) return false;
    write_offset_ += static_cast<std::size_t>(count);
    pending_write_bytes_ -= static_cast<std::size_t>(count);
    if (write_offset_ == bytes.size()) {
      pending_writes_.pop_front();
      write_offset_ = 0;
    }
  }
  if (pending_writes_.empty()) write_started_.reset();
  return true;
}

void BluetoothIapProfile::release_session(const std::shared_ptr<Iap2SessionEngine>& engine) {
  if (engine && engine == engine_) close_transport(false);
}

void BluetoothIapProfile::maintain_connection(bool idle) {
  const auto now = Iap2SessionEngine::Clock::now();
  idle = idle && transport_ < 0 && !engine_;
  if (!idle) {
    idle_since_.reset();
    connect_failures_ = 0;
    initial_connection_ = false;
    return;
  }
  if (!idle_since_) {
    // The explicitly selected phone can connect immediately at startup.
    // After a session, give a phone reconnecting itself the first chance.
    idle_since_ = now;
    next_connect_ = now + std::chrono::seconds(initial_connection_ ? 0 : 3);
    initial_connection_ = false;
  }
  if (!client_registered_ || connect_pending_ || now < next_connect_) return;
  connect_pending_ = true;
  connect_started_ = now;
  page_fd_ = page_(selected_device_);
  if (page_fd_ < 0) {
    const int error = errno;
    connect_pending_ = false;
    connect_failed("page", page_error_reason(error), error);
    return;
  }
  page_watch_ = g_unix_fd_add(page_fd_, static_cast<GIOCondition>(G_IO_OUT | G_IO_ERR | G_IO_HUP),
                              page_ready, this);
  page_timer_ = g_timeout_add(static_cast<guint>(page_timeout_.count()), page_expired, this);
}

gboolean BluetoothIapProfile::page_ready(gint, GIOCondition, gpointer data) {
  auto* owner = static_cast<BluetoothIapProfile*>(data);
  owner->page_watch_ = 0;
  owner->on_page_ready();
  return G_SOURCE_REMOVE;
}

void BluetoothIapProfile::on_page_ready() {
  if (profile_requested_) return;  // The link-up signal already asked BlueZ.
  if (page_timer_) {
    g_source_remove(page_timer_);
    page_timer_ = 0;
  }
  if (transport_ >= 0 || engine_) {
    // The phone reconnected by itself while it was being paged.
    close_page();
    connect_pending_ = false;
    return;
  }
  int error = 0;
  socklen_t size = sizeof(error);
  if (getsockopt(page_fd_, SOL_SOCKET, SO_ERROR, &error, &size) != 0) error = errno;
  if (error) {
    close_page();
    connect_pending_ = false;
    connect_failed("page", page_error_reason(error), error);
    return;
  }
  // Keep the page link open: it carries BlueZ's iAP connection, so BlueZ
  // never pages (or falls back to Low Energy) itself.
  request_profile();
}

gboolean BluetoothIapProfile::page_expired(gpointer data) {
  auto* owner = static_cast<BluetoothIapProfile*>(data);
  owner->page_timer_ = 0;
  owner->close_page();
  owner->connect_pending_ = false;
  owner->connect_failed("page", "timeout");
  return G_SOURCE_REMOVE;
}

void BluetoothIapProfile::device_changed(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                         const gchar*, GVariant* parameters, gpointer data) {
  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv}as)"))) return;
  GVariant* changed = g_variant_get_child_value(parameters, 1);
  gboolean connected = FALSE;
  const bool present = g_variant_lookup(changed, "Connected", "b", &connected);
  g_variant_unref(changed);
  if (present && connected) static_cast<BluetoothIapProfile*>(data)->on_link_up();
}

void BluetoothIapProfile::on_link_up() {
  // Only an outstanding page is advanced; otherwise the link belongs to a
  // session or to the phone reconnecting itself.
  if (page_fd_ < 0 || profile_requested_ || transport_ >= 0 || engine_) return;
  if (page_watch_) {
    g_source_remove(page_watch_);
    page_watch_ = 0;
  }
  if (page_timer_) {
    g_source_remove(page_timer_);
    page_timer_ = 0;
  }
  // Keep the page descriptor until BlueZ replies: it holds the link up.
  request_profile();
}

void BluetoothIapProfile::close_page() {
  if (page_watch_) {
    g_source_remove(page_watch_);
    page_watch_ = 0;
  }
  if (page_timer_) {
    g_source_remove(page_timer_);
    page_timer_ = 0;
  }
  if (page_fd_ >= 0) {
    close(page_fd_);
    page_fd_ = -1;
  }
}

void BluetoothIapProfile::request_profile() {
  profile_requested_ = true;
  g_dbus_connection_call(bus_, "org.bluez", selected_device_.c_str(), "org.bluez.Device1",
                         "ConnectProfile", g_variant_new("(s)", phone_iap_uuid), nullptr,
                         G_DBUS_CALL_FLAGS_NONE, 20000, connect_cancel_, connect_done, this);
}

void BluetoothIapProfile::connect_failed(const char* category, const char* reason,
                                         std::optional<int> native_error) {
  const auto now = Iap2SessionEngine::Clock::now();
  const unsigned failures = std::min(connect_failures_++, 4u);
  // A phone that did not answer a page while it is expected back is paged
  // again almost at once. Otherwise (absent phone, BlueZ or local errors)
  // page again later, at most once a minute.
  const bool unanswered =
      std::string_view(reason) == "page-timeout" ||
      (std::string_view(category) == "page" && std::string_view(reason) == "timeout");
  const bool expected = idle_since_ && now - *idle_since_ < expected_window_;
  const unsigned retry_s = unanswered && expected ? 1u : 4u << failures;
  next_connect_ = now + std::chrono::seconds(retry_s);
  std::cout << "iAP accessory reconnect attempt failed failures=" << connect_failures_
            << " retry_s=" << retry_s << " error=" << category << " reason=" << reason
            << " elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(now - connect_started_).count()
            << " native_errno=";
  // Timer expiry and D-Bus failures do not supply a native socket errno.
  // Keep their fixed classification without inventing a kernel result.
  if (native_error)
    std::cout << *native_error;
  else
    std::cout << "unavailable";
  std::cout << '\n' << std::flush;
}

void BluetoothIapProfile::connect_done(GObject* source, GAsyncResult* result, gpointer data) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
  if (reply) g_variant_unref(reply);
  // A cancelled call may complete after its owner was destroyed.
  if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_error_free(error);
    return;
  }
  auto* owner = static_cast<BluetoothIapProfile*>(data);
  owner->connect_pending_ = false;
  owner->profile_requested_ = false;
  // BlueZ's iAP connection now holds the link, or the request failed.
  owner->close_page();
  if (error) {
    owner->connect_failed(connect_error_category(error), connect_error_reason(error));
    g_error_free(error);
  } else {
    owner->next_connect_ = Iap2SessionEngine::Clock::now() + std::chrono::seconds(10);
    std::cout << "iAP accessory reconnect requested\n" << std::flush;
  }
}

void BluetoothIapProfile::close_transport(bool preserve_handoff) {
  if (watch_) {
    g_source_remove(watch_);
    watch_ = 0;
  }
  if (transport_ >= 0) {
    // bluetoothd keeps a duplicate of this RFCOMM socket. close() alone
    // leaves the link up, so neither the phone nor BlueZ would ever start a
    // fresh iAP connection. shutdown() ends the shared connection itself.
    shutdown(transport_, SHUT_RDWR);
    close(transport_);
    transport_ = -1;
  }
  const bool keep_engine =
      preserve_handoff && engine_ && engine_->handoff_sent() && !engine_->failed();
  if (!keep_engine) {
    if (engine_) engine_->retire();
    engine_.reset();
  }
  sync_attempts_ = 0;
  pending_writes_.clear();
  pending_write_bytes_ = write_offset_ = 0;
  write_started_.reset();
  watch_output_ = false;
}

void BluetoothIapProfile::method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                      const gchar* method, GVariant* args,
                                      GDBusMethodInvocation* invocation, gpointer data) {
  auto* owner = static_cast<BluetoothIapProfile*>(data);
  try {
    owner->on_method(method, args, invocation);
  } catch (...) {
    owner->close_transport(false);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                               "iAP profile operation failed");
  }
}

void BluetoothIapProfile::on_method(const gchar* method, GVariant* args,
                                    GDBusMethodInvocation* invocation) {
  if (std::string_view(method) == "Release") {
    registered_ = false;
    close_transport(false);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::string_view(method) == "RequestDisconnection") {
    close_transport(true);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::string_view(method) != "NewConnection") {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotSupported",
                                               "unknown profile method");
    return;
  }
  if (transport_ >= 0 || (engine_ && engine_->wifi_active())) {
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                               "iAP session already active");
    return;
  }
  const char* device = nullptr;
  gint handle = -1;
  GVariant* properties = nullptr;
  g_variant_get(args, "(&oh@a{sv})", &device, &handle, &properties);
  g_variant_unref(properties);
  if (!selected_device_.empty() && selected_device_ != device) {
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.bluez.Error.Rejected",
        "Select this phone in the native connection page first");
    return;
  }
  GDBusMessage* message = g_dbus_method_invocation_get_message(invocation);
  GUnixFDList* descriptors = g_dbus_message_get_unix_fd_list(message);
  GError* error = nullptr;
  const int fd = descriptors ? g_unix_fd_list_get(descriptors, handle, &error) : -1;
  if (fd < 0) {
    const std::string detail = error ? error->message : "missing transport descriptor";
    if (error) g_error_free(error);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                               detail.c_str());
    return;
  }
  if (generation_ == UINT64_MAX) {
    close(fd);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                               "iAP generation exhausted");
    return;
  }
  ++generation_;
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                               "cannot make iAP transport nonblocking");
    return;
  }
#ifndef MSG_NOSIGNAL
  const int suppress_sigpipe = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &suppress_sigpipe, sizeof(suppress_sigpipe)) < 0) {
    close(fd);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                               "cannot suppress transport SIGPIPE");
    return;
  }
#endif
  transport_ = fd;
  engine_ = std::make_shared<Iap2SessionEngine>(initial_sequence, identity_, &chip_, handoff_,
                                                carplay_, generation_);
  if (!queue_write(Iap2SessionEngine::sync_preamble())) {
    close_transport(false);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Rejected",
                                               "cannot send iAP sync marker");
    return;
  }
  last_sync_ = Iap2SessionEngine::Clock::now();
  sync_attempts_ = 1;
  update_watch();
  engine_->transport_accepted();
  if (page_watch_ || page_timer_) {
    // An independently accepted transport supersedes an unanswered page.
    // Its deadline must not report a failure after the phone has connected.
    close_page();
    connect_pending_ = false;
  }
  std::cout << "iAP Bluetooth transport accepted\n" << std::flush;
  g_dbus_method_invocation_return_value(invocation, nullptr);
  (void)device;
}

gboolean BluetoothIapProfile::transport_ready(gint fd, GIOCondition condition, gpointer data) {
  auto* owner = static_cast<BluetoothIapProfile*>(data);
  try {
    return owner->on_transport(fd, condition);
  } catch (...) {
    owner->close_transport(false);
    return G_SOURCE_REMOVE;
  }
}

gboolean BluetoothIapProfile::on_transport(gint fd, GIOCondition condition) {
  if (engine_ && engine_->wifi_active()) {
    close_transport(true);
    return G_SOURCE_REMOVE;
  }
  if ((condition & (G_IO_ERR | G_IO_NVAL)) || ((condition & G_IO_HUP) && !(condition & G_IO_IN))) {
    close_transport(true);
    return G_SOURCE_REMOVE;
  }
  if (condition & G_IO_OUT) {
    if (!flush_writes()) {
      close_transport(false);
      return G_SOURCE_REMOVE;
    }
    update_watch();
  }
  if (!(condition & G_IO_IN)) return G_SOURCE_CONTINUE;
  std::array<std::uint8_t, 4096> bytes{};
  const auto count = read(fd, bytes.data(), bytes.size());
  if (count > 0) {
    try {
      const bool already_started = engine_->carplay_start_sent();
      const auto output = engine_->receive_bluetooth(
          {bytes.data(), static_cast<std::size_t>(count)}, Iap2SessionEngine::Clock::now());
      for (const auto& frame : output.bluetooth) {
        if (!queue_write(frame)) throw std::runtime_error("Bluetooth reply failed");
      }
      if (!already_started && engine_->carplay_start_sent())
        std::cout << "authenticated CarPlay start queued with installed asset metadata\n"
                  << std::flush;
      for (const auto& control : output.unhandled) {
        std::cout << "iAP control id=" << control.identifier << '\n';
      }
    } catch (const std::exception& error) {
      std::cerr << "iAP Bluetooth session error: " << error.what() << '\n';
      close_transport(false);
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }
  if (count < 0 && (errno == EAGAIN || errno == EINTR)) return G_SOURCE_CONTINUE;
  close_transport(true);
  return G_SOURCE_REMOVE;
}

gboolean BluetoothIapProfile::retry(gpointer data) {
  auto* owner = static_cast<BluetoothIapProfile*>(data);
  try {
    return owner->on_retry();
  } catch (...) {
    owner->close_transport(false);
    return G_SOURCE_CONTINUE;
  }
}

gboolean BluetoothIapProfile::on_retry() {
  if (transport_ < 0 || !engine_) return G_SOURCE_CONTINUE;
  if (engine_->wifi_active()) return G_SOURCE_CONTINUE;
  const auto now = Iap2SessionEngine::Clock::now();
  // Local transport policy, independent of the reliable link's wire retries.
  if (write_started_ && now - *write_started_ >= std::chrono::seconds(2)) {
    close_transport(false);
    return G_SOURCE_CONTINUE;
  }
  if (engine_->handshake_state() == LinkHandshakeState::waiting_for_sync && now >= last_sync_ &&
      now - last_sync_ >= std::chrono::milliseconds(200)) {
    if (sync_attempts_ >= 10 || !queue_write(Iap2SessionEngine::sync_preamble())) {
      close_transport(false);
      return G_SOURCE_CONTINUE;
    }
    last_sync_ = now;
    ++sync_attempts_;
  }
  try {
    const auto output = engine_->tick(now);
    for (const auto& frame : output.bluetooth) {
      if (!queue_write(frame)) {
        close_transport(false);
        break;
      }
    }
  } catch (const std::exception&) {
    // No exception may cross the GLib callback boundary. A terminal link
    // must release this transport so a later phone connection can start fresh.
    std::cerr << "iAP Bluetooth session retired after timer failure\n" << std::flush;
    close_transport(false);
  }
  return G_SOURCE_CONTINUE;
}

}  // namespace headunit
