// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_identity.hpp"

#include <gio/gio.h>
#include <glib-unix.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <net/if.h>
#include <stdexcept>
#include <string>

namespace {
constexpr char avahi_name[] = "org.freedesktop.Avahi";
constexpr char avahi_server[] = "org.freedesktop.Avahi.Server";
constexpr char avahi_group[] = "org.freedesktop.Avahi.EntryGroup";

void check_reply(GVariant* reply, GError* error, const char* operation) {
  if (reply) {
    g_variant_unref(reply);
    return;
  }
  const std::string detail = error ? error->message : "unknown D-Bus error";
  if (error) g_error_free(error);
  throw std::runtime_error(std::string(operation) + ": " + detail);
}

unsigned parse_seconds(const char* value) {
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoul(value, &end, 10);
  if (errno || !end || *end != '\0' || parsed > 3600) {
    throw std::invalid_argument("duration must be 0..3600 seconds (0 runs until stopped)");
  }
  return static_cast<unsigned>(parsed);
}

gboolean end_after_timeout(gpointer loop) {
  g_main_loop_quit(static_cast<GMainLoop*>(loop));
  return G_SOURCE_REMOVE;
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc != 4) {
    throw std::invalid_argument(
        "usage: headunit-airplay-advertiser SECONDS IDENTITY_FILE INTERFACE");
  }
  const auto seconds = parse_seconds(argv[1]);
  const auto identity = headunit::load_airplay_identity(argv[2]);
  const auto interface_index = if_nametoindex(argv[3]);
  if (!interface_index) throw std::invalid_argument("network interface unavailable");

  GError* error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (!bus) {
    const std::string detail = error ? error->message : "system bus unavailable";
    if (error) g_error_free(error);
    throw std::runtime_error(detail);
  }
  GVariant* result = g_dbus_connection_call_sync(bus, avahi_name, "/", avahi_server,
                                                 "EntryGroupNew", nullptr, G_VARIANT_TYPE("(o)"),
                                                 G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  if (!result) {
    const std::string detail = error ? error->message : "Avahi unavailable";
    if (error) g_error_free(error);
    g_object_unref(bus);
    throw std::runtime_error(detail);
  }
  const char* group_name = nullptr;
  g_variant_get(result, "(&o)", &group_name);
  const std::string group_path(group_name);
  g_variant_unref(result);

  GVariantBuilder records;
  g_variant_builder_init(&records, G_VARIANT_TYPE("aay"));
  const std::array<std::string, 8> records_text{{
      "deviceid=" + identity.device_id,
      "features=" + identity.features_text,
      "flags=" + identity.flags_text,
      "model=" + identity.model,
      "protovers=" + identity.protocol_version,
      "srcvers=" + identity.source_version,
      "pi=" + identity.pairing_id,
      "pk=" + identity.public_key_hex,
  }};
  for (const auto& record : records_text) {
    g_variant_builder_add_value(
        &records, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, record.data(), record.size(), 1));
  }
  result = g_dbus_connection_call_sync(
      bus, avahi_name, group_path.c_str(), avahi_group, "AddService",
      g_variant_new("(iiussssq@aay)", static_cast<gint32>(interface_index), static_cast<gint32>(-1),
                    static_cast<guint32>(0), identity.name.c_str(), "_airplay._tcp", "", "",
                    static_cast<guint16>(7000), g_variant_builder_end(&records)),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  check_reply(result, error, "Avahi AddService failed");
  error = nullptr;
  result =
      g_dbus_connection_call_sync(bus, avahi_name, group_path.c_str(), avahi_group, "Commit",
                                  nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  check_reply(result, error, "Avahi Commit failed");

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_unix_signal_add(SIGINT, end_after_timeout, loop);
  g_unix_signal_add(SIGTERM, end_after_timeout, loop);
  if (seconds) g_timeout_add_seconds(seconds, end_after_timeout, loop);
  std::cout << "AirPlay discovery advertised on " << argv[3] << " port 7000\n" << std::flush;
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  error = nullptr;
  result =
      g_dbus_connection_call_sync(bus, avahi_name, group_path.c_str(), avahi_group, "Free", nullptr,
                                  nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  check_reply(result, error, "Avahi Free failed");
  g_object_unref(bus);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
