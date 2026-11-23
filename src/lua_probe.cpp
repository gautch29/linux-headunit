// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/lua_host.hpp"
#include "headunit/asset_bundle.hpp"
#include "headunit/binary_plist.hpp"
#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  const bool allow_expired = argc > 1 && std::string_view(argv[argc - 1]) == "--allow-expired-leaf";
  if (allow_expired) --argc;
  if (argc != 5 && argc != 6) {
    std::cerr << "usage: headunit-lua-probe ASSET_DIRECTORY IDENTIFIER VERSION "
                 "LOCAL_REFERENCE_OR_DASH [REFERENCE_PNG] [--allow-expired-leaf]\n";
    return 1;
  }
  try {
    const std::filesystem::path directory(argv[1]);
    const std::string version(argv[3]);
    std::uint64_t number = 0;
    const auto [end, error] =
        std::from_chars(version.data(), version.data() + version.size(), number);
    if (error != std::errc{} || end != version.data() + version.size())
      throw std::invalid_argument("invalid version");
    headunit::AssetBundlePolicy policy{argv[2], number, {}};
    if (allow_expired)
      policy.certificate_time_policy = headunit::AssetCertificateTimePolicy::allow_expired_leaf;
    if (std::string_view(argv[4]) != "-")
      policy.previously_accepted = headunit::read_asset_reference(argv[4]);
    const auto bundle = headunit::AssetBundle::open(
        (directory / "archive.zip").string(), (directory / "signature.der").string(),
        (directory / "certificate.pem").string(), (directory / "trusted-root.pem").string(),
        policy);
    if (bundle.info().expired_signer_exception)
      std::clog << "Asset trust: expiredSigningLeafExceptionUsed=true; chain and archive signature "
                   "verified\n";
    headunit::LuaHost host([&](const std::string& name) { return bundle.resource(name); });
    host.connected(true);
    const headunit::LuaHost::Bytes input{1, 2, 3};
    if (!host.receive(input)) throw std::runtime_error("synthetic input rejected");
    host.execute(R"(
      connection=false; thread=false; session=false
      sessionReady=false; commandReceived=false; sessionReset=false
      local Connection = require 'APL.ClusterControlConnection'
      local Thread = require 'APL.Thread'
      connection = Connection.new()
      local r,w,e = connection:status(0)
      assert(r and w and e == nil)
      local value,err = connection:receiveMessage(0)
      assert(value and #value == 3 and err == nil)
      assert(connection:sendMessage(value,0))
      thread = Thread.new(function()
        assert(connection:sendMessage('first',0))
        Thread.yield()
        assert(connection:sendMessage('second',0))
      end)
      thread:start()
    )",
                 "independent-connection-check");
    host.tick(1);
    host.tick(2);
    const auto messages = host.take_outbound();
    if (messages.size() != 3 || messages[0] != input ||
        messages[1] != headunit::LuaHost::Bytes{'f', 'i', 'r', 's', 't'} ||
        messages[2] != headunit::LuaHost::Bytes{'s', 'e', 'c', 'o', 'n', 'd'} ||
        host.active_tasks() != 0)
      throw std::runtime_error("asset connection/coroutine check did not complete");
    host.execute("connection:reset()");
    if (host.reset_count() != 1) throw std::runtime_error("asset reset was not forwarded");
    std::cout << "Verified asset connection and task scripts executed: binary echo, coroutine "
                 "yield/resume, completion and reset passed.\n";
    host.connected(true);
    host.execute(R"(
      local Session = require 'APL.ClusterControlSession'
      sessionReady = false
      commandReceived = false
      sessionReset = false
      session = Session.new('synthetic-phone')
      session:setResetHandler(function() sessionReset = true end)
      session:setReadyHandler(function(id) assert(id=='synthetic-phone'); sessionReady=true end)
      session:setDataHandler(function(data)
        local command = CP.parsePlist(data)
        assert(command.fixture=='peer-command' and command.value==7)
        commandReceived = true
      end)
      session:start()
      assert(not session:getIsReady())
    )",
                 "independent-session-check");
    using V = headunit::PlistValue;
    using D = V::Dictionary;
    const auto integer = [](std::int64_t value) {
      return V{value};
    };
    const auto one_message = [&]() {
      const auto outgoing = host.take_outbound();
      if (outgoing.size() != 1) throw std::runtime_error("expected exactly one session message");
      auto decoded = headunit::decode_binary_plist(outgoing[0]);
      return std::get<D>(std::move(decoded.value));
    };
    const auto expect_number = [&](const D& message, const char* name, std::int64_t expected) {
      if (message.at(name) != integer(expected))
        throw std::runtime_error("unexpected session sequence/type/version");
    };
    host.tick(3);
    const auto reset = one_message();
    expect_number(reset, "type", 0);
    expect_number(reset, "version", 2);
    expect_number(reset, "ssn", 1);
    expect_number(reset, "psn", 1);
    expect_number(reset, "asn", 0);
    if (std::get<std::string>(reset.at("id").value) != "synthetic-phone")
      throw std::runtime_error("unexpected session identity");
    const auto wrong_identity =
        headunit::encode_binary_plist(V{D{{"type", integer(1)},
                                          {"version", integer(2)},
                                          {"ssn", integer(1)},
                                          {"psn", integer(1)},
                                          {"asn", integer(1)},
                                          {"id", V{std::string("other-phone")}}}});
    if (!host.receive(wrong_identity))
      throw std::runtime_error("synthetic wrong-identity input rejected by queue");
    host.tick(3.005);
    host.execute("assert(not sessionReady and not session:getIsReady())");
    if (!host.take_outbound().empty())
      throw std::runtime_error("wrong phone identity was acknowledged");
    const auto reply =
        headunit::encode_binary_plist(V{D{{"type", integer(1)},
                                          {"version", integer(2)},
                                          {"ssn", integer(1)},
                                          {"psn", integer(1)},
                                          {"asn", integer(1)},
                                          {"id", V{std::string("synthetic-phone")}}}});
    if (!host.receive(reply)) throw std::runtime_error("session reset reply rejected");
    host.tick(3.01);
    host.execute("assert(sessionReady and session:getIsReady())");
    host.tick(3.02);
    const auto complete = one_message();
    expect_number(complete, "type", 1);
    expect_number(complete, "psn", 2);
    expect_number(complete, "asn", 1);
    const auto payload = headunit::encode_binary_plist(
        V{D{{"fixture", V{std::string("peer-command")}}, {"value", integer(7)}}});
    const auto command = headunit::encode_binary_plist(V{D{{"type", integer(2)},
                                                           {"ssn", integer(1)},
                                                           {"psn", integer(2)},
                                                           {"asn", integer(2)},
                                                           {"data", V{payload}}}});
    if (!host.receive(command)) throw std::runtime_error("session command rejected");
    host.tick(3.03);
    host.execute("assert(commandReceived)");
    host.tick(3.04);
    const auto acknowledgment = one_message();
    expect_number(acknowledgment, "type", 3);
    expect_number(acknowledgment, "psn", 3);
    expect_number(acknowledgment, "asn", 2);
    host.execute("session:sendCommand({fixture='theme-command',value=9})");
    host.tick(3.05);
    const auto outgoing_command = one_message();
    expect_number(outgoing_command, "type", 2);
    expect_number(outgoing_command, "psn", 4);
    const auto data =
        headunit::decode_binary_plist(std::get<V::Bytes>(outgoing_command.at("data").value));
    if (data != V{D{{"fixture", V{std::string("theme-command")}}, {"value", integer(9)}}})
      throw std::runtime_error("nested command plist changed");
    const auto stale_session = headunit::encode_binary_plist(
        V{D{{"type", integer(3)}, {"ssn", integer(2)}, {"psn", integer(3)}, {"asn", integer(4)}}});
    if (!host.receive(stale_session))
      throw std::runtime_error("synthetic stale-session input rejected by queue");
    host.tick(3.06);
    host.execute("assert(session:getIsReady() and not sessionReset)");
    if (!host.take_outbound().empty()) throw std::runtime_error("stale session produced a reply");
    const auto missing_sequence = headunit::encode_binary_plist(
        V{D{{"type", integer(3)}, {"ssn", integer(1)}, {"psn", integer(4)}, {"asn", integer(4)}}});
    if (!host.receive(missing_sequence))
      throw std::runtime_error("synthetic missing-sequence input rejected by queue");
    host.tick(3.07);
    host.execute("assert(not session:getIsReady() and sessionReset)");
    if (host.reset_count() != 2)
      throw std::runtime_error("sequence gap did not reset the connection");
    host.execute("session:destroy()");
    if (host.active_tasks()) throw std::runtime_error("session tasks survived destroy");
    std::cout << "Verified theme session: reset/version/identity negotiation, ready callback, "
                 "inbound command, acknowledgment, outbound nested command, identity/session "
                 "rejection and sequence-gap reset passed.\n";
    headunit::LuaHost drawing([&](const std::string& name) { return bundle.resource(name); },
                              64 * 1024 * 1024, {{"Cluster_Display", 64, 64, 1.0 / 60}});
    drawing.execute(R"(
      root=false
      local Layer = require 'APL.Layer'
      local Shape = require 'APL.ShapeLayer'
      local Path = require 'APL.Path'
      local Rect = require 'APL.Rect'
      local Transform = require 'APL.Transform'
      root=Layer.new():setFrame(0,0,64,64):setBackgroundColor('#000000ff')
      local shape=Shape.new():setPath(Path.new():rectangleIn(Rect.new(8,8,32,32)))
      shape:setFillColor('#ff0000ff')
      shape:setTransform(Transform.newWithTranslation(4,0))
      root:addSublayer(shape)
      CP.setRootLayer('Cluster_Display',root:getNativeLayer())
    )",
                    "reused-native-graphics-check");
    const auto picture = drawing.render_reference();
    if (picture.pixels.at(20 * 64 + 20) != 0xffff0000 ||
        picture.pixels.at(50 * 64 + 50) != 0xff000000)
      throw std::runtime_error("verified asset drawing helper pixels mismatch");
    if (argc == 6) headunit::theme::graphics::write_png(picture, argv[5]);
    std::cout << "Verified asset layer/path/shape/rect/transform helpers rendered through our "
                 "reused native graphics module.\n";
    std::cout << "Lua allocated bytes=" << host.memory_used()
              << ". No phone channel or graphics readiness advertised.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
