// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/mfi_coprocessor.hpp"
#include "headunit/iap2_mfi_auth.hpp"
#include "headunit/iap2_parameter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Bus final : headunit::MfiRegisterBus {
  std::map<std::uint8_t, std::vector<std::uint8_t>> values;
  std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> writes;
  void read(std::uint8_t reg, std::span<std::uint8_t> out) override {
    const auto& value = values.at(reg);
    if (value.size() < out.size()) throw std::runtime_error("short fake register");
    std::copy_n(value.begin(), out.size(), out.begin());
  }
  void write(std::uint8_t reg, std::span<const std::uint8_t> in) override {
    writes.emplace_back(reg, std::vector<std::uint8_t>(in.begin(), in.end()));
  }
};
}  // namespace

int main() try {
  Bus bus;
  bus.values[0x02] = {3};
  bus.values[0x05] = {0};
  bus.values[0x10] = {0x10};
  bus.values[0x11] = {0, 64};
  bus.values[0x12] = std::vector<std::uint8_t>(64, 0x9b);
  bus.values[0x30] = {0x02, 0x5f};  // 607 bytes observed on the wire.
  bus.values[0x31] = std::vector<std::uint8_t>(607, 0x5b);
  headunit::MfiCoprocessor chip(bus);
  headunit::Iap2MfiAuth auth(chip);

  const auto certificate_reply = auth.on_message({0xaa00, {}});
  check(certificate_reply && certificate_reply->identifier == 0xaa01, "certificate reply ID");
  check(headunit::encode_control_message(*certificate_reply).size() == 617,
        "certificate reply length differs from observed iAP2 exchange");
  const auto certificate_params = headunit::decode_iap2_parameters(certificate_reply->parameters);
  check(certificate_params.size() == 1 && certificate_params[0].identifier == 0 &&
            certificate_params[0].value.size() == 607,
        "certificate parameter shape");

  const std::array<headunit::Iap2Parameter, 1> challenge{
      {{0, std::vector<std::uint8_t>(32, 0x42)}}};
  const headunit::ControlMessage challenge_message{0xaa02,
                                                   headunit::encode_iap2_parameters(challenge)};
  check(headunit::encode_control_message(challenge_message).size() == 42,
        "challenge length differs from observed exchange");
  const auto signature_reply = auth.on_message(challenge_message);
  check(signature_reply && signature_reply->identifier == 0xaa03, "signature reply ID");
  check(headunit::encode_control_message(*signature_reply).size() == 74,
        "signature length differs from observed exchange");
  check(bus.writes.size() == 3 && bus.writes[0].first == 0x20 &&
            bus.writes[0].second == std::vector<std::uint8_t>({0, 32}) &&
            bus.writes[1].first == 0x21 && bus.writes[1].second == challenge[0].value,
        "challenge was not sent intact to the coprocessor");
  check(auth.on_message({0xaa05, {}}) == std::nullopt &&
            auth.state() == headunit::Iap2MfiAuth::State::Complete,
        "success result did not complete authentication");

  auth.reset();
  bool rejected = false;
  try {
    (void)auth.on_message(challenge_message);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "out-of-order challenge was accepted");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
