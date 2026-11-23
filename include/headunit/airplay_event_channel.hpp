// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/hap_control_frame.hpp"
#include "headunit/touch_input.hpp"

#include <cstdint>
#include <cstddef>
#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace headunit {

struct AirplayEventMessage {
  std::string method;
  std::string path;
  std::string protocol;
  std::string cseq;
  std::vector<std::uint8_t> body;
  bool response = false;
};

class AirplayEventChannel {
 public:
  using Clock = std::chrono::steady_clock;
  enum class CommandKind { iap, show_ui, touch, refresh, night_mode, request_ui, limited_ui };
  enum class Outcome { accepted, rejected, timed_out, evicted };
  struct Completion {
    std::uint32_t cseq;
    CommandKind command;
    Outcome outcome;
    std::optional<unsigned> status;
  };
  struct Diagnostics {
    std::uint64_t accepted = 0, rejected = 0, timed_out = 0, evicted = 0;
    std::uint64_t unmatched = 0, dropped_completions = 0;
    std::size_t pending = 0;
  };
  explicit AirplayEventChannel(std::span<const std::uint8_t> pair_secret);
  std::vector<AirplayEventMessage> receive(std::span<const std::uint8_t> ciphertext);
  std::vector<std::uint8_t> reply(const AirplayEventMessage& message, unsigned status);
  // Capacity rejection returns no bytes and consumes neither CSeq nor nonces.
  std::vector<std::uint8_t> send_iap_message(std::span<const std::uint8_t> message,
                                             std::size_t available_wire_bytes = 65536);
  std::vector<std::uint8_t> show_ui(const std::string& display_uuid, const std::string& url);
  std::vector<std::uint8_t> send_touch(const TouchInput& touch);
  std::vector<std::uint8_t> send_cluster_button(std::uint8_t report);
  std::vector<std::uint8_t> send_media_button(std::uint8_t report);
  std::vector<std::uint8_t> force_key_frame(const std::string& display_uuid);
  // setNightMode for the phone's main display (the phone's default target).
  std::vector<std::uint8_t> set_night_mode(bool night);
  // setLimitedUI for the phone's main display: driving restrictions on/off.
  std::vector<std::uint8_t> set_limited_ui(bool limited);
  // requestUI: ask the phone to show CarPlay on the main screen, optionally
  // opening a URL (for example "maps:"). Bounded printable ASCII only.
  std::vector<std::uint8_t> request_ui(const std::string& url = {});
  // A successful response confirms only event transport acceptance. It does
  // not confirm presentation, touch consumption or an applied vehicle state.
  std::vector<Completion> take_completions(Clock::time_point now = Clock::now());
  Diagnostics diagnostics() const;

 private:
  HapControlFrame framing_;
  std::string pending_;
  std::uint32_t next_cseq_ = 0;
  struct PendingCommand {
    std::uint32_t cseq;
    CommandKind command;
    Clock::time_point deadline;
  };
  std::array<std::optional<PendingCommand>, 64> commands_;
  std::deque<Completion> completions_;
  Diagnostics diagnostics_;
  void parse_messages(std::vector<AirplayEventMessage>&);
  void complete(const PendingCommand&, Outcome, std::optional<unsigned> = {});
  void expire(Clock::time_point);
  void accept_response(const AirplayEventMessage&, Clock::time_point);
  std::vector<std::uint8_t> send_command_body(std::span<const std::uint8_t> body, CommandKind,
                                              std::size_t available_wire_bytes = 65536 + 64 * 18);
};

}  // namespace headunit
