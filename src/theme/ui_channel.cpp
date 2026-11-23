// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_channel.hpp>
#include <algorithm>
#include <stdexcept>

namespace headunit::theme::ui {
namespace {
bool identifier(std::string_view value) {
  return !value.empty() && value.size() <= 256 && value.find('\0') == std::string_view::npos;
}
[[noreturn]] void invalid() {
  throw std::invalid_argument("invalid UI-sync channel envelope");
}
void validate(const Message& message) {
  if (!identifier(message.display_uuid) || message.data.empty() ||
      message.data.size() > max_payload_bytes)
    invalid();
}
}  // namespace
Bytes encode(const Message& message) {
  validate(message);
  return opack::encode(opack::Value{opack::Dictionary{
      {opack::Value{std::string("displayUUID")}, opack::Value{message.display_uuid}},
      {opack::Value{std::string("data")}, opack::Value{message.data}}}});
}
Message decode(std::span<const std::uint8_t> wire) {
  auto value = opack::decode(wire);
  const auto* dict = std::get_if<opack::Dictionary>(&value.data);
  if (!dict) invalid();
  Message message;
  bool has_id = false, has_data = false;
  for (const auto& [key, item] : *dict) {
    const auto* name = std::get_if<std::string>(&key.data);
    if (!name) invalid();
    if (*name == "displayUUID") {
      const auto* text = std::get_if<std::string>(&item.data);
      if (!text || has_id) invalid();
      message.display_uuid = *text;
      has_id = true;
    } else if (*name == "data") {
      const auto* bytes = std::get_if<Bytes>(&item.data);
      if (!bytes || has_data) invalid();
      message.data = *bytes;
      has_data = true;
    }
  }
  if (!has_id || !has_data) invalid();
  validate(message);
  return message;
}
Channel::Channel(std::vector<Display> displays, std::size_t limit, std::uint64_t timeout) {
  if (displays.empty() || displays.size() > 8)
    throw std::invalid_argument("invalid UI-sync display count");
  for (auto& display : displays) {
    if (!identifier(display.identifier) ||
        std::any_of(displays_.begin(), displays_.end(),
                    [&](const auto& e) { return e.identifier == display.identifier; }))
      throw std::invalid_argument("invalid or duplicate UI-sync display identifier");
    // Configuration follows the same UTF-8 bounds as a wire identifier.
    (void)opack::encode(opack::Value{display.identifier});
    displays_.push_back(
        {std::move(display.identifier), Session(std::move(display.peer_id), limit, timeout)});
  }
}
void Channel::start() {
  selected_.reset();
  pending_.clear();
  next_ = 0;
  for (auto& display : displays_)
    display.session.start();
}
void Channel::stop() {
  for (auto& display : displays_)
    display.session.stop();
  selected_.reset();
  pending_.clear();
  next_ = 0;
}
void Channel::invalidate(std::size_t index) {
  if (selected_ == index) {
    selected_.reset();
    pending_.clear();
  }
}
Routed Channel::receive(std::span<const std::uint8_t> wire) {
  Message message;
  try {
    message = decode(wire);
  } catch (const std::invalid_argument&) {
    return {{}, {Result::malformed, {}}};
  }
  for (std::size_t index = 0; index < displays_.size(); ++index) {
    auto& display = displays_[index];
    if (display.identifier != message.display_uuid) continue;
    const auto session = display.session.session_number();
    auto received = display.session.receive(message.data);
    if (session != display.session.session_number()) invalidate(index);
    return {message.display_uuid, std::move(received)};
  }
  return {message.display_uuid, {Result::ignored, {}}};
}
bool Channel::send_command(std::string_view id, Bytes data) {
  for (auto& display : displays_)
    if (display.identifier == id) return display.session.send_command(std::move(data));
  return false;
}
const Bytes* Channel::pending() {
  if (selected_) return &pending_;
  for (std::size_t offset = 0; offset < displays_.size(); ++offset) {
    const auto index = (next_ + offset) % displays_.size();
    auto& display = displays_[index];
    if (const auto* inner = display.session.pending()) {
      auto wire = encode(Message{display.identifier, *inner});
      pending_ = std::move(wire);
      selected_ = index;
      return &pending_;
    }
  }
  return nullptr;
}
void Channel::sent(std::uint64_t now_ms) {
  if (!selected_) throw std::logic_error("no pending UI-sync channel message");
  const auto index = *selected_;
  displays_[index].session.sent(now_ms);
  next_ = (index + 1) % displays_.size();
  selected_.reset();
  pending_.clear();
}
bool Channel::tick(std::uint64_t now_ms) {
  bool reset = false;
  for (std::size_t i = 0; i < displays_.size(); ++i)
    if (displays_[i].session.tick(now_ms)) {
      invalidate(i);
      reset = true;
    }
  return reset;
}
std::optional<State> Channel::state(std::string_view id) const {
  for (const auto& display : displays_)
    if (display.identifier == id) return display.session.state();
  return {};
}
}  // namespace headunit::theme::ui
