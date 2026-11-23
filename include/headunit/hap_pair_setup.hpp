// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/srp_server.hpp"
#include "headunit/hap_crypto.hpp"
#include "headunit/mfi_authenticator.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace headunit {

struct HapPairRecord {
  std::string identifier;
  HapKey32 public_key{};
};

// HomeKit pair-setup. Without a private identity it stops at M4; with one it
// can verify M5 and sign M6. Persistence and pair-verify belong to the host.
class HapPairSetup {
 public:
  explicit HapPairSetup(std::string_view setup_secret, std::string_view accessory_identifier = {},
                        const std::string& private_key_path = {},
                        MfiAuthenticator* mfi_auth = nullptr);
  ~HapPairSetup();
  HapPairSetup(HapPairSetup&&) = delete;
  HapPairSetup& operator=(HapPairSetup&&) = delete;
  HapPairSetup(const HapPairSetup&) = delete;
  HapPairSetup& operator=(const HapPairSetup&) = delete;

  std::vector<std::uint8_t> respond(std::span<const std::uint8_t> request);
  bool proof_verified() const;
  std::optional<std::array<std::uint8_t, SrpServer::proof_size>> transient_control_secret() const;
  const std::optional<HapPairRecord>& paired_controller() const;

 private:
  std::string setup_secret_;
  std::unique_ptr<SrpServer> srp_;
  std::string accessory_identifier_;
  std::unique_ptr<HapEd25519Identity> identity_;
  std::optional<HapPairRecord> paired_controller_;
  MfiAuthenticator* mfi_auth_ = nullptr;
  std::uint8_t method_ = 0;
  bool transient_ = false;
  bool verified_ = false;
};

}  // namespace headunit
