// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/pem.h>

#include <array>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace headunit {
namespace {
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using PrivateKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::array<std::uint8_t, 12> full_nonce(std::string_view nonce) {
  if (nonce.size() != 8) throw std::invalid_argument("HomeKit nonce must be 8 bytes");
  std::array<std::uint8_t, 12> result{};
  for (std::size_t i = 0; i < 8; ++i) {
    result[i + 4] = static_cast<std::uint8_t>(nonce[i]);
  }
  return result;
}
}  // namespace

HapKey32 hap_hkdf_sha512(std::span<const std::uint8_t> secret, std::string_view salt,
                         std::string_view info) {
  if (secret.empty() || salt.empty() || info.empty()) {
    throw std::invalid_argument("invalid HomeKit key derivation input");
  }
  KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  require(context && EVP_PKEY_derive_init(context.get()) == 1 &&
              EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha512()) == 1 &&
              EVP_PKEY_CTX_set1_hkdf_salt(context.get(),
                                          reinterpret_cast<const unsigned char*>(salt.data()),
                                          static_cast<int>(salt.size())) == 1 &&
              EVP_PKEY_CTX_set1_hkdf_key(context.get(), secret.data(),
                                         static_cast<int>(secret.size())) == 1 &&
              EVP_PKEY_CTX_add1_hkdf_info(context.get(),
                                          reinterpret_cast<const unsigned char*>(info.data()),
                                          static_cast<int>(info.size())) == 1,
          "HomeKit HKDF setup failed");
  HapKey32 key{};
  std::size_t size = key.size();
  require(EVP_PKEY_derive(context.get(), key.data(), &size) == 1 && size == key.size(),
          "HomeKit HKDF failed");
  return key;
}

std::vector<std::uint8_t> hap_encrypt(const HapKey32& key, std::string_view nonce,
                                      std::span<const std::uint8_t> plaintext,
                                      std::span<const std::uint8_t> aad) {
  if (plaintext.size() > 65536) {
    throw std::invalid_argument("HomeKit plaintext exceeds limit");
  }
  const auto iv = full_nonce(nonce);
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  require(context && EVP_EncryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, key.data(),
                                        iv.data()) == 1,
          "HomeKit encryption setup failed");
  int aad_written = 0;
  if (!aad.empty()) {
    require(EVP_EncryptUpdate(context.get(), nullptr, &aad_written, aad.data(),
                              static_cast<int>(aad.size())) == 1,
            "HomeKit encryption AAD failed");
  }
  std::vector<std::uint8_t> output(plaintext.size() + 16);
  int written = 0;
  int final = 0;
  require(EVP_EncryptUpdate(context.get(), output.data(), &written, plaintext.data(),
                            static_cast<int>(plaintext.size())) == 1 &&
              EVP_EncryptFinal_ex(context.get(), output.data() + written, &final) == 1 &&
              static_cast<std::size_t>(written + final) == plaintext.size() &&
              EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG, 16,
                                  output.data() + plaintext.size()) == 1,
          "HomeKit encryption failed");
  return output;
}

std::optional<std::vector<std::uint8_t>> hap_decrypt(
    const HapKey32& key, std::string_view nonce, std::span<const std::uint8_t> ciphertext_and_tag,
    std::span<const std::uint8_t> aad) {
  if (ciphertext_and_tag.size() < 16 || ciphertext_and_tag.size() > 65552) {
    return std::nullopt;
  }
  const auto iv = full_nonce(nonce);
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  require(context && EVP_DecryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, key.data(),
                                        iv.data()) == 1,
          "HomeKit decryption setup failed");
  int aad_written = 0;
  if (!aad.empty()) {
    require(EVP_DecryptUpdate(context.get(), nullptr, &aad_written, aad.data(),
                              static_cast<int>(aad.size())) == 1,
            "HomeKit decryption AAD failed");
  }
  const auto payload_length = ciphertext_and_tag.size() - 16;
  std::vector<std::uint8_t> output(payload_length);
  int written = 0;
  int final = 0;
  require(EVP_DecryptUpdate(context.get(), output.data(), &written, ciphertext_and_tag.data(),
                            static_cast<int>(payload_length)) == 1 &&
              EVP_CIPHER_CTX_ctrl(
                  context.get(), EVP_CTRL_AEAD_SET_TAG, 16,
                  const_cast<std::uint8_t*>(ciphertext_and_tag.data() + payload_length)) == 1,
          "HomeKit decryption failed");
  if (EVP_DecryptFinal_ex(context.get(), output.data() + written, &final) != 1) {
    OPENSSL_cleanse(output.data(), output.size());
    return std::nullopt;
  }
  output.resize(static_cast<std::size_t>(written + final));
  return output;
}

bool hap_verify_ed25519(const HapKey32& public_key, std::span<const std::uint8_t> message,
                        std::span<const std::uint8_t> signature) {
  if (signature.size() != 64) return false;
  PrivateKey key(
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()),
      EVP_PKEY_free);
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  require(key && context &&
              EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1,
          "Ed25519 verification setup failed");
  return EVP_DigestVerify(context.get(), signature.data(), signature.size(), message.data(),
                          message.size()) == 1;
}

struct HapEd25519Identity::Impl {
  PrivateKey key{nullptr, EVP_PKEY_free};
  HapKey32 public_bytes{};

  explicit Impl(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error("cannot open private pairing key");
    struct stat stat_info{};
    if (fstat(fd, &stat_info) != 0 || !S_ISREG(stat_info.st_mode) ||
        (stat_info.st_mode & 077) != 0 || (stat_info.st_uid != getuid() && getuid() != 0)) {
      close(fd);
      throw std::runtime_error("pairing key must be a private regular file");
    }
    FILE* file = fdopen(fd, "r");
    if (!file) {
      close(fd);
      throw std::runtime_error("cannot read private pairing key");
    }
    key.reset(PEM_read_PrivateKey(file, nullptr, nullptr, nullptr));
    fclose(file);
    if (!key || EVP_PKEY_id(key.get()) != EVP_PKEY_ED25519) {
      throw std::runtime_error("pairing key is not Ed25519");
    }
    std::size_t length = public_bytes.size();
    require(EVP_PKEY_get_raw_public_key(key.get(), public_bytes.data(), &length) == 1 &&
                length == public_bytes.size(),
            "cannot extract pairing public key");
  }
};

HapEd25519Identity::HapEd25519Identity(const std::string& path)
    : impl_(std::make_unique<Impl>(path)) {}
HapEd25519Identity::~HapEd25519Identity() = default;
HapEd25519Identity::HapEd25519Identity(HapEd25519Identity&&) noexcept = default;
HapEd25519Identity& HapEd25519Identity::operator=(HapEd25519Identity&&) noexcept = default;
HapKey32 HapEd25519Identity::public_key() const {
  return impl_->public_bytes;
}
HapSignature HapEd25519Identity::sign(std::span<const std::uint8_t> message) const {
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  require(context &&
              EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, impl_->key.get()) == 1,
          "Ed25519 signing setup failed");
  HapSignature signature{};
  std::size_t length = signature.size();
  require(EVP_DigestSign(context.get(), signature.data(), &length, message.data(),
                         message.size()) == 1 &&
              length == signature.size(),
          "Ed25519 signing failed");
  return signature;
}

struct HapX25519Ephemeral::Impl {
  PrivateKey key{nullptr, EVP_PKEY_free};
  HapKey32 public_bytes{};

  Impl() {
    KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* generated = nullptr;
    require(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
                EVP_PKEY_keygen(context.get(), &generated) == 1,
            "X25519 key generation failed");
    key.reset(generated);
    std::size_t length = public_bytes.size();
    require(EVP_PKEY_get_raw_public_key(key.get(), public_bytes.data(), &length) == 1 &&
                length == public_bytes.size(),
            "X25519 public key unavailable");
  }

  std::optional<HapKey32> shared_secret(const HapKey32& peer_public) const {
    PrivateKey peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_public.data(),
                                                peer_public.size()),
                    EVP_PKEY_free);
    KeyContext context(EVP_PKEY_CTX_new(key.get(), nullptr), EVP_PKEY_CTX_free);
    if (!peer || !context || EVP_PKEY_derive_init(context.get()) != 1 ||
        EVP_PKEY_derive_set_peer(context.get(), peer.get()) != 1) {
      return std::nullopt;
    }
    HapKey32 shared{};
    std::size_t length = shared.size();
    if (EVP_PKEY_derive(context.get(), shared.data(), &length) != 1 || length != shared.size()) {
      OPENSSL_cleanse(shared.data(), shared.size());
      return std::nullopt;
    }
    std::uint8_t nonzero = 0;
    for (const auto byte : shared)
      nonzero |= byte;
    if (!nonzero) {
      OPENSSL_cleanse(shared.data(), shared.size());
      return std::nullopt;
    }
    return shared;
  }
};

HapX25519Ephemeral::HapX25519Ephemeral() : impl_(std::make_unique<Impl>()) {}
HapX25519Ephemeral::~HapX25519Ephemeral() = default;
HapX25519Ephemeral::HapX25519Ephemeral(HapX25519Ephemeral&&) noexcept = default;
HapX25519Ephemeral& HapX25519Ephemeral::operator=(HapX25519Ephemeral&&) noexcept = default;
HapKey32 HapX25519Ephemeral::public_key() const {
  return impl_->public_bytes;
}
std::optional<HapKey32> HapX25519Ephemeral::shared_secret(const HapKey32& peer_public) const {
  return impl_->shared_secret(peer_public);
}

}  // namespace headunit
