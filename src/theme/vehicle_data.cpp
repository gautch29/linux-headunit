// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_data.hpp>
#include <limits>
#include <stdexcept>

namespace headunit::theme::vehicle {
namespace {
using opack::Dictionary;
using opack::Value;
Value str(std::string_view s) {
  return Value{std::string(s)};
}
const Value* field(const Value& v, std::string_view name) {
  const auto* dict = std::get_if<Dictionary>(&v.data);
  if (!dict) throw std::invalid_argument("vehicle-data dictionary required");
  const Value* found = nullptr;
  for (const auto& [key, value] : *dict) {
    const auto* s = std::get_if<std::string>(&key.data);
    if (!s) throw std::invalid_argument("vehicle-data field name must be a string");
    if (*s == name) {
      if (found) throw std::invalid_argument("duplicate vehicle-data field");
      found = &value;
    }
  }
  return found;
}
std::int64_t integer(const Value& v) {
  const auto* n = std::get_if<std::int64_t>(&v.data);
  if (!n) throw std::invalid_argument("vehicle-data integer required");
  return *n;
}
const Value& required(const Value& v, std::string_view name) {
  const auto* f = field(v, name);
  if (!f) throw std::invalid_argument("missing vehicle-data field");
  return *f;
}
std::string name(const Value& v) {
  const auto* s = std::get_if<std::string>(&v.data);
  if (!s || s->empty() || s->size() > 64)
    throw std::invalid_argument("invalid vehicle-data command");
  return *s;
}
void add(Dictionary& d, std::string_view key, const Value& v) {
  d.emplace_back(str(key), v);
}
}  // namespace

opack::Value command_value(const Command& command) {
  (void)name(str(command.name));
  Dictionary d;
  add(d, "command", str(command.name));
  if (command.transaction) add(d, "transactionID", Value{*command.transaction});
  if (command.error && *command.error != 0) {
    add(d, "error", Value{std::int64_t(*command.error)});
  } else {
    if (command.values) add(d, "values", *command.values);
    if (command.errors) add(d, "errors", *command.errors);
  }
  return Value{std::move(d)};
}

Command parse_command(const opack::Value& value) {
  Command c;
  c.name = name(required(value, "command"));
  if (const auto* f = field(value, "transactionID")) c.transaction = integer(*f);
  if (const auto* f = field(value, "values")) c.values = *f;
  if (const auto* f = field(value, "errors")) c.errors = *f;
  if (const auto* f = field(value, "error")) {
    const auto n = integer(*f);
    if (n < INT32_MIN || n > INT32_MAX)
      throw std::invalid_argument("vehicle-data error out of range");
    c.error = static_cast<std::int32_t>(n);
  }
  return c;
}

Incoming decode_incoming(std::span<const std::uint8_t> bytes) {
  const auto outer = opack::decode(bytes);
  Incoming result{};
  result.plugin_id = integer(required(outer, "pluginID"));
  if (result.plugin_id < 0) throw std::invalid_argument("negative vehicle-data plugin ID");
  if (const auto* f = field(outer, "priority")) {
    const auto n = integer(*f);
    if (n < 0 || n > 255) throw std::invalid_argument("vehicle-data priority out of range");
    result.priority = static_cast<std::uint8_t>(n);
  }
  const auto* inner = std::get_if<Bytes>(&required(outer, "pluginData").data);
  if (!inner) throw std::invalid_argument("vehicle-data pluginData must be bytes");
  result.command = parse_command(opack::decode(*inner));
  return result;
}

Bytes encode_outgoing(const Command& command, std::int64_t plugin_id) {
  if (plugin_id < 0) throw std::invalid_argument("negative vehicle-data plugin ID");
  Dictionary d;
  add(d, "pluginData", Value{opack::encode(command_value(command))});
  add(d, "pluginID", Value{plugin_id});
  return opack::encode(Value{std::move(d)});
}

opack::Value empty_configuration() {
  return Value{Dictionary{{str("accessories"), Value{opack::Array{}}}}};
}

opack::Value bench_server_info() {
  const auto plugin_name = str("Communication Plug-in");
  auto plugin = empty_configuration();
  auto& d = std::get<Dictionary>(plugin.data);
  add(d, "pluginID", Value{std::int64_t(0)});
  add(d, "name", plugin_name);
  return Value{
      Dictionary{{str("protocolVersion"), str("1.0")},
                 {str("pluginCount"), Value{std::int64_t(1)}},
                 {str("pluginMapping"), Value{Dictionary{{plugin_name, Value{std::int64_t(0)}}}}},
                 {str("pluginConfigs"), Value{opack::Array{plugin}}}}};
}

}  // namespace headunit::theme::vehicle
