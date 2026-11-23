// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <gio/gio.h>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

bool boolean(GVariant* dictionary, const char* name) {
  gboolean value = FALSE;
  g_variant_lookup(dictionary, name, "b", &value);
  return value;
}

std::string label(GVariant* dictionary) {
  const char* keys[]{"Alias", "Name"};
  for (const char* key : keys) {
    GVariant* value = g_variant_lookup_value(dictionary, key, G_VARIANT_TYPE_STRING);
    if (value) {
      std::string result(g_variant_get_string(value, nullptr));
      g_variant_unref(value);
      return result;
    }
  }
  return "unnamed";
}

std::string snapshot(GDBusConnection* connection) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      connection, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
      nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
  if (!reply) {
    const std::string detail = error ? error->message : "unknown D-Bus error";
    if (error) g_error_free(error);
    throw std::runtime_error("BlueZ snapshot failed: " + detail);
  }
  GVariant* objects = g_variant_get_child_value(reply, 0);
  GVariantIter iterator;
  g_variant_iter_init(&iterator, objects);
  const char* path = nullptr;
  GVariant* interfaces = nullptr;
  std::ostringstream output;
  while (g_variant_iter_next(&iterator, "{&o@a{sa{sv}}}", &path, &interfaces)) {
    GVariant* adapter =
        g_variant_lookup_value(interfaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
    if (adapter) {
      output << "adapter " << label(adapter) << " powered=" << boolean(adapter, "Powered")
             << " discoverable=" << boolean(adapter, "Discoverable")
             << " pairable=" << boolean(adapter, "Pairable") << '\n';
      g_variant_unref(adapter);
    }
    GVariant* device =
        g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
    if (device && (boolean(device, "Paired") || boolean(device, "Connected"))) {
      output << "device " << label(device) << " paired=" << boolean(device, "Paired")
             << " connected=" << boolean(device, "Connected") << '\n';
    }
    if (device) g_variant_unref(device);
    g_variant_unref(interfaces);
  }
  g_variant_unref(objects);
  g_variant_unref(reply);
  return output.str();
}

struct Monitor {
  GDBusConnection* connection = nullptr;
  std::string previous;
};

void emit_if_changed(Monitor& monitor) {
  try {
    const auto current = snapshot(monitor.connection);
    if (current != monitor.previous) {
      std::cout << current << std::flush;
      monitor.previous = current;
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
}

void on_bluez_signal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                     GVariant*, gpointer data) {
  emit_if_changed(*static_cast<Monitor*>(data));
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--once")) {
    throw std::invalid_argument("usage: headunit-bluetooth-monitor [--once]");
  }
  GError* error = nullptr;
  GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (!connection) {
    const std::string detail = error ? error->message : "unknown D-Bus error";
    if (error) g_error_free(error);
    throw std::runtime_error("system bus unavailable: " + detail);
  }
  Monitor monitor{connection, {}};
  monitor.previous = snapshot(connection);
  std::cout << monitor.previous << std::flush;
  if (argc == 2) {
    g_object_unref(connection);
    return 0;
  }
  const guint properties_id = g_dbus_connection_signal_subscribe(
      connection, "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr,
      "org.bluez.Device1", G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_signal, &monitor, nullptr);
  const guint added_id = g_dbus_connection_signal_subscribe(
      connection, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", nullptr,
      nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_signal, &monitor, nullptr);
  const guint removed_id = g_dbus_connection_signal_subscribe(
      connection, "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", nullptr,
      nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_signal, &monitor, nullptr);
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  g_dbus_connection_signal_unsubscribe(connection, properties_id);
  g_dbus_connection_signal_unsubscribe(connection, added_id);
  g_dbus_connection_signal_unsubscribe(connection, removed_id);
  g_object_unref(connection);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
