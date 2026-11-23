// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/software_mfi_authenticator.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit {
namespace {
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool same_dates(const struct stat& before, const struct stat& after) {
#ifdef __APPLE__
  return before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
         before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
         before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
         before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
  return before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
}

struct PrivateBytes {
  std::vector<std::uint8_t> value;
  explicit PrivateBytes(std::size_t size) : value(size) {}
  PrivateBytes(PrivateBytes&&) = default;
  ~PrivateBytes() { OPENSSL_cleanse(value.data(), value.size()); }
};

// Reject links, devices, permissive files and replacement during a bounded read.
PrivateBytes read_identity_file(const std::string& path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  require(!error && canonical == std::filesystem::path(path),
          "Software MFi inputs must be real canonical absolute files");
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  require(fd >= 0, "Cannot open software MFi input");
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } close_file{fd};
  struct stat before{}, after{};
  require(fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_uid == geteuid() &&
              (before.st_mode & 0777) == 0600 && before.st_nlink == 1 && before.st_size > 0 &&
              before.st_size <= 16384,
          "Software MFi inputs must be owned 0600 regular files within 16 KiB");
  PrivateBytes bytes(static_cast<std::size_t>(before.st_size));
  std::size_t offset = 0;
  while (offset < bytes.value.size()) {
    const auto count = read(fd, bytes.value.data() + offset, bytes.value.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    require(count > 0, "Software MFi input read failed");
    offset += static_cast<std::size_t>(count);
  }
  std::uint8_t extra{};
  ssize_t count;
  do {
    count = read(fd, &extra, 1);
  } while (count < 0 && errno == EINTR);
  require(count == 0 && fstat(fd, &after) == 0 && before.st_dev == after.st_dev &&
              before.st_ino == after.st_ino && before.st_size == after.st_size &&
              before.st_mode == after.st_mode && before.st_uid == after.st_uid &&
              after.st_nlink == 1 && same_dates(before, after),
          "Software MFi input changed while loading");
  return bytes;
}

Certificate parse_certificate(std::span<const std::uint8_t> bytes) {
  const auto* cursor = bytes.data();
  Certificate direct(d2i_X509(nullptr, &cursor, static_cast<long>(bytes.size())), X509_free);
  if (direct && cursor == bytes.data() + bytes.size()) return direct;
  ERR_clear_error();
  cursor = bytes.data();
  std::unique_ptr<PKCS7, decltype(&PKCS7_free)> container(
      d2i_PKCS7(nullptr, &cursor, static_cast<long>(bytes.size())), PKCS7_free);
  require(container && cursor == bytes.data() + bytes.size() &&
              PKCS7_type_is_signed(container.get()) && container->d.sign &&
              sk_X509_num(container->d.sign->cert) == 1,
          "Software MFi needs one DER X.509 certificate or PKCS7 certificate");
  Certificate certificate(X509_dup(sk_X509_value(container->d.sign->cert, 0)), X509_free);
  require(bool(certificate), "Cannot load software MFi certificate");
  return certificate;
}

bool p256(EVP_PKEY* key) {
  std::array<char, 80> group{};
  std::size_t size = 0;
  return key && EVP_PKEY_is_a(key, "EC") == 1 &&
         EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group.data(), group.size(),
                                        &size) == 1 &&
         (std::string_view(group.data()) == "prime256v1" ||
          std::string_view(group.data()) == "P-256");
}
}  // namespace

struct SoftwareMfiAuthenticator::Impl {
  Key key{nullptr, EVP_PKEY_free};
  std::vector<std::uint8_t> certificate;

  Impl(const std::string& key_path, const std::string& certificate_path) {
    auto encoded_key = read_identity_file(key_path);
    const auto* cursor = encoded_key.value.data();
    std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)> info(
        d2i_PKCS8_PRIV_KEY_INFO(nullptr, &cursor, static_cast<long>(encoded_key.value.size())),
        PKCS8_PRIV_KEY_INFO_free);
    require(info && cursor == encoded_key.value.data() + encoded_key.value.size(),
            "Software MFi private key must be unencrypted DER PKCS8");
    key.reset(EVP_PKCS82PKEY(info.get()));
    require(p256(key.get()), "Software MFi requires a P-256 private key");
    auto encoded_certificate = read_identity_file(certificate_path);
    auto parsed = parse_certificate(encoded_certificate.value);
    Key public_key(X509_get_pubkey(parsed.get()), EVP_PKEY_free);
    KeyContext check(EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
    require(p256(public_key.get()) && EVP_PKEY_eq(key.get(), public_key.get()) == 1 && check &&
                EVP_PKEY_pairwise_check(check.get()) == 1,
            "Software MFi private key does not match its P-256 certificate");
    certificate = encoded_certificate.value;
  }
};

SoftwareMfiAuthenticator::SoftwareMfiAuthenticator(const std::string& private_key_path,
                                                   const std::string& certificate_path)
    : impl_(std::make_unique<Impl>(private_key_path, certificate_path)) {}
SoftwareMfiAuthenticator::~SoftwareMfiAuthenticator() = default;

std::vector<std::uint8_t> SoftwareMfiAuthenticator::read_certificate() {
  return impl_->certificate;
}

std::array<std::uint8_t, 64> SoftwareMfiAuthenticator::sign_digest(
    std::span<const std::uint8_t, 32> digest, std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(2)) {
    throw std::invalid_argument("MFi signature deadline must be within two seconds");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto check_deadline = [&] {
    require(std::chrono::steady_clock::now() < deadline, "Software MFi signature timed out");
  };
  check_deadline();
  KeyContext signer(EVP_PKEY_CTX_new(impl_->key.get(), nullptr), EVP_PKEY_CTX_free);
  // EVP_PKEY_sign consumes the protocol's digest directly; DigestSign would hash again.
  require(signer && EVP_PKEY_sign_init(signer.get()) == 1 &&
              EVP_PKEY_CTX_set_signature_md(signer.get(), EVP_sha256()) == 1,
          "Software MFi signer initialization failed");
  std::array<std::uint8_t, 80> der{};
  std::size_t size = der.size();
  check_deadline();
  require(EVP_PKEY_sign(signer.get(), der.data(), &size, digest.data(), digest.size()) == 1,
          "Software MFi signing failed");
  check_deadline();
  const auto* cursor = der.data();
  std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> signature(
      d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(size)), ECDSA_SIG_free);
  require(signature && cursor == der.data() + size, "Software MFi returned invalid ECDSA");
  const BIGNUM *r = nullptr, *s = nullptr;
  ECDSA_SIG_get0(signature.get(), &r, &s);
  std::array<std::uint8_t, 64> raw{};
  require(r && s && !BN_is_negative(r) && !BN_is_negative(s) && !BN_is_zero(r) && !BN_is_zero(s) &&
              BN_bn2binpad(r, raw.data(), 32) == 32 && BN_bn2binpad(s, raw.data() + 32, 32) == 32,
          "Software MFi signature is not P-256");
  check_deadline();
  return raw;
}

}  // namespace headunit
