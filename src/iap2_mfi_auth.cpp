// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_mfi_auth.hpp"

#include "headunit/iap2_parameter.hpp"

#include <array>
#include <stdexcept>

namespace headunit {

std::optional<ControlMessage> Iap2MfiAuth::on_message(const ControlMessage& message) {
  if (message.identifier == 0xaa00) {
    if (state_ != State::WaitingForCertificateRequest || !message.parameters.empty()) {
      throw std::invalid_argument("unexpected MFi certificate request");
    }
    const auto certificate = auth_.read_certificate();
    const std::array<Iap2Parameter, 1> parameters{{{0, certificate}}};
    state_ = State::WaitingForChallenge;
    return ControlMessage{0xaa01, encode_iap2_parameters(parameters)};
  }
  if (message.identifier == 0xaa02) {
    if (state_ != State::WaitingForChallenge) {
      throw std::invalid_argument("unexpected MFi challenge");
    }
    const auto parameters = decode_iap2_parameters(message.parameters);
    if (parameters.size() != 1 || parameters[0].identifier != 0 ||
        parameters[0].value.size() != 32) {
      throw std::invalid_argument("MFi challenge must contain one 32-byte parameter");
    }
    const auto digest = std::span<const std::uint8_t, 32>(parameters[0].value);
    const auto signature = auth_.sign_digest(digest);
    const std::array<Iap2Parameter, 1> reply{{{0, {signature.begin(), signature.end()}}}};
    state_ = State::WaitingForResult;
    return ControlMessage{0xaa03, encode_iap2_parameters(reply)};
  }
  if (message.identifier == 0xaa05) {
    if (state_ != State::WaitingForResult || !message.parameters.empty()) {
      throw std::invalid_argument("unexpected MFi success result");
    }
    state_ = State::Complete;
    return std::nullopt;
  }
  if (message.identifier == 0xaa04) {
    state_ = State::Failed;
    return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace headunit
