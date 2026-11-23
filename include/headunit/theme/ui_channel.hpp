// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/opack.hpp>

namespace headunit::theme::ui {
// One cluster-control channel multiplexes the configured display identifiers.
// Its direct OPACK payload wraps the binary-plist Session message under data.
inline constexpr std::size_t max_payload_bytes = max_packet_bytes - 1024;
struct Message {
  std::string display_uuid;  // Opaque exact identifier from display configuration.
  Bytes data;
  bool operator==(const Message&) const = default;
};
Bytes encode(const Message&);
Message decode(std::span<const std::uint8_t>);
struct Display {
  std::string identifier;
  std::optional<std::string> peer_id;
};
struct Routed {
  std::optional<std::string> display_uuid;
  Received session;
};

// Only locally configured displays have sessions. Unknown remote IDs cannot
// allocate state. Call start/stop with channel lifecycle, tick on a monotonic
// timer, receive for complete channel payloads and sent after transport enqueue.
// Receiving a reset or a timeout can invalidate an earlier pending pointer;
// reacquire pending() before sending. A single event-loop thread owns this class.
class Channel {
 public:
  explicit Channel(std::vector<Display>, std::size_t queue_limit = 128,
                   std::uint64_t reset_timeout_ms = 1000);
  void start();
  void stop();
  Routed receive(std::span<const std::uint8_t>);
  bool send_command(std::string_view display, Bytes);
  const Bytes* pending();  // Round-robin among displays; stable until sent/reset.
  void sent(std::uint64_t now_ms);
  bool tick(std::uint64_t now_ms);
  std::optional<State> state(std::string_view display) const;

 private:
  struct Entry {
    std::string identifier;
    Session session;
  };
  std::vector<Entry> displays_;
  std::size_t next_ = 0;
  std::optional<std::size_t> selected_;
  Bytes pending_;
  void invalidate(std::size_t index);
};
}  // namespace headunit::theme::ui
