// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/protocol.hpp>
#include <plist/plist.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace headunit::theme {
namespace {
struct FreePlist {
  void operator()(void* p) const { plist_free(p); }
};
using Tree = std::unique_ptr<void, FreePlist>;
struct FreeMemory {
  void operator()(void* p) const { plist_mem_free(p); }
};
using Memory = std::unique_ptr<char, FreeMemory>;
[[noreturn]] void invalid(const char* message) {
  throw std::invalid_argument(message);
}
Tree parse(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 40 || bytes.size() > max_packet_bytes ||
      std::memcmp(bytes.data(), "bplist00", 8) != 0)
    invalid("invalid binary plist size/header");
  plist_t raw = nullptr;
  const auto status = plist_from_bin(reinterpret_cast<const char*>(bytes.data()),
                                     static_cast<std::uint32_t>(bytes.size()), &raw);
  Tree tree(raw);
  if (status != PLIST_ERR_SUCCESS || !raw || plist_get_node_type(raw) != PLIST_DICT)
    invalid("binary plist root must be a dictionary");
  return tree;
}
Bytes serialize(plist_t root) {
  char* raw = nullptr;
  std::uint32_t size = 0;
  const auto status = plist_to_bin(root, &raw, &size);
  Memory memory(raw);
  if (status != PLIST_ERR_SUCCESS || !raw || size > max_packet_bytes)
    invalid("binary plist serialization failed or exceeded packet limit");
  return Bytes(raw, raw + size);
}
plist_t item(plist_t root, const char* key) {
  return plist_dict_get_item(root, key);
}
std::uint64_t number(plist_t root, const char* key) {
  auto node = item(root, key);
  if (!node || plist_get_node_type(node) != PLIST_INT) invalid("missing or non-integer field");
  std::uint64_t value = 0;
  plist_get_uint_val(node, &value);
  // The observed Lua side represents these sequence values exactly as doubles.
  if (value > 9007199254740991ULL) invalid("integer outside exact protocol range");
  return value;
}
bool boolean(plist_t root, const char* key) {
  auto node = item(root, key);
  if (!node || plist_get_node_type(node) != PLIST_BOOLEAN) invalid("missing or non-boolean field");
  std::uint8_t value = 0;
  plist_get_bool_val(node, &value);
  return value != 0;
}
bool valid_payload(std::span<const std::uint8_t> payload) {
  try {
    auto tree = parse(payload);
    return static_cast<bool>(tree);
  } catch (const std::invalid_argument&) {
    return false;
  }
}
bool is_transition_type(std::uint64_t type) {
  return type == 1 || type == 2 || type == 3 || type == 4 || type == 6 || type == 9;
}
}  // namespace

Bytes encode(const Packet& p) {
  Tree root(plist_new_dict());
  auto set = [&](const char* key, std::uint64_t value) {
    if (value > 9007199254740991ULL) invalid("integer outside exact protocol range");
    plist_dict_set_item(root.get(), key, plist_new_uint(value));
  };
  if (static_cast<std::uint64_t>(p.type) > 3) invalid("unknown envelope type");
  set("type", static_cast<std::uint64_t>(p.type));
  set("ssn", p.session);
  set("psn", p.sequence);
  set("asn", p.acknowledgement);
  if (p.version) set("version", *p.version);
  if (p.peer_id) {
    if (p.peer_id->size() > 256 || p.peer_id->find('\0') != std::string::npos)
      invalid("invalid peer identifier");
    plist_dict_set_item(root.get(), "id", plist_new_string(p.peer_id->c_str()));
  }
  if (!p.data.empty()) {
    if (!valid_payload(p.data)) invalid("command data must be a serialized dictionary");
    plist_dict_set_item(
        root.get(), "data",
        plist_new_data(reinterpret_cast<const char*>(p.data.data()), p.data.size()));
  }
  if (p.no_ack) plist_dict_set_item(root.get(), "noAck", plist_new_bool(1));
  return serialize(root.get());
}

Packet decode(std::span<const std::uint8_t> bytes) {
  auto root = parse(bytes);
  Packet p;
  const auto type = number(root.get(), "type");
  if (type > 3) invalid("unknown envelope type");
  p.type = static_cast<MessageType>(type);
  p.session = number(root.get(), "ssn");
  p.sequence = number(root.get(), "psn");
  p.acknowledgement = number(root.get(), "asn");
  if (!p.session || !p.sequence) invalid("zero session or sequence");
  if (item(root.get(), "version")) {
    p.version = number(root.get(), "version");
    if (*p.version < 1) invalid("invalid protocol version");
  }
  if (auto node = item(root.get(), "id")) {
    if (plist_get_node_type(node) != PLIST_STRING) invalid("invalid identifier type");
    std::uint64_t length = 0;
    const char* str = plist_get_string_ptr(node, &length);
    if (!str || length > 256 || std::memchr(str, '\0', length))
      invalid("invalid identifier length");
    p.peer_id = std::string(str, length);
  }
  if (auto node = item(root.get(), "data")) {
    if (plist_get_node_type(node) != PLIST_DATA) invalid("nested data must be binary");
    std::uint64_t size = 0;
    const char* data = plist_get_data_ptr(node, &size);
    if (!data || size > max_packet_bytes) invalid("invalid nested data size");
    p.data.assign(data, data + size);
    if (!valid_payload(p.data)) invalid("invalid nested dictionary");
  }
  if (item(root.get(), "noAck")) p.no_ack = boolean(root.get(), "noAck");
  if (p.type == MessageType::command && p.data.empty()) invalid("command has no payload");
  return p;
}

Session::Session(std::optional<std::string> peer, std::size_t limit, std::uint64_t timeout)
    : peer_id_(std::move(peer)), queue_limit_(limit), timeout_ms_(timeout) {
  if (limit < 2 || limit > 4096 || timeout == 0) invalid("invalid session limits");
  if (peer_id_ && (peer_id_->size() > 256 || peer_id_->find('\0') != std::string::npos))
    invalid("invalid peer identifier");
}
void Session::start() {
  reset();
}
void Session::stop() {
  state_ = State::stopped;
  queue_.clear();
  pending_.clear();
  queued_bytes_ = 0;
  reset_sent_at_.reset();
  version_ = 0;
}
void Session::reset() {
  if (session_ >= 9007199254740991ULL) throw std::overflow_error("session sequence exhausted");
  ++session_;
  next_tx_ = 1;
  last_rx_ = 0;
  last_ack_ = 0;
  version_ = 0;
  ack_required_ = 0;
  pending_ack_ = 0;
  state_ = State::awaiting_reset;
  queue_.clear();
  pending_.clear();
  queued_bytes_ = 0;
  queue_.push_back({MessageType::reset, {}});
  reset_sent_at_.reset();
}
bool Session::matching_peer(const Packet& p) const {
  return p.version.value_or(1) < 2 || !peer_id_ || p.peer_id == peer_id_;
}
bool Session::send_command(Bytes payload) {
  if (state_ != State::ready || queue_.size() >= queue_limit_ ||
      payload.size() > max_command_bytes || payload.size() + queued_bytes_ > 2 * max_packet_bytes ||
      !valid_payload(payload))
    return false;
  // Do not replace a frame already handed to the transport by pending().
  if (pending_.empty() && queue_.size() == 1 && queue_.front().type == MessageType::ack)
    queue_.pop_front();
  queued_bytes_ += payload.size();
  queue_.push_back({MessageType::command, std::move(payload)});
  return true;
}
const Bytes* Session::pending() {
  if (!pending_.empty()) return &pending_;
  if (queue_.empty()) return nullptr;
  const auto& q = queue_.front();
  Packet p;
  p.type = q.type;
  p.session = session_;
  p.sequence = next_tx_;
  p.acknowledgement = last_rx_;
  p.data = q.payload;
  if (q.type == MessageType::reset) {
    p.version = 2;
    p.peer_id = peer_id_;
  }
  if (q.type == MessageType::reset_complete) {
    p.version = version_;
    if (version_ >= 2) p.peer_id = peer_id_;
  }
  pending_ = encode(p);
  pending_ack_ = p.acknowledgement;
  return &pending_;
}
void Session::sent(std::uint64_t now_ms) {
  if (pending_.empty() || queue_.empty()) throw std::logic_error("no pending packet");
  if (queue_.front().type == MessageType::reset) reset_sent_at_ = now_ms;
  queued_bytes_ -= queue_.front().payload.size();
  queue_.pop_front();
  pending_.clear();
  if (next_tx_ >= 9007199254740991ULL) {
    reset();
    return;
  }
  ++next_tx_;
  if (state_ == State::ready && ack_required_ > pending_ack_) enqueue_ack();
}
void Session::enqueue_ack() {
  if (queue_.empty()) queue_.push_back({MessageType::ack, {}});
}
Received Session::receive(std::span<const std::uint8_t> bytes) {
  Packet p;
  try {
    p = decode(bytes);
  } catch (const std::invalid_argument&) {
    return {Result::malformed, {}};
  }
  if (state_ == State::stopped) return {};
  if (p.type == MessageType::reset) {
    if (!matching_peer(p)) return {};
    if (state_ == State::awaiting_reset && next_tx_ == 2) return {};
    reset();
    return {Result::reset_started, {}};
  }
  if (p.session != session_) return {};
  if (p.type == MessageType::reset_complete) {
    if (state_ != State::awaiting_reset || next_tx_ < 2 || !matching_peer(p) ||
        p.acknowledgement >= next_tx_)
      return {};
    version_ = std::min<std::uint64_t>(2, p.version.value_or(1));
    peer_id_ = p.peer_id;
    last_rx_ = p.sequence;
    last_ack_ = p.acknowledgement;
    state_ = State::ready;
    reset_sent_at_.reset();
    queue_.push_back({MessageType::reset_complete, {}});
    return {Result::ready, {}};
  }
  if (state_ != State::ready || p.sequence <= last_rx_) return {};
  if (p.sequence != last_rx_ + 1) {
    reset();
    return {Result::reset_started, {}};
  }
  if (p.acknowledgement < last_ack_ || p.acknowledgement >= next_tx_) return {};
  last_rx_ = p.sequence;
  last_ack_ = p.acknowledgement;
  if (p.type == MessageType::command) {
    if (!p.no_ack) {
      ack_required_ = p.sequence;
      enqueue_ack();
    }
    return {Result::command, std::move(p.data)};
  }
  return {Result::acknowledgement, {}};
}
bool Session::tick(std::uint64_t now_ms) {
  if (state_ == State::awaiting_reset && reset_sent_at_ && now_ms >= *reset_sent_at_ &&
      now_ms - *reset_sent_at_ >= timeout_ms_) {
    reset();
    return true;
  }
  return false;
}

TransitionCommand decode_transition(std::span<const std::uint8_t> bytes) {
  auto root = parse(bytes);
  const auto type = number(root.get(), "type");
  if (!is_transition_type(type)) invalid("command is outside the transition timeline");
  TransitionCommand command{
      static_cast<CommandType>(type), Bytes(bytes.begin(), bytes.end()), {}, {}};
  auto data = item(root.get(), "data");
  if (!data || plist_get_node_type(data) != PLIST_DICT)
    invalid("transition data must be a dictionary");
  if (command.type == CommandType::metadata)
    command.transitioning = boolean(data, "isTransitioning");
  if (command.type == CommandType::transition) {
    auto node = item(data, "pts");
    double value = 0;
    if (node && plist_get_node_type(node) == PLIST_REAL)
      plist_get_real_val(node, &value);
    else
      value = static_cast<double>(number(data, "pts"));
    if (!std::isfinite(value) || value < 0 || value > 9007199254740991.0)
      invalid("invalid presentation timestamp");
    command.pts_ms = value;
  }
  return command;
}
bool Timeline::push(Bytes bytes) {
  if (queue_.size() >= limit_ || queued_bytes_ + bytes.size() > 2 * max_packet_bytes) return false;
  try {
    auto command = decode_transition(bytes);
    queued_bytes_ += command.encoded.size();
    queue_.push_back(std::move(command));
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}
std::vector<TransitionCommand> Timeline::frame(double pts, double duration) {
  std::vector<TransitionCommand> due;
  if (!std::isfinite(pts) || pts < 0 || !std::isfinite(duration) || duration <= 0 || duration > 1)
    return due;
  const double end_ms = std::floor((pts + duration) * 1000);
  if (!std::isfinite(end_ms) || end_ms > 9007199254740991.0) return due;
  while (!queue_.empty()) {
    auto& c = queue_.front();
    if (c.type == CommandType::metadata)
      transitioning_ = *c.transitioning;
    else if (!transitioning_ && c.type != CommandType::end_layout) {
      queued_bytes_ -= c.encoded.size();
      queue_.pop_front();
      continue;
    } else if (c.pts_ms && *c.pts_ms >= end_ms)
      break;
    queued_bytes_ -= c.encoded.size();
    due.push_back(std::move(c));
    queue_.pop_front();
  }
  return due;
}
void Timeline::reset() {
  queue_.clear();
  queued_bytes_ = 0;
  transitioning_ = false;
}
}  // namespace headunit::theme
