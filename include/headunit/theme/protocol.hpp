// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace headunit::theme {
using Bytes = std::vector<std::uint8_t>;
inline constexpr std::size_t max_packet_bytes = 1024 * 1024;
inline constexpr std::size_t max_command_bytes = max_packet_bytes - 4096;

// Observed application-level envelope. The underlying iPhone transport is not
// established by this library; the replay framing is explicitly separate.
enum class MessageType : std::uint64_t { reset = 0, reset_complete = 1, command = 2, ack = 3 };
enum class CommandType : std::uint64_t {
  start_zone = 0x01,
  transition = 0x02,
  start_layout = 0x03,
  end_zone = 0x04,
  request_layout = 0x05,
  end_layout = 0x06,
  give_focus = 0x07,
  request_focus = 0x08,
  metadata = 0x09,
  palette = 0x10,
  allow_layout = 0x11,
  request_appearance = 0x12,
  appearance = 0x13,
  request_drive_mode = 0x14
};
struct Packet {
  MessageType type = MessageType::reset;
  std::uint64_t session = 0, sequence = 0, acknowledgement = 0;
  std::optional<std::uint64_t> version;
  std::optional<std::string> peer_id;
  Bytes data;  // Serialized nested plist, not a dictionary in the envelope.
  bool no_ack = false;
};
Bytes encode(const Packet& packet);
Packet decode(std::span<const std::uint8_t> bytes);

enum class State { stopped, awaiting_reset, ready };
enum class Result { ignored, reset_started, ready, command, acknowledgement, malformed };
struct Received {
  Result result = Result::ignored;
  Bytes command;
};

// One instance per display. All methods run on one owning event-loop thread.
// A transport must call sent() only after accepting the complete pending frame.
class Session {
 public:
  explicit Session(std::optional<std::string> peer_id = std::nullopt, std::size_t queue_limit = 128,
                   std::uint64_t timeout_ms = 1000);
  void start();
  void stop();
  bool send_command(Bytes payload);
  const Bytes* pending();
  void sent(std::uint64_t now_ms);
  Received receive(std::span<const std::uint8_t> bytes);
  bool tick(std::uint64_t now_ms);
  State state() const { return state_; }
  std::uint64_t session_number() const { return session_; }
  std::uint64_t version() const { return version_; }
  std::size_t queued() const { return queue_.size(); }

 private:
  struct Outgoing {
    MessageType type;
    Bytes payload;
  };
  void reset();
  bool matching_peer(const Packet& packet) const;
  void enqueue_ack();
  State state_ = State::stopped;
  std::optional<std::string> peer_id_;
  std::uint64_t session_ = 0, next_tx_ = 1, last_rx_ = 0, last_ack_ = 0, version_ = 0;
  std::uint64_t ack_required_ = 0, pending_ack_ = 0;
  std::size_t queue_limit_, queued_bytes_ = 0;
  std::uint64_t timeout_ms_;
  std::optional<std::uint64_t> reset_sent_at_;
  std::deque<Outgoing> queue_;
  Bytes pending_;
};

struct TransitionCommand {
  CommandType type;
  Bytes encoded;
  std::optional<double> pts_ms;
  std::optional<bool> transitioning;
};
TransitionCommand decode_transition(std::span<const std::uint8_t> payload);

// Commands leave this queue only within the corresponding video presentation
// window. No wall-clock substitution: missing video timing freezes dispatch.
class Timeline {
 public:
  explicit Timeline(std::size_t limit = 512) : limit_(limit) {}
  bool push(Bytes command);
  std::vector<TransitionCommand> frame(double video_pts_seconds, double frame_seconds);
  void reset();
  std::size_t queued() const { return queue_.size(); }
  bool transitioning() const { return transitioning_; }

 private:
  std::size_t limit_, queued_bytes_ = 0;
  bool transitioning_ = false;
  std::deque<TransitionCommand> queue_;
};
}  // namespace headunit::theme
