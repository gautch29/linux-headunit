// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/iap2_message.hpp"
#include "headunit/mfi_authenticator.hpp"

#include <optional>

namespace headunit {

class Iap2MfiAuth {
 public:
  enum class State {
    WaitingForCertificateRequest,
    WaitingForChallenge,
    WaitingForResult,
    Complete,
    Failed
  };

  explicit Iap2MfiAuth(MfiAuthenticator& auth) : auth_(auth) {}
  State state() const { return state_; }
  void reset() { state_ = State::WaitingForCertificateRequest; }
  std::optional<ControlMessage> on_message(const ControlMessage& message);

 private:
  MfiAuthenticator& auth_;
  State state_ = State::WaitingForCertificateRequest;
};

}  // namespace headunit
