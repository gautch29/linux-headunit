// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <ctime>

namespace headunit {
enum class AssetSignatureResult {
  valid,
  malformed,
  untrusted_chain,
  invalid_certificate_time,
  invalid_signer_usage,
  invalid_signature
};
// Explicit local development policy. Only the signing leaf's expiry may be
// relaxed; trusted roots/intermediates, not-before dates and signatures remain
// mandatory. Historical acceptance references always use strict verification.
enum class AssetCertificateTimePolicy { strict, allow_expired_leaf };
// PEM leaf-first chain and DER ECDSA signature over an archive's SHA-256.
// trusted_root_pem is a separate local trust decision, never the supplied chain.
// Uses current wall time and defaults to strict expiry; no system CA stores.
AssetSignatureResult verify_asset_signature(
    const std::array<std::uint8_t, 32>& digest, std::span<const std::uint8_t> signature_der,
    std::string_view chain_pem, std::string_view trusted_root_pem,
    AssetCertificateTimePolicy time_policy = AssetCertificateTimePolicy::strict);
// Host-attested receipt for an exact bundle accepted earlier. This is not an
// Apple-signed timestamp. Only read-only reopening may use this policy.
struct AcceptedAssetReference {
  std::array<std::uint8_t, 32> archive_digest{}, certificate_digest{}, signature_digest{};
  std::time_t accepted_at = 0;
};
AssetSignatureResult verify_previously_accepted_asset(const std::array<std::uint8_t, 32>& digest,
                                                      std::span<const std::uint8_t> signature_der,
                                                      std::string_view chain_pem,
                                                      std::string_view trusted_root_pem,
                                                      const AcceptedAssetReference& reference);
}  // namespace headunit
