// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/mfi_coprocessor.hpp"
#include <headunit/iap2_carplay.hpp>
#include <headunit/iap2_session_engine.hpp>
#include <headunit/iap2_parameter.hpp>
#include <map>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace headunit;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
ControlMessage availability(std::uint8_t value) {
  const std::array<Iap2Parameter, 1> flag{{{0, {value}}}};
  const std::array<Iap2Parameter, 1> group{{{1, encode_iap2_parameters(flag)}}};
  return {0x4300, encode_iap2_parameters(group)};
}
struct Bus : MfiRegisterBus {
  std::map<std::uint8_t, std::vector<std::uint8_t>> registers;
  Bus() {
    registers[0x02] = {3};
    registers[0x05] = {0};
    registers[0x10] = {0x10};
    registers[0x11] = {0, 64};
    registers[0x12] = std::vector<std::uint8_t>(64, 42);
    registers[0x30] = {0, 32};
    registers[0x31] = std::vector<std::uint8_t>(32, 51);
  }
  void read(std::uint8_t r, std::span<std::uint8_t> out) override {
    const auto& v = registers.at(r);
    CHECK(v.size() >= out.size());
    std::copy_n(v.begin(), out.size(), out.begin());
  }
  void write(std::uint8_t, std::span<const std::uint8_t>) override {}
};
int main() try {
  Iap2CarplayStart config;
  config.identity.device_id = "02:11:22:33:44:55";
  config.identity.public_key_hex = std::string(64, 'a');
  config.identity.source_version = "950.7.1";
  config.wifi = {"Fixture-AP", "fixture-password", 36, 2};
  auto plain = make_carplay_start(config);
  CHECK(plain.identifier == 0x4301);
  auto fields = decode_iap2_parameters(plain.parameters);
  CHECK(fields.size() == 5 && fields[1].identifier == 2 &&
        fields[1].value == std::vector<std::uint8_t>({0, 0, 0x1b, 0x58}));
  auto wifi = decode_iap2_parameters(fields[0].value);
  CHECK(wifi.size() == 5 && wifi[2].value == std::vector<std::uint8_t>{36} &&
        wifi[4].value == std::vector<std::uint8_t>{2});
  config.installed_asset = Iap2ThemeAsset{"FIXTURE", "19.0", 985};
  const auto message = make_carplay_start(config);
  fields = decode_iap2_parameters(message.parameters);
  CHECK(fields.size() == 7 && fields[5].identifier == 6 && fields[6].identifier == 7);
  auto asset = decode_iap2_parameters(fields[6].value);
  CHECK(asset.size() == 2 && asset[1].value == std::vector<std::uint8_t>({0, 0, 3, 217}));
  CHECK(!wireless_carplay_available(availability(0)) &&
        wireless_carplay_available(availability(1)));
  bool rejected = false;
  try {
    wireless_carplay_available(availability(2));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
  auto duplicate = availability(1);
  const auto original = duplicate.parameters;
  duplicate.parameters.insert(duplicate.parameters.end(), original.begin(), original.end());
  rejected = false;
  try {
    wireless_carplay_available(duplicate);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
  auto invalid = config;
  invalid.installed_asset->version = 0x80000000;
  rejected = false;
  try {
    make_carplay_start(invalid);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
  Iap2Identity identity;
  identity.bluetooth_address = {1, 2, 3, 4, 5, 6};
  identity.supports_theme_assets = true;
  const auto identity_fields =
      decode_iap2_parameters(make_identification_reply(identity).parameters);
  const auto transport = decode_iap2_parameters(identity_fields.back().value);
  CHECK(transport.size() == 5 && transport.back() == Iap2Parameter({5, {}}));
  Bus bus;
  MfiCoprocessor chip(bus);
  Iap2SessionEngine session(99, identity, &chip, std::nullopt, config);
  auto now = Iap2SessionEngine::Clock::now();
  (void)session.receive_bluetooth(Iap2SessionEngine::sync_preamble(), now);
  (void)session.receive_bluetooth(encode_link_frame({0xc0, 135, 99, 0, {1}}), now);
  std::uint8_t peer = 135, ack = 99;
  auto send = [&](ControlMessage m) {
    auto output = session.receive_bluetooth(
        encode_link_frame({0x40, ++peer, ack, 10, encode_control_message(m)}), now);
    std::vector<ControlMessage> replies;
    for (auto& wire : output.bluetooth) {
      LinkFrameReader r;
      const auto f = r.feed(wire);
      CHECK(f.size() == 1);
      if (f[0].session == 10) {
        ack = f[0].sequence;
        ControlMessageReader c;
        for (auto& v : c.feed(f[0].payload))
          replies.push_back(std::move(v));
      }
    }
    return replies;
  };
  CHECK(send({0xaa00, {}}).at(0).identifier == 0xaa01);
  const std::array<Iap2Parameter, 1> challenge{{{0, std::vector<std::uint8_t>(32, 12)}}};
  CHECK(send({0xaa02, encode_iap2_parameters(challenge)}).at(0).identifier == 0xaa03);
  CHECK(send({0xaa05, {}}).empty());
  CHECK(send(availability(0)).empty() && !session.carplay_start_sent());
  auto replies = send(availability(1));
  CHECK(replies.size() == 1 && replies[0].parameters == message.parameters &&
        session.carplay_start_sent() && session.handoff_sent());
  CHECK(send(availability(1)).empty());
  Iap2SessionEngine unauthenticated(99, identity, nullptr, std::nullopt, config);
  (void)unauthenticated.receive_bluetooth(Iap2SessionEngine::sync_preamble(), now);
  (void)unauthenticated.receive_bluetooth(encode_link_frame({0xc0, 135, 99, 0, {1}}), now);
  rejected = false;
  try {
    (void)unauthenticated.receive_bluetooth(
        encode_link_frame({0x40, 136, 99, 10, encode_control_message(availability(1))}), now);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected && !unauthenticated.carplay_start_sent());
  std::cout << "CarPlay start metadata/authentication tests passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
