// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/hap_crypto.hpp"
#include "headunit/hap_pair_store.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace headunit {

// HomeKit pair-verify M1-M4 against a previously stored controller identity.
class HapPairVerify {
 public:
  HapPairVerify(std::string accessory_identifier, const std::string& private_key_path,
                const HapPairStore& pair_store);
  ~HapPairVerify();
  HapPairVerify(const HapPairVerify&) = delete;
  HapPairVerify& operator=(const HapPairVerify&) = delete;

  std::vector<std::uint8_t> respond(std::span<const std::uint8_t> request);
  bool verified() const;
  // Present only after the stored controller's M3 signature was verified.
  // Private identity data; use locally to bind session policy, never log it.
  std::optional<HapPairRecord> verified_controller() const;
  std::optional<HapKey32> shared_secret() const;

 private:
  std::string accessory_identifier_;
  HapEd25519Identity identity_;
  const HapPairStore& pair_store_;
  std::unique_ptr<HapX25519Ephemeral> ephemeral_;
  HapKey32 controller_ephemeral_{};
  std::optional<HapKey32> shared_;
  std::optional<HapKey32> encryption_key_;
  std::optional<HapPairRecord> controller_;
  unsigned stage_ = 0;
};

}  // namespace headunit
