// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_transport.hpp>
#include <stdexcept>
#include <plist/plist.h>
#include <memory>
#include <limits>
#include <algorithm>

namespace headunit::theme::vehicle {
namespace {
constexpr std::uint32_t cmnd = 0x636d6e64, comm = 0x636f6d6d;
const opack::Value* resource(const std::optional<opack::Value>& values, std::int64_t iid) noexcept {
  if (!values) return nullptr;
  const auto* dictionary = std::get_if<opack::Dictionary>(&values->data);
  if (!dictionary) return nullptr;
  for (const auto& [key, value] : *dictionary) {
    const auto* id = std::get_if<std::int64_t>(&key.data);
    if (id && *id == iid) return &value;
  }
  return nullptr;
}
void increment(std::size_t& count) noexcept {
  if (count != std::numeric_limits<std::size_t>::max()) ++count;
}
const opack::Value* field(const opack::Value& value, std::string_view name) {
  for (const auto& [key, item] : std::get<opack::Dictionary>(value.data))
    if (const auto* text = std::get_if<std::string>(&key.data); text && *text == name) return &item;
  return nullptr;
}
const opack::Array& children(const opack::Value& value, std::string_view name) {
  const auto* item = field(value, name);
  if (!item) throw std::invalid_argument("coverage profile has no children");
  return std::get<opack::Array>(item->data);
}
std::optional<std::int64_t> integer_field(const opack::Value& value, std::string_view name) {
  const auto* item = field(value, name);
  if (item)
    if (const auto* number = std::get_if<std::int64_t>(&item->data)) return *number;
  return {};
}
template <class Visit>
bool visit_ids(const std::optional<opack::Value>& value, Visit visit) noexcept {
  if (!value) return true;
  std::array<std::int64_t, 512> seen{};
  std::size_t count = 0;
  auto id = [&](const opack::Value& key) {
    const auto* number = std::get_if<std::int64_t>(&key.data);
    if (!number || std::find(seen.begin(), seen.begin() + count, *number) != seen.begin() + count)
      return;
    if (count == seen.size()) return;
    seen[count++] = *number;
    visit(*number);
  };
  if (const auto* array = std::get_if<opack::Array>(&value->data)) {
    for (std::size_t i = 0; i < std::min(array->size(), seen.size()); ++i)
      id((*array)[i]);
    return array->size() <= seen.size();
  }
  if (const auto* dictionary = std::get_if<opack::Dictionary>(&value->data)) {
    for (std::size_t i = 0; i < std::min(dictionary->size(), seen.size()); ++i) {
      const auto& [key, item] = (*dictionary)[i];
      (void)item;
      id(key);
    }
    return dictionary->size() <= seen.size();
  }
  return true;
}
PlistValue plist(const opack::Value& v) {
  return std::visit(
      [](const auto& value) -> PlistValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> ||
                      std::is_same_v<T, double> || std::is_same_v<T, std::string> ||
                      std::is_same_v<T, Bytes>)
          return PlistValue{value};
        else if constexpr (std::is_same_v<T, opack::Array>) {
          PlistValue::Array a;
          for (const auto& item : value)
            a.push_back(plist(item));
          return PlistValue{std::move(a)};
        } else if constexpr (std::is_same_v<T, opack::Dictionary>) {
          PlistValue::Dictionary d;
          for (const auto& [key, item] : value) {
            const auto* name = std::get_if<std::string>(&key.data);
            if (!name || !d.emplace(*name, plist(item)).second)
              throw std::invalid_argument("non-plist vehicle configuration key");
          }
          return PlistValue{std::move(d)};
        } else
          throw std::invalid_argument("non-plist vehicle configuration value");
      },
      v.data);
}
}  // namespace
Coverage::Coverage(const opack::Value& profile) {
  for (const auto& accessory : children(profile, "accessories"))
    for (const auto& service : children(accessory, "services"))
      for (const auto& characteristic : children(service, "characteristics")) {
        const auto iid = integer_field(characteristic, "iid");
        if (!iid || known_.size() == 512 ||
            !known_
                 .emplace(
                     *iid,
                     Resource{
                         *iid, integer_field(service, "iid"), integer_field(service, "type"), {}})
                 .second)
          throw std::invalid_argument("invalid or oversized coverage profile");
      }
}
Coverage::ResourceCounts& Coverage::resource(unsigned channel, std::int64_t iid) {
  if (const auto found = known_.find(iid); found != known_.end())
    return found->second.channels[channel];
  for (auto& entry : unknown_)
    if (entry && entry->iid == iid) return entry->channels[channel];
  auto& entry = unknown_[next_unknown_];
  if (entry) increment(summary_.unknown_evictions);
  entry = Resource{iid, {}, {}, {}};
  next_unknown_ = (next_unknown_ + 1) % max_unknown;
  return entry->channels[channel];
}
std::vector<Coverage::Resource> Coverage::resources() const {
  std::vector<Resource> result;
  for (const auto& [iid, entry] : known_) {
    (void)iid;
    if (entry.channels != std::array<ResourceCounts, 2>{}) result.push_back(entry);
  }
  for (const auto& entry : unknown_)
    if (entry) result.push_back(*entry);
  return result;
}
void Coverage::request(unsigned channel, const Incoming& request,
                       const std::optional<Command>& response) noexcept {
  if (channel >= 2) {
    increment(summary_.diagnostic_errors);
    return;
  }
  auto& totals = summary_.channels[channel];
  increment(totals.received);
  const auto& command = request.command;
  auto& counts = totals.requests;
  if (command.name == "configRequest")
    increment(counts.configuration);
  else if (command.name == "readRequest")
    increment(counts.read);
  else if (command.name == "registerRequest")
    increment(counts.registration);
  else if (command.name == "unregisterRequest")
    increment(counts.unregistration);
  else if (command.name == "writeRequest")
    increment(totals.writes);
  else if (command.name == "controlRequest")
    increment(counts.control);
  else
    increment(counts.other);
  if (response && response->error && *response->error) increment(totals.response_errors);
  if (request.plugin_id != 0) return;  // This profile cannot classify another plug-in's IIDs.
  if (command.name != "readRequest" && command.name != "registerRequest" &&
      command.name != "unregisterRequest" && command.name != "writeRequest")
    return;  // Unknown/control payloads do not establish an IID selector contract.
  const auto observed = visit_ids(command.values, [&](std::int64_t iid) {
    auto& r = resource(channel, iid);
    if (command.name == "readRequest") increment(r.reads);
    if (command.name == "registerRequest") increment(r.registrations);
    if (command.name == "unregisterRequest") increment(r.unregistrations);
    if (command.name == "writeRequest") increment(r.writes);
  });
  if (!observed) increment(summary_.diagnostic_errors);
  const auto* wildcard = command.values ? std::get_if<std::string>(&command.values->data) : nullptr;
  if (response && !(response->error && *response->error) && wildcard && *wildcard == "*" &&
      command.name == "registerRequest") {
    visit_ids(response->values,
              [&](std::int64_t iid) { increment(resource(channel, iid).registrations); });
    visit_ids(response->errors,
              [&](std::int64_t iid) { increment(resource(channel, iid).registrations); });
  }
  if (response) this->response(channel, *response);
}
void Coverage::response(unsigned channel, const Command& response) noexcept {
  if (channel >= 2) return;
  if (response.error && *response.error) return;
  auto returned = [&](const std::optional<opack::Value>& values, bool error) {
    if (!visit_ids(values, [&](std::int64_t iid) {
          auto& r = resource(channel, iid);
          increment(error ? r.returned_errors : r.returned_values);
          if (!error && response.name == "writeResponse") increment(r.confirmed_writes);
        }))
      increment(summary_.diagnostic_errors);
  };
  if (response.name != "configResponse" && response.name != "configNotify") {
    returned(response.values, false);
    returned(response.errors, true);
  }
}

void Coverage::transferred(unsigned channel, const Incoming& message) noexcept {
  if (channel >= 2) {
    increment(summary_.diagnostic_errors);
    return;
  }
  increment(summary_.channels[channel].outputs);
  const auto& command = message.command;
  if (message.plugin_id != 0 || (command.error && *command.error) ||
      (command.name != "readResponse" && command.name != "registerResponse" &&
       command.name != "writeResponse" && command.name != "updateNotify"))
    return;
  if (!visit_ids(command.values,
                 [&](std::int64_t iid) { increment(resource(channel, iid).transferred_values); }) ||
      !visit_ids(command.errors,
                 [&](std::int64_t iid) { increment(resource(channel, iid).transferred_errors); }))
    increment(summary_.diagnostic_errors);
}
void Coverage::unsupported(unsigned channel) noexcept {
  if (channel < 2) increment(summary_.channels[channel].unsupported_types);
}
void Coverage::retry(unsigned channel) noexcept {
  if (channel < 2) increment(summary_.channels[channel].retries);
}
void Coverage::fault(unsigned channel) noexcept {
  if (channel < 2) increment(summary_.channels[channel].faults);
}
void Coverage::receipt(unsigned channel) noexcept {
  if (channel < 2) increment(summary_.channels[channel].receipts);
}

Transport::Transport(Service& service, WriteObserver observer, std::shared_ptr<ResourceTrace> trace,
                     std::shared_ptr<Coverage> coverage, unsigned channel)
    : service_(service),
      observer_(std::move(observer)),
      trace_(std::move(trace)),
      coverage_(coverage ? std::move(coverage) : std::make_shared<Coverage>(service.profile())),
      channel_(channel) {
  if (channel >= 2) throw std::invalid_argument("invalid vehicle coverage channel");
  response_owner_ = service_.attach_response_owner();
}
Transport::~Transport() {
  service_.retire_response_owner(response_owner_);
}
void Transport::pump(std::uint64_t now) {
  if (!deferred_request_) return;
  if (!service_.pending_response(response_owner_)) {
    deferred_request_.reset();
    return;
  }
  auto response = service_.complete_response(response_owner_, now);
  if (!response) return;
  const auto command = decode_incoming(*response).command;
  coverage_->response(channel_, command);
  if (observer_) {
    try {
      observer_(*deferred_request_, command);
    } catch (...) {
    }
  }
  deferred_request_.reset();
  DataMessage reply;
  reply.payload = std::move(*response);
  enqueue(std::move(reply));
}
ResourceTrace::ResourceTrace(std::array<std::int64_t, 2> iids) : iids_(iids) {
  if (iids[0] < 0 || iids[1] < 0 || iids[0] == iids[1])
    throw std::invalid_argument("tracked resources require two distinct valid IIDs");
}
void ResourceTrace::request(const Command& request, const Command& response) noexcept {
  if (response.error && *response.error) return;
  for (std::size_t index = 0; index < iids_.size(); ++index) {
    const auto id = iids_[index];
    auto& count = counts_[index];
    const bool included = resource(response.values, id) || resource(response.errors, id);
    if (request.name == "readRequest" && response.name == "readResponse" && included)
      increment(count.reads);
    if (request.name == "registerRequest" && response.name == "registerResponse" && included)
      increment(count.registrations);
    if (request.name == "unregisterRequest" && response.name == "unregisterResponse" &&
        request.values) {
      const auto* all = std::get_if<std::string>(&request.values->data);
      bool included_request = all && *all == "*";
      if (const auto* ids = std::get_if<opack::Array>(&request.values->data))
        for (const auto& value : *ids)
          if (const auto* n = std::get_if<std::int64_t>(&value.data); n && *n == id)
            included_request = true;
      if (included_request) increment(count.unregistrations);
    }
  }
}
void ResourceTrace::transferred(const Command& command) noexcept {
  if (command.error && *command.error) return;
  const bool update = command.name == "updateNotify";
  if (!update && command.name != "readResponse" && command.name != "registerResponse") return;
  for (std::size_t index = 0; index < iids_.size(); ++index) {
    auto& count = counts_[index];
    if (resource(command.errors, iids_[index]))
      increment(update ? count.update_errors : count.response_errors);
    else if (const auto* value = resource(command.values, iids_[index])) {
      if (const auto* on = std::get_if<bool>(&value->data))
        increment(update ? (*on ? count.updates_on : count.updates_off)
                         : (*on ? count.responses_on : count.responses_off));
    }
  }
}
void Transport::enqueue(DataMessage message) {
  const auto bytes = message.payload.size() + 32;
  if (pending_.size() >= max_messages || bytes > max_bytes - pending_bytes_)
    throw std::logic_error("vehicle output reservation exceeded");
  pending_bytes_ += bytes;
  pending_.push_back(std::move(message));
}
bool Transport::receive(const DataMessage& message, std::uint64_t now) {
  if (message.kind == DataMessageKind::reply) {
    if (message.type || !message.payload.empty() || message.error)
      throw std::invalid_argument("unexpected vehicle transport reply");
    coverage_->receipt(channel_);
    return true;  // We send asynchronous commands, so no ACK transaction is pending.
  }
  if (message.kind != DataMessageKind::synchronous && message.kind != DataMessageKind::asynchronous)
    throw std::invalid_argument("invalid vehicle message kind");
  if (message.error || message.payload.size() > max_packet_bytes)
    throw std::invalid_argument("invalid vehicle message header or size");
  // Reserve both the receipt and maximum command response before any writer
  // runs. A retry after backpressure must not execute a write a second time.
  const std::size_t reserved_messages = deferred_request_ ? 1 : 0;
  const std::size_t reserved_bytes = deferred_request_ ? max_packet_bytes + 32 : 0;
  const std::size_t required_bytes = reserved_bytes + max_packet_bytes + 64;
  if (pending_.size() > max_messages - reserved_messages - 2 ||
      required_bytes > max_bytes - pending_bytes_) {
    coverage_->retry(channel_);
    return false;
  }
  const bool supported = message.type == cmnd || message.type == comm;
  std::optional<Bytes> response;
  if (supported) {
    const auto incoming = decode_incoming(message.payload);
    response = service_.handle(message.payload, now, response_owner_);
    if (!response && incoming.command.name == "writeRequest" &&
        service_.pending_response(response_owner_))
      deferred_request_ = incoming.command;
    if (trace_ && response) {
      // Optional diagnostics cannot change authenticated command semantics.
      try {
        trace_->request(incoming.command, decode_incoming(*response).command);
      } catch (...) {
      }
    }
    try {
      std::optional<Command> result;
      if (response) result = decode_incoming(*response).command;
      coverage_->request(channel_, incoming, result);
    } catch (...) {
      coverage_->fault(channel_);
    }
    if (incoming.command.name == "writeRequest") {
      if (observer_ && response) {
        try {
          observer_(incoming.command, decode_incoming(*response).command);
        } catch (...) {
        }
      }
    }
  } else
    coverage_->unsupported(channel_);
  if (message.kind == DataMessageKind::synchronous) {
    DataMessage ack;
    ack.kind = DataMessageKind::reply;
    ack.type = 0;
    ack.group = message.group;
    ack.token = message.token;
    ack.error = supported ? 0 : -6710;
    enqueue(std::move(ack));
  }
  if (response) {
    DataMessage reply;
    reply.payload = std::move(*response);
    enqueue(std::move(reply));
  }
  return true;
}
bool Transport::notify(Bytes bytes) {
  if (bytes.size() > max_packet_bytes) throw std::invalid_argument("vehicle update too large");
  const std::size_t reserved_messages = deferred_request_ ? 1 : 0;
  const std::size_t reserved_bytes = deferred_request_ ? max_packet_bytes + 32 : 0;
  if (pending_.size() >= max_messages - reserved_messages ||
      bytes.size() + 32 > max_bytes - reserved_bytes - pending_bytes_)
    return false;
  DataMessage message;
  message.payload = std::move(bytes);
  enqueue(std::move(message));
  return true;
}
const DataMessage* Transport::pending() const {
  return pending_.empty() ? nullptr : &pending_.front();
}
void Transport::sent() {
  if (pending_.empty()) throw std::logic_error("no pending vehicle output");
  if (trace_ && !pending_.front().payload.empty()) {
    try {
      trace_->transferred(decode_incoming(pending_.front().payload).command);
    } catch (...) {
    }
  }
  if (!pending_.front().payload.empty()) {
    try {
      coverage_->transferred(channel_, decode_incoming(pending_.front().payload));
    } catch (...) {
      coverage_->fault(channel_);
    }
  }
  pending_bytes_ -= pending_.front().payload.size() + 32;
  pending_.pop_front();
}
Bytes add_server_info(std::span<const std::uint8_t> info, const Service& service,
                      std::uint64_t now) {
  // /info is generated locally by make_airplay_info, not received from a peer.
  // Preserve its unsigned capability fields: libplist can encode these as
  // 128-bit integer objects, outside the deliberately restricted UI codec.
  if (info.empty() || info.size() > 32768)
    throw std::invalid_argument("invalid generated AirPlay info size");
  plist_t raw = nullptr;
  if (plist_from_bin(reinterpret_cast<const char*>(info.data()), info.size(), &raw) !=
          PLIST_ERR_SUCCESS ||
      !raw)
    throw std::invalid_argument("invalid generated AirPlay info");
  std::unique_ptr<void, decltype(&plist_free)> root(raw, plist_free);
  if (plist_get_node_type(raw) != PLIST_DICT ||
      plist_dict_get_item(raw, "vehicleStateProtocolInfo"))
    throw std::invalid_argument("invalid or already configured AirPlay info");
  auto config = plist(service.configuration_snapshot(now));
  auto& plugin = std::get<PlistValue::Dictionary>(config.value);
  plugin.emplace("pluginID", PlistValue{std::int64_t(0)});
  plugin.emplace("name", PlistValue{std::string("Communication Plug-in")});
  const auto protocol = encode_binary_plist(PlistValue{PlistValue::Dictionary{
      {"protocolVersion", PlistValue{std::string("1.0")}},
      {"pluginCount", PlistValue{std::int64_t(1)}},
      {"pluginMapping",
       PlistValue{PlistValue::Dictionary{{"Communication Plug-in", PlistValue{std::int64_t(0)}}}}},
      {"pluginConfigs", PlistValue{PlistValue::Array{std::move(config)}}}}});
  plist_t field = nullptr;
  if (plist_from_bin(reinterpret_cast<const char*>(protocol.data()), protocol.size(), &field) !=
          PLIST_ERR_SUCCESS ||
      !field)
    throw std::logic_error("vehicle protocol plist failed to encode");
  plist_dict_set_item(raw, "vehicleStateProtocolInfo", field);
  char* encoded = nullptr;
  std::uint32_t length = 0;
  const auto result = plist_to_bin(raw, &encoded, &length);
  std::unique_ptr<void, decltype(&plist_mem_free)> bytes(encoded, plist_mem_free);
  if (result != PLIST_ERR_SUCCESS || !encoded || length > plist_wire_limit)
    throw std::invalid_argument("augmented AirPlay info too large");
  return Bytes(reinterpret_cast<const std::uint8_t*>(encoded),
               reinterpret_cast<const std::uint8_t*>(encoded) + length);
}
}  // namespace headunit::theme::vehicle
