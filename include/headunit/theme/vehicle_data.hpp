// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/opack.hpp>
#include <optional>

namespace headunit::theme::vehicle {
struct Command {
  std::string name;
  std::optional<std::int64_t> transaction;
  std::optional<opack::Value> values;
  std::optional<opack::Value> errors;
  std::optional<std::int32_t> error;
};
struct Incoming {
  std::int64_t plugin_id;
  std::uint8_t priority;
  Command command;
};

opack::Value command_value(const Command& command);
Command parse_command(const opack::Value& value);
// Phone -> accessory includes a required pluginID and nested OPACK data.
Incoming decode_incoming(std::span<const std::uint8_t> bytes);
// Socket envelope from the SDK router's final send path: pluginID + pluginData.
// Its exported CreateMessage constructor is an internal communication envelope,
// not this wire envelope. Priority selects the socket and is not a wire field.
Bytes encode_outgoing(const Command& command, std::int64_t plugin_id);
opack::Value empty_configuration();
opack::Value bench_server_info();

}  // namespace headunit::theme::vehicle
