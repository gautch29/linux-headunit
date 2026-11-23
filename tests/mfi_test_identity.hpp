// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mfi_test {
inline void require(bool value) {
  if (!value) throw std::runtime_error("synthetic MFi fixture failed");
}
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

inline void write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  stream.close();
  require(bool(stream) && chmod(path.c_str(), 0600) == 0);
}

struct Identity {
  std::filesystem::path directory;
  Key key{nullptr, EVP_PKEY_free};
  std::vector<std::uint8_t> certificate, container, encoded_key;
  explicit Identity(int curve = NID_X9_62_prime256v1) {
    std::array<char, 48> pattern{};
    const std::string prefix = "/tmp/headunit-synthetic-mfi-XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    require(mkdtemp(pattern.data()) != nullptr);
    directory = std::filesystem::canonical(pattern.data());
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* generated = nullptr;
    require(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), curve) == 1 &&
            EVP_PKEY_keygen(context.get(), &generated) == 1);
    key.reset(generated);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    require(cert && X509_set_version(cert.get(), 2) == 1 &&
            ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) == 1 &&
            X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) &&
            X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) &&
            X509_set_pubkey(cert.get(), key.get()) == 1);
    std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> name(X509_NAME_new(), X509_NAME_free);
    require(name &&
            X509_NAME_add_entry_by_txt(
                name.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("Synthetic MFi test only"), -1, -1,
                0) == 1 &&
            X509_set_subject_name(cert.get(), name.get()) == 1 &&
            X509_set_issuer_name(cert.get(), name.get()) == 1 &&
            X509_sign(cert.get(), key.get(), EVP_sha256()) > 0);
    certificate.resize(static_cast<std::size_t>(i2d_X509(cert.get(), nullptr)));
    auto* out = certificate.data();
    require(i2d_X509(cert.get(), &out) > 0);
    std::unique_ptr<PKCS7, decltype(&PKCS7_free)> p7(PKCS7_new(), PKCS7_free);
    require(p7 && PKCS7_set_type(p7.get(), NID_pkcs7_signed) == 1 &&
            PKCS7_content_new(p7.get(), NID_pkcs7_data) == 1 &&
            PKCS7_add_certificate(p7.get(), cert.get()) == 1);
    container.resize(static_cast<std::size_t>(i2d_PKCS7(p7.get(), nullptr)));
    out = container.data();
    require(i2d_PKCS7(p7.get(), &out) > 0);
    std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)> info(
        EVP_PKEY2PKCS8(key.get()), PKCS8_PRIV_KEY_INFO_free);
    require(bool(info));
    encoded_key.resize(static_cast<std::size_t>(i2d_PKCS8_PRIV_KEY_INFO(info.get(), nullptr)));
    out = encoded_key.data();
    require(i2d_PKCS8_PRIV_KEY_INFO(info.get(), &out) > 0);
    write(directory / "identity.pk8", encoded_key);
    write(directory / "certificate.p7b", container);
    write(directory / "certificate.der", certificate);
  }
  ~Identity() {
    OPENSSL_cleanse(encoded_key.data(), encoded_key.size());
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  Identity(const Identity&) = delete;
  Identity& operator=(const Identity&) = delete;

  bool verify(std::span<const std::uint8_t> digest, std::span<const std::uint8_t> raw) const {
    require(raw.size() == 64);
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> signature(ECDSA_SIG_new(),
                                                                    ECDSA_SIG_free);
    require(signature && ECDSA_SIG_set0(signature.get(), BN_bin2bn(raw.data(), 32, nullptr),
                                        BN_bin2bn(raw.data() + 32, 32, nullptr)) == 1);
    std::vector<std::uint8_t> der(
        static_cast<std::size_t>(i2d_ECDSA_SIG(signature.get(), nullptr)));
    auto* out = der.data();
    require(i2d_ECDSA_SIG(signature.get(), &out) > 0);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> verifier(
        EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
    require(verifier && EVP_PKEY_verify_init(verifier.get()) == 1 &&
            EVP_PKEY_CTX_set_signature_md(verifier.get(), EVP_sha256()) == 1);
    return EVP_PKEY_verify(verifier.get(), der.data(), der.size(), digest.data(), digest.size()) ==
           1;
  }
};
}  // namespace mfi_test
