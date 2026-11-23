// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_event_channel.hpp"
#include "headunit/cluster_input.hpp"
#include "headunit/control_request.hpp"

#include <plist/plist.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <limits>

namespace headunit {
namespace {
constexpr std::size_t header_limit = 8192;
constexpr std::size_t message_limit = 65536;

std::vector<std::uint8_t> encode_plist(plist_t root) {
  char* bytes = nullptr;
  std::uint32_t length = 0;
  const auto status = plist_to_bin(root, &bytes, &length);
  const std::unique_ptr<char, decltype(&plist_mem_free)> owned(bytes, &plist_mem_free);
  if (status != PLIST_ERR_SUCCESS || !owned) {
    throw std::runtime_error("cannot encode event command plist");
  }
  return {owned.get(), owned.get() + length};
}
}  // namespace

AirplayEventChannel::AirplayEventChannel(std::span<const std::uint8_t> pair_secret)
    : framing_(pair_secret, HapFrameChannel::events) {}

std::vector<AirplayEventMessage> AirplayEventChannel::receive(
    std::span<const std::uint8_t> ciphertext) {
  const auto plain = framing_.receive(ciphertext);
  std::vector<AirplayEventMessage> messages;
  for (std::size_t offset = 0; offset < plain.size();) {
    const auto count = std::min(message_limit - pending_.size(), plain.size() - offset);
    if (count == 0) throw std::invalid_argument("event channel buffer exceeded");
    pending_.append(reinterpret_cast<const char*>(plain.data() + offset), count);
    offset += count;
    // Drain complete messages before charging the successor against the
    // incomplete-message budget. TCP reads need not end at message boundaries.
    parse_messages(messages);
  }
  if (plain.empty()) parse_messages(messages);
  return messages;
}

void AirplayEventChannel::parse_messages(std::vector<AirplayEventMessage>& messages) {
  for (;;) {
    const auto end = pending_.find("\r\n\r\n");
    if (end == std::string::npos) {
      if (pending_.size() > header_limit) {
        throw std::invalid_argument("event header exceeded limit");
      }
      break;
    }
    if (end > header_limit) throw std::invalid_argument("event header too large");
    const auto first_end = pending_.find("\r\n");
    const auto first = std::string_view(pending_).substr(0, first_end);
    if (!valid_control_start_line(first)) throw std::invalid_argument("invalid event start line");
    AirplayEventMessage message;
    std::istringstream request{std::string(first)};
    if (!(request >> message.method >> message.path >> message.protocol)) {
      throw std::invalid_argument("invalid event request line");
    }
    message.response = message.method == "RTSP/1.0" || message.method == "HTTP/1.1";
    if (message.response) {
      unsigned status = 0;
      const auto [last, error] =
          std::from_chars(message.path.data(), message.path.data() + message.path.size(), status);
      if (error != std::errc{} || last != message.path.data() + message.path.size() ||
          message.path.size() != 3 || status < 100 || status > 599)
        throw std::invalid_argument("invalid event response status");
    } else {
      std::string extra;
      if (!valid_control_request_line(message.method, message.path, message.protocol) ||
          request >> extra)
        throw std::invalid_argument("invalid event request protocol");
    }
    const auto headers = parse_control_headers(
        first_end == end ? std::string_view{}
                         : std::string_view(pending_).substr(first_end + 2, end - first_end - 2),
        message_limit - end - 4);
    if (!headers) throw std::invalid_argument("invalid event headers");
    const auto content_length = headers->body_bytes;
    message.cseq = headers->cseq;
    const auto length = end + 4 + content_length;
    if (pending_.size() < length) break;
    const auto* data = reinterpret_cast<const std::uint8_t*>(pending_.data() + end + 4);
    message.body.assign(data, data + content_length);
    pending_.erase(0, length);
    if (message.response) accept_response(message, Clock::now());
    messages.push_back(std::move(message));
  }
}

std::vector<std::uint8_t> AirplayEventChannel::reply(const AirplayEventMessage& message,
                                                     unsigned status) {
  std::uint32_t sequence = 0;
  const auto [last, error] =
      std::from_chars(message.cseq.data(), message.cseq.data() + message.cseq.size(), sequence);
  const bool valid_cseq =
      message.cseq.empty() || (message.cseq.size() <= 10 && error == std::errc{} &&
                               last == message.cseq.data() + message.cseq.size());
  if (message.response || !valid_cseq || (status != 200 && status != 400 && status != 501)) {
    throw std::invalid_argument("invalid event response request");
  }
  std::string plain = "RTSP/1.0 " + std::to_string(status) +
                      (status == 200   ? " OK\r\n"
                       : status == 400 ? " Bad Request\r\n"
                                       : " Not Implemented\r\n");
  if (!message.cseq.empty()) plain += "CSeq: " + message.cseq + "\r\n";
  plain += "Content-Length: 0\r\n\r\n";
  return framing_.encode({reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()});
}

std::vector<std::uint8_t> AirplayEventChannel::send_iap_message(
    std::span<const std::uint8_t> message, std::size_t available_wire_bytes) {
  if (message.empty() || message.size() > 60000) {
    throw std::invalid_argument("invalid outbound iAP message size");
  }
  plist_t root = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("iAPSendMessage"));
  plist_t params = plist_new_dict();
  plist_dict_set_item(
      params, "data",
      plist_new_data(reinterpret_cast<const char*>(message.data()), message.size()));
  plist_dict_set_item(root, "params", params);
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::iap, available_wire_bytes);
}

std::vector<std::uint8_t> AirplayEventChannel::show_ui(const std::string& display_uuid,
                                                       const std::string& url) {
  if (display_uuid.size() != 36 || url.empty() || url.size() > 1024 ||
      url.find('\0') != std::string::npos) {
    throw std::invalid_argument("invalid showUI target");
  }
  for (std::size_t index = 0; index < display_uuid.size(); ++index) {
    const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
    if (separator ? display_uuid[index] != '-'
                  : !std::isxdigit(static_cast<unsigned char>(display_uuid[index]))) {
      throw std::invalid_argument("invalid showUI display UUID");
    }
  }
  plist_t root = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("showUI"));
  plist_t params = plist_new_dict();
  plist_dict_set_item(params, "uuid", plist_new_string(display_uuid.c_str()));
  plist_dict_set_item(params, "url", plist_new_string(url.c_str()));
  plist_dict_set_item(root, "params", params);
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::show_ui);
}

std::vector<std::uint8_t> AirplayEventChannel::send_touch(const TouchInput& touch) {
  // One finger record per declared contact: transducer index, tip, X, Y.
  std::vector<std::uint8_t> report{1,
                                   static_cast<std::uint8_t>(touch.pressed ? 1 : 0),
                                   static_cast<std::uint8_t>(touch.x),
                                   static_cast<std::uint8_t>(touch.x >> 8),
                                   static_cast<std::uint8_t>(touch.y),
                                   static_cast<std::uint8_t>(touch.y >> 8)};
  if (multitouch_enabled()) {
    report.insert(report.end(), {2, static_cast<std::uint8_t>(touch.second_pressed ? 1 : 0),
                                 static_cast<std::uint8_t>(touch.second_x),
                                 static_cast<std::uint8_t>(touch.second_x >> 8),
                                 static_cast<std::uint8_t>(touch.second_y),
                                 static_cast<std::uint8_t>(touch.second_y >> 8)});
  }
  plist_t root = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("hidSendReport"));
  plist_dict_set_item(root, "uuid", plist_new_string(touchscreen_hid_uuid));
  plist_dict_set_item(root, "hidReport",
                      plist_new_data(reinterpret_cast<const char*>(report.data()), report.size()));
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::touch);
}

std::vector<std::uint8_t> AirplayEventChannel::send_cluster_button(std::uint8_t report) {
  if (report && report != 1 && report != 2 && report != 4 && report != 8 && report != 16 &&
      report != 32)
    throw std::invalid_argument("invalid cluster button report");
  plist_t root = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("hidSendReport"));
  plist_dict_set_item(root, "uuid", plist_new_string(cluster_hid_uuid));
  plist_dict_set_item(root, "hidReport", plist_new_data(reinterpret_cast<const char*>(&report), 1));
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::touch);
}
std::vector<std::uint8_t> AirplayEventChannel::send_media_button(std::uint8_t report) {
  if (report && report != 1 && report != 2 && report != 4)
    throw std::invalid_argument("invalid media button report");
  plist_t root = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("hidSendReport"));
  plist_dict_set_item(root, "uuid", plist_new_string(media_hid_uuid));
  plist_dict_set_item(root, "hidReport", plist_new_data(reinterpret_cast<const char*>(&report), 1));
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::touch);
}
std::vector<std::uint8_t> AirplayEventChannel::force_key_frame(const std::string& uuid) {
  if (uuid.size() != 36) throw std::invalid_argument("invalid video refresh UUID");
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
    if (separator ? uuid[i] != '-' : !std::isxdigit(static_cast<unsigned char>(uuid[i])))
      throw std::invalid_argument("invalid video refresh UUID");
  }
  plist_t root = plist_new_dict(), params = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("forceKeyFrame"));
  plist_dict_set_item(params, "uuid", plist_new_string(uuid.c_str()));
  plist_dict_set_item(root, "params", params);
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::refresh);
}

std::vector<std::uint8_t> AirplayEventChannel::set_limited_ui(bool limited) {
  plist_t root = plist_new_dict(), params = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("setLimitedUI"));
  plist_dict_set_item(params, "limitedUI", plist_new_bool(limited ? 1 : 0));
  plist_dict_set_item(root, "params", params);
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::limited_ui);
}

std::vector<std::uint8_t> AirplayEventChannel::request_ui(const std::string& url) {
  if (url.size() > 256 || std::any_of(url.begin(), url.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7e;
      }))
    throw std::invalid_argument("invalid requestUI URL");
  plist_t root = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("requestUI"));
  if (!url.empty()) {
    plist_t params = plist_new_dict();
    plist_dict_set_item(params, "url", plist_new_string(url.c_str()));
    plist_dict_set_item(root, "params", params);
  }
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::request_ui);
}

std::vector<std::uint8_t> AirplayEventChannel::set_night_mode(bool night) {
  plist_t root = plist_new_dict(), params = plist_new_dict();
  plist_dict_set_item(root, "type", plist_new_string("setNightMode"));
  plist_dict_set_item(params, "nightMode", plist_new_bool(night ? 1 : 0));
  plist_dict_set_item(root, "params", params);
  std::vector<std::uint8_t> body;
  try {
    body = encode_plist(root);
  } catch (...) {
    plist_free(root);
    throw;
  }
  plist_free(root);
  return send_command_body(body, CommandKind::night_mode);
}

std::vector<std::uint8_t> AirplayEventChannel::send_command_body(std::span<const std::uint8_t> body,
                                                                 CommandKind kind,
                                                                 std::size_t available_wire_bytes) {
  if (next_cseq_ == std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("event CSeq exhausted");
  const auto sequence = next_cseq_ + 1;
  const std::string header =
      "POST /command RTSP/1.0\r\nContent-Type: application/x-apple-binary-plist\r\n"
      "Content-Length: " +
      std::to_string(body.size()) + "\r\nCSeq: " + std::to_string(sequence) + "\r\n\r\n";
  std::vector<std::uint8_t> plain(header.begin(), header.end());
  plain.insert(plain.end(), body.begin(), body.end());
  if (plain.size() > message_limit) {
    throw std::invalid_argument("outbound event exceeds message limit");
  }
  if (HapControlFrame::encoded_size(plain.size()) > available_wire_bytes) return {};
  auto bytes = framing_.encode(plain);
  const auto now = Clock::now();
  expire(now);
  auto& slot = commands_[sequence % commands_.size()];
  if (slot) complete(*slot, Outcome::evicted);
  slot = PendingCommand{sequence, kind, now + std::chrono::seconds(5)};
  next_cseq_ = sequence;
  return bytes;
}

void AirplayEventChannel::complete(const PendingCommand& command, Outcome outcome,
                                   std::optional<unsigned> status) {
  switch (outcome) {
    case Outcome::accepted:
      ++diagnostics_.accepted;
      break;
    case Outcome::rejected:
      ++diagnostics_.rejected;
      break;
    case Outcome::timed_out:
      ++diagnostics_.timed_out;
      break;
    case Outcome::evicted:
      ++diagnostics_.evicted;
      break;
  }
  if (completions_.size() == commands_.size()) {
    completions_.pop_front();
    ++diagnostics_.dropped_completions;
  }
  completions_.push_back({command.cseq, command.command, outcome, status});
}

void AirplayEventChannel::expire(Clock::time_point now) {
  for (auto& slot : commands_)
    if (slot && now >= slot->deadline) {
      complete(*slot, Outcome::timed_out);
      slot.reset();
    }
}

void AirplayEventChannel::accept_response(const AirplayEventMessage& response,
                                          Clock::time_point now) {
  expire(now);
  std::uint32_t sequence = 0;
  const auto [last, error] =
      std::from_chars(response.cseq.data(), response.cseq.data() + response.cseq.size(), sequence);
  auto& slot = commands_[sequence % commands_.size()];
  if (error != std::errc{} || last != response.cseq.data() + response.cseq.size() || !slot ||
      slot->cseq != sequence) {
    ++diagnostics_.unmatched;
    return;
  }
  unsigned status = 0;
  (void)std::from_chars(response.path.data(), response.path.data() + response.path.size(), status);
  if (status < 200) return;
  complete(*slot, status < 300 ? Outcome::accepted : Outcome::rejected, status);
  slot.reset();
}

std::vector<AirplayEventChannel::Completion> AirplayEventChannel::take_completions(
    Clock::time_point now) {
  expire(now);
  std::vector<Completion> result(completions_.begin(), completions_.end());
  completions_.clear();
  return result;
}

AirplayEventChannel::Diagnostics AirplayEventChannel::diagnostics() const {
  auto result = diagnostics_;
  result.pending = std::count_if(commands_.begin(), commands_.end(),
                                 [](const auto& command) { return command.has_value(); });
  return result;
}

}  // namespace headunit
