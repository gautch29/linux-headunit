// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/iap2_session_engine.hpp"

#include <gio/gio.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <vector>
#include <string>
#include <string_view>

namespace headunit {

// BlueZ Profile1 owner for a single wireless iAP connection. The same engine
// remains available after RFCOMM closes so the IP receiver can attach it.
class BluetoothIapProfile {
 public:
  // Starts a nonblocking classic (BR/EDR) link to a BlueZ device path and
  // returns its descriptor, which becomes writable when the attempt ends;
  // or returns -1 with errno set. The default opens an L2CAP SDP channel.
  using PageOpener = std::function<int(std::string_view device_path)>;

  BluetoothIapProfile(MfiAuthenticator& chip, Iap2WifiConfig wifi,
                      std::optional<Iap2CarplayStart> carplay = std::nullopt,
                      Iap2Identity identity = {}, std::string selected_device = {},
                      PageOpener page = {},
                      std::chrono::milliseconds page_timeout = std::chrono::seconds(20),
                      std::chrono::milliseconds expected_window = std::chrono::seconds(90));
  ~BluetoothIapProfile();
  BluetoothIapProfile(const BluetoothIapProfile&) = delete;
  BluetoothIapProfile& operator=(const BluetoothIapProfile&) = delete;

  void start();
  void pump();
  // Wake the existing receiver poll for RFCOMM input; GLib remains its dispatcher.
  void append_poll_descriptors(std::vector<pollfd>& descriptors) const;
  std::shared_ptr<Iap2SessionEngine> engine() const { return engine_; }
  void release_session(const std::shared_ptr<Iap2SessionEngine>& engine);
  // iOS does not reopen iAP after a CarPlay session ends. While the receiver
  // is idle, open the selected phone's iAP service as its accessory, with
  // backoff. Asynchronous: never blocks the control loop.
  //
  // The phone is paged over classic Bluetooth first, and BlueZ is asked for
  // the iAP profile only once that link is up. Asking BlueZ directly makes a
  // page timeout fall back to a Low Energy connection on dual-mode phones,
  // which cannot carry iAP and keeps the device busy for about 40 seconds.
  //
  // For the first expected_window of an idle period (receiver start or the
  // end of a session) the phone is expected back, so an unanswered page is
  // retried after one second; each page already occupies the radio for the
  // controller's 5.12 s page timeout. After the window, back off as for an
  // absent phone. Once BlueZ reports the radio link up, iAP is requested
  // without waiting for the probe channel, which an iPhone can answer slowly.
  void maintain_connection(bool idle);

 private:
  static void connect_done(GObject*, GAsyncResult*, gpointer);
  static void method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
                          GVariant*, GDBusMethodInvocation*, gpointer);
  static gboolean transport_ready(gint, GIOCondition, gpointer);
  static gboolean retry(gpointer);
  static gboolean page_ready(gint, GIOCondition, gpointer);
  static gboolean page_expired(gpointer);
  static void device_changed(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                             const gchar*, GVariant*, gpointer);
  void on_link_up();
  void on_page_ready();
  void close_page();
  void request_profile();
  void connect_failed(const char* category, const char* reason,
                      std::optional<int> native_error = std::nullopt);
  void close_transport(bool preserve_handoff);
  bool queue_write(std::span<const std::uint8_t> bytes);
  bool flush_writes();
  void update_watch();
  void on_method(const gchar*, GVariant*, GDBusMethodInvocation*);
  gboolean on_transport(gint, GIOCondition);
  gboolean on_retry();
  std::array<std::uint8_t, 6> adapter_address() const;

  MfiAuthenticator& chip_;
  Iap2WifiHandoff handoff_;
  std::optional<Iap2CarplayStart> carplay_;
  GDBusConnection* bus_ = nullptr;
  GDBusNodeInfo* node_ = nullptr;
  guint object_id_ = 0;
  guint watch_ = 0;
  guint timer_ = 0;
  bool registered_ = false;
  int transport_ = -1;
  Iap2Identity identity_;
  std::string selected_device_;
  std::shared_ptr<Iap2SessionEngine> engine_;
  Iap2SessionEngine::Clock::time_point last_sync_{};
  unsigned sync_attempts_ = 0;
  std::uint64_t generation_ = 0;
  std::deque<std::vector<std::uint8_t>> pending_writes_;
  std::size_t pending_write_bytes_ = 0, write_offset_ = 0;
  std::optional<Iap2SessionEngine::Clock::time_point> write_started_;
  bool watch_output_ = false;
  guint client_object_id_ = 0;
  bool client_registered_ = false;
  GCancellable* connect_cancel_ = nullptr;
  bool connect_pending_ = false;
  Iap2SessionEngine::Clock::time_point connect_started_{};
  unsigned connect_failures_ = 0;
  bool initial_connection_ = true;
  std::optional<Iap2SessionEngine::Clock::time_point> idle_since_;
  Iap2SessionEngine::Clock::time_point next_connect_{};
  PageOpener page_;
  std::chrono::milliseconds page_timeout_;
  std::chrono::milliseconds expected_window_;
  bool profile_requested_ = false;
  guint device_signal_ = 0;
  int page_fd_ = -1;
  guint page_watch_ = 0;
  guint page_timer_ = 0;
};

Iap2WifiConfig load_private_iap_wifi_config(const std::string& path);

}  // namespace headunit
