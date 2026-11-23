// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_service.hpp>
#include <iostream>
#include <stdexcept>
using namespace headunit::theme;
namespace v = headunit::theme::vehicle;
namespace op = headunit::theme::opack;
void check(bool b) {
  if (!b) throw std::runtime_error("vehicle-data test failed");
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid vehicle-data message accepted");
}
int main() {
  try {
    v::ReadOnlyService service;
    auto handle = [&](const Bytes& bytes) {
      return service.handle(bytes, 0);
    };
    v::Command cmd{"configRequest", 42, {}, {}, {}};
    auto wire = v::encode_outgoing(cmd, 0);
    auto in = v::decode_incoming(wire);
    check(in.plugin_id == 0 && in.command.transaction == 42 && in.command.name == "configRequest");
    auto result = handle(wire);
    check(result.has_value());
    auto response = v::decode_incoming(*result).command;
    check(response.name == "configResponse" && response.transaction == 42);
    check(response.values == v::empty_configuration() && !response.error);
    cmd.transaction.reset();
    response = v::decode_incoming(*handle(v::encode_outgoing(cmd, 0))).command;
    check(response.name == "configNotify" && !response.transaction);
    cmd = {"writeRequest", 9, op::Value{op::Dictionary{}}, {}, {}};
    response = v::decode_incoming(*handle(v::encode_outgoing(cmd, 0))).command;
    check(response.error == -6710 && !response.values && response.transaction == 9);
    check(!handle(v::encode_outgoing(response, 0)));
    cmd.name = "configRequest";
    response = v::decode_incoming(*handle(v::encode_outgoing(cmd, 7))).command;
    check(response.error == -6710);
    cmd = {"readResponse", 123, op::Value{op::Array{}}, op::Value{op::Dictionary{}}, -6710};
    auto parsed = v::parse_command(v::command_value(cmd));
    check(parsed.error == -6710 && !parsed.values && !parsed.errors);
    rejects([] { v::parse_command(op::Value{op::Array{}}); });
    rejects([] { v::decode_incoming(op::encode(op::Value{op::Dictionary{}})); });
    rejects([&] { v::encode_outgoing(cmd, -1); });
    rejects([] {
      v::parse_command(op::Value{op::Dictionary{
          {op::Value{std::string("command")}, op::Value{std::string("configRequest")}},
          {op::Value{std::string("transactionID")}, op::Value{true}}}});
    });
    for (std::size_t i = 0; i < wire.size(); ++i)
      rejects([&] { v::decode_incoming(std::span(wire).first(i)); });
    std::cout
        << "Vehicle routing, config replies, unsupported controls and malformed input passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
