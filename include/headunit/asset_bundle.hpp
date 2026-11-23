// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/asset_signature.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace headunit {
struct AssetBundlePolicy {
  std::string identifier;
  std::uint64_t version = 0;
  std::optional<AcceptedAssetReference> previously_accepted;
  AssetCertificateTimePolicy certificate_time_policy = AssetCertificateTimePolicy::strict;
};
AcceptedAssetReference read_asset_reference(const std::string& path);

struct AssetBundleInfo {
  std::string identifier, minimum_sdk;
  std::uint64_t version = 0;
  std::size_t files = 0, expanded_bytes = 0;
  bool expired_signer_exception = false;
};
// Immutable verified resource snapshot. No extracted filesystem content is trusted.
// Reading verifies trust, all archive paths/limits, and signed Info.plist.
// Retained resources share the existing 256 MiB expanded-archive limit. Resource
// reads never reopen or rescan the archive on the script owner thread.
// This is resource access only: it does not execute a theme or claim SDK support.
class AssetBundle {
 public:
  static AssetBundle open(const std::string& archive_path, const std::string& signature_path,
                          const std::string& certificate_path, const std::string& root_path,
                          const AssetBundlePolicy& policy);
  const AssetBundleInfo& info() const { return info_; }
  std::vector<std::uint8_t> resource(const std::string& name) const;

 private:
  std::map<std::string, std::vector<std::uint8_t>, std::less<>> resources_;
  AssetBundleInfo info_;
};
}  // namespace headunit
