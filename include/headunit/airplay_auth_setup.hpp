// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/mfi_authenticator.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

// AirPlay's MFiSAP exchange on an authenticated control connection.
// This handler owns no key material beyond a single request.
class AirplayAuthSetup {
 public:
  explicit AirplayAuthSetup(MfiAuthenticator& auth) : auth_(auth) {}
  std::vector<std::uint8_t> respond(std::span<const std::uint8_t> request);

 private:
  MfiAuthenticator& auth_;
};

}  // namespace headunit
