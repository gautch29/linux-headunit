// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/hap_pair_setup.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace headunit {

// Private, receiver-owned storage for verified controller Ed25519 public keys.
class HapPairStore {
 public:
  explicit HapPairStore(const std::string& directory);
  void save(const HapPairRecord& record) const;
  std::optional<HapPairRecord> load(std::string_view identifier) const;

 private:
  std::string directory_;
};

}  // namespace headunit
