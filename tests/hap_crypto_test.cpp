// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_crypto.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() try {
  std::array<std::uint8_t, 64> secret{};
  for (std::size_t i = 0; i < secret.size(); ++i)
    secret[i] = i;
  const auto key =
      headunit::hap_hkdf_sha512(secret, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info");
  const std::array<std::uint8_t, 32> expected{0xb6, 0x33, 0x55, 0x36, 0x16, 0x2d, 0x66, 0x29,
                                              0xe4, 0xc0, 0xba, 0xde, 0x85, 0xf1, 0xb7, 0x12,
                                              0xc8, 0x5a, 0x36, 0x4b, 0xab, 0x0d, 0xed, 0xb2,
                                              0x50, 0x14, 0xcf, 0xb8, 0x14, 0x48, 0x92, 0x73};
  check(key == expected, "HomeKit HKDF-SHA512 vector mismatch");
  const std::vector<std::uint8_t> message{1, 2, 3, 4, 5};
  const auto encrypted = headunit::hap_encrypt(key, "PS-Msg05", message);
  check(encrypted.size() == message.size() + 16, "ChaCha20 tag missing");
  const auto decrypted = headunit::hap_decrypt(key, "PS-Msg05", encrypted);
  check(decrypted && *decrypted == message, "HomeKit message decrypt failed");
  auto tampered = encrypted;
  tampered.back() ^= 1;
  check(!headunit::hap_decrypt(key, "PS-Msg05", tampered),
        "invalid HomeKit authentication tag accepted");
  check(!headunit::hap_decrypt(key, "PS-Msg06", encrypted), "wrong HomeKit nonce accepted");
  const auto empty_encrypted = headunit::hap_encrypt(key, "PS-Msg05", {});
  const auto empty_decrypted = headunit::hap_decrypt(key, "PS-Msg05", empty_encrypted);
  check(empty_decrypted && empty_decrypted->empty(), "empty HomeKit message failed");

  using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
  using PrivateKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
  EVP_PKEY* generated = nullptr;
  check(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
            EVP_PKEY_keygen(context.get(), &generated) == 1,
        "test Ed25519 key generation failed");
  PrivateKey private_key(generated, EVP_PKEY_free);
  const std::string path_template = "/tmp/headunit-hap-key-XXXXXX";
  std::vector<char> path(path_template.begin(), path_template.end());
  path.push_back('\0');
  const int fd = mkstemp(path.data());
  check(fd >= 0, "test private key file failed");
  FILE* file = fdopen(fd, "w");
  check(file && PEM_write_PrivateKey(file, private_key.get(), nullptr, nullptr, 0, nullptr,
                                     nullptr) == 1,
        "test private key write failed");
  fclose(file);
  try {
    headunit::HapEd25519Identity identity(path.data());
    const auto signature = identity.sign(message);
    check(headunit::hap_verify_ed25519(identity.public_key(), message, signature),
          "Ed25519 signature rejected");
    auto changed = message;
    changed[0] ^= 1;
    check(!headunit::hap_verify_ed25519(identity.public_key(), changed, signature),
          "tampered Ed25519 message accepted");
  } catch (...) {
    unlink(path.data());
    throw;
  }
  unlink(path.data());
  headunit::HapX25519Ephemeral first;
  headunit::HapX25519Ephemeral second;
  const auto first_secret = first.shared_secret(second.public_key());
  const auto second_secret = second.shared_secret(first.public_key());
  check(first_secret && second_secret && *first_secret == *second_secret,
        "X25519 peers derived different shared secrets");
  check(!first.shared_secret({}), "X25519 accepted zero peer key");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
