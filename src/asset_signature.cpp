// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/asset_signature.hpp"

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cctype>
#include <memory>
#include <stdexcept>
#include <vector>

namespace headunit {
namespace {
using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
std::vector<Cert> certificates(std::string_view pem, std::size_t maximum) {
  if (pem.empty() || pem.size() > 32768) throw std::invalid_argument("invalid certificate size");
  constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
  constexpr std::string_view end = "-----END CERTIFICATE-----";
  std::vector<Cert> result;
  while (!pem.empty()) {
    while (!pem.empty() && std::isspace(static_cast<unsigned char>(pem.front())))
      pem.remove_prefix(1);
    if (pem.empty()) break;
    if (!pem.starts_with(begin) || result.size() >= maximum)
      throw std::invalid_argument("invalid certificate chain");
    const auto finish = pem.find(end, begin.size());
    if (finish == std::string_view::npos) throw std::invalid_argument("incomplete certificate");
    const auto length = finish + end.size();
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(length)), BIO_free);
    if (!bio) throw std::runtime_error("cannot allocate certificate reader");
    Cert cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
    if (!cert) throw std::invalid_argument("invalid PEM certificate");
    result.push_back(std::move(cert));
    pem.remove_prefix(length);
  }
  if (result.empty()) throw std::invalid_argument("empty chain");
  return result;
}
struct CertStack {
  STACK_OF(X509) * value = sk_X509_new_null();
  ~CertStack() { sk_X509_free(value); }
};
}  // namespace
static AssetSignatureResult verify_signature_at(
    const std::array<std::uint8_t, 32>& digest, std::span<const std::uint8_t> signature,
    std::string_view chain_pem, std::string_view root_pem, std::time_t at,
    AssetCertificateTimePolicy time_policy = AssetCertificateTimePolicy::strict) {
  using Result = AssetSignatureResult;
  if (time_policy != AssetCertificateTimePolicy::strict &&
      time_policy != AssetCertificateTimePolicy::allow_expired_leaf)
    return Result::malformed;
  if (signature.empty() || signature.size() > 4096) return Result::malformed;
  try {
    auto chain = certificates(chain_pem, 8);
    auto roots = certificates(root_pem, 1);
    if (X509_check_ca(roots.front().get()) <= 0) return Result::malformed;
    auto* leaf = chain.front().get();
    std::unique_ptr<ASN1_BIT_STRING, decltype(&ASN1_BIT_STRING_free)> usage(
        static_cast<ASN1_BIT_STRING*>(X509_get_ext_d2i(leaf, NID_key_usage, nullptr, nullptr)),
        ASN1_BIT_STRING_free);
    if (X509_check_ca(leaf) != 0 || !usage || ASN1_BIT_STRING_get_bit(usage.get(), 0) != 1)
      return Result::invalid_signer_usage;
    auto* key = X509_get0_pubkey(leaf);
    if (!key || EVP_PKEY_is_a(key, "EC") != 1 || EVP_PKEY_get_bits(key) < 256)
      return Result::invalid_signer_usage;
    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(),
                                                                  X509_STORE_free);
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> context(X509_STORE_CTX_new(),
                                                                            X509_STORE_CTX_free);
    CertStack intermediates;
    if (!store || !context || !intermediates.value)
      throw std::runtime_error("certificate allocation failed");
    if (X509_STORE_add_cert(store.get(), roots.front().get()) != 1) return Result::malformed;
    for (std::size_t i = 1; i < chain.size(); ++i)
      if (!sk_X509_push(intermediates.value, chain[i].get()))
        throw std::runtime_error("chain allocation failed");
    if (X509_STORE_CTX_init(context.get(), store.get(), leaf, intermediates.value) != 1)
      return Result::malformed;
    if (at) X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(context.get()), at);
    X509_VERIFY_PARAM_set_depth(X509_STORE_CTX_get0_param(context.get()), 7);
    if (time_policy == AssetCertificateTimePolicy::allow_expired_leaf) {
      if (at) return Result::malformed;  // Never weaken a historical receipt.
      X509_STORE_CTX_set_verify_cb(context.get(), [](int ok, X509_STORE_CTX* ctx) {
        if (ok) return 1;
        const auto* current = X509_STORE_CTX_get_current_cert(ctx);
        // Do not use NO_CHECK_TIME: an expired issuer or a future signing leaf
        // must still fail. Also reject malformed/future not-before explicitly.
        if (!current || X509_STORE_CTX_get_error_depth(ctx) != 0 ||
            X509_STORE_CTX_get_error(ctx) != X509_V_ERR_CERT_HAS_EXPIRED)
          return 0;
        const auto now = std::time(nullptr);
        if (now == std::time_t{-1}) return 0;
        // ASN1_TIME_cmp_time_t is supported by both OpenSSL 3 and 4. Unlike
        // X509_cmp_current_time, it distinguishes equality (0) from error (-2).
        const auto comparison = ASN1_TIME_cmp_time_t(X509_get0_notBefore(current), now);
        return comparison == -1 || comparison == 0 ? 1 : 0;
      });
    }
    if (X509_verify_cert(context.get()) != 1) {
      const auto error = X509_STORE_CTX_get_error(context.get());
      if (error == X509_V_ERR_CERT_HAS_EXPIRED || error == X509_V_ERR_CERT_NOT_YET_VALID)
        return Result::invalid_certificate_time;
      return Result::untrusted_chain;
    }
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> verifier(
        EVP_PKEY_CTX_new(key, nullptr), EVP_PKEY_CTX_free);
    if (!verifier || EVP_PKEY_verify_init(verifier.get()) != 1 ||
        EVP_PKEY_CTX_set_signature_md(verifier.get(), EVP_sha256()) != 1)
      return Result::invalid_signature;
    return EVP_PKEY_verify(verifier.get(), signature.data(), signature.size(), digest.data(),
                           digest.size()) == 1
               ? Result::valid
               : Result::invalid_signature;
  } catch (const std::exception&) {
    return Result::malformed;
  }
}
AssetSignatureResult verify_asset_signature(const std::array<std::uint8_t, 32>& digest,
                                            std::span<const std::uint8_t> signature,
                                            std::string_view chain, std::string_view root,
                                            AssetCertificateTimePolicy time_policy) {
  return verify_signature_at(digest, signature, chain, root, 0, time_policy);
}
AssetSignatureResult verify_previously_accepted_asset(const std::array<std::uint8_t, 32>& digest,
                                                      std::span<const std::uint8_t> signature,
                                                      std::string_view chain, std::string_view root,
                                                      const AcceptedAssetReference& reference) {
  using R = AssetSignatureResult;
  if (reference.accepted_at <= 0 || reference.accepted_at > std::time(nullptr) ||
      CRYPTO_memcmp(digest.data(), reference.archive_digest.data(), digest.size()))
    return R::malformed;
  const auto matches = [](const void* bytes, std::size_t length,
                          const std::array<std::uint8_t, 32>& expected) {
    std::array<std::uint8_t, 32> hash{};
    unsigned count = 0;
    return EVP_Digest(bytes, length, hash.data(), &count, EVP_sha256(), nullptr) == 1 &&
           count == hash.size() && CRYPTO_memcmp(hash.data(), expected.data(), hash.size()) == 0;
  };
  if (!matches(signature.data(), signature.size(), reference.signature_digest) ||
      !matches(chain.data(), chain.size(), reference.certificate_digest))
    return R::malformed;
  const auto current = verify_asset_signature(digest, signature, chain, root);
  if (current != R::valid && current != R::invalid_certificate_time) return current;
  return verify_signature_at(digest, signature, chain, root, reference.accepted_at);
}
}  // namespace headunit
