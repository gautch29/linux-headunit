// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/script_channel.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace headunit::theme::script {
Channel::Channel(ResourceReader reader, std::vector<Display> displays, Limits limits) {
  initialize(reader, std::move(displays), limits);
}
Channel::Channel(const TrustedDirectory& root, std::vector<Display> displays, Limits limits) {
  initialize(root, std::move(displays), limits);
}
template <class Resources>
void Channel::initialize(const Resources& asset, std::vector<Display> displays, Limits limits) {
  if (displays.empty() || displays.size() > 8)
    throw std::invalid_argument("invalid script display count");
  std::set<std::string> identifiers;
  // Validate uniqueness before allocating any VM. Unknown phone IDs cannot
  // create a display, load an asset or grow the configured set.
  for (const auto& display : displays)
    if (!identifiers.insert(display.identifier).second)
      throw std::invalid_argument("duplicate script display");
  for (auto& display : displays) {
    const auto id = display.identifier;
    entries_.push_back({id, std::make_unique<Runtime>(asset, std::move(display), limits)});
  }
}
Runtime& Channel::runtime(std::string_view id) {
  const auto i = std::find_if(entries_.begin(), entries_.end(),
                              [&](const auto& e) { return e.identifier == id; });
  if (i == entries_.end()) throw std::invalid_argument("unknown script display");
  return *i->vm;
}
void Channel::set_connected(bool connected) {
  for (auto& e : entries_)
    e.vm->set_connected(connected);
  if (!connected) next_ = 0;
}
Delivery Channel::receive(std::span<const std::uint8_t> bytes) {
  ui::Message message;
  try {
    message = ui::decode(bytes);
  } catch (const std::invalid_argument&) {
    return Delivery::malformed;
  }
  return receive(std::move(message));
}
Delivery Channel::receive(ui::Message message) {
  if (message.data.empty() || message.data.size() > ui::max_payload_bytes)
    return Delivery::malformed;
  const auto i = std::find_if(entries_.begin(), entries_.end(),
                              [&](const auto& e) { return e.identifier == message.display_uuid; });
  if (i == entries_.end()) return Delivery::unknown_display;
  if (i->vm->stats().faulted) return Delivery::faulted;
  return i->vm->receive(std::move(message.data)) ? Delivery::accepted : Delivery::backpressure;
}
std::optional<Bytes> Channel::take_outgoing() {
  if (auto message = take_outgoing_message()) return ui::encode(*message);
  return {};
}
std::optional<ui::Message> Channel::take_outgoing_message() {
  for (std::size_t offset = 0; offset < entries_.size(); ++offset) {
    const auto index = (next_ + offset) % entries_.size();
    auto& e = entries_[index];
    if (auto data = e.vm->take_outgoing()) {
      next_ = (index + 1) % entries_.size();
      return ui::Message{e.identifier, std::move(*data)};
    }
  }
  return {};
}
}  // namespace headunit::theme::script
