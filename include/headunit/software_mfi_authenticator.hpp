// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/mfi_authenticator.hpp"

#include <memory>
#include <string>

namespace headunit {

// Explicit experimental identity, loaded once before radio/listener startup.
// Local key consistency does not establish iPhone trust or certification.
class SoftwareMfiAuthenticator final : public MfiAuthenticator {
 public:
  SoftwareMfiAuthenticator(const std::string& private_key_path,
                           const std::string& certificate_path);
  ~SoftwareMfiAuthenticator() override;
  SoftwareMfiAuthenticator(const SoftwareMfiAuthenticator&) = delete;
  SoftwareMfiAuthenticator& operator=(const SoftwareMfiAuthenticator&) = delete;

  std::uint8_t protocol_major() override { return 3; }
  std::vector<std::uint8_t> read_certificate() override;
  std::array<std::uint8_t, 64> sign_digest(
      std::span<const std::uint8_t, 32> digest,
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace headunit
