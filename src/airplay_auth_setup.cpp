// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_auth_setup.hpp"

#include "headunit/hap_crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace headunit {
namespace {
struct CleanseOnExit {
  void* data;
  std::size_t size;
  ~CleanseOnExit() { OPENSSL_cleanse(data, size); }
};

template <std::size_t N>
std::array<std::uint8_t, N> digest(const EVP_MD* algorithm, std::string_view prefix,
                                   std::span<const std::uint8_t> first,
                                   std::span<const std::uint8_t> second = {}) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                  EVP_MD_CTX_free);
  std::array<std::uint8_t, N> result{};
  unsigned size = 0;
  if (!context || EVP_DigestInit_ex(context.get(), algorithm, nullptr) != 1 ||
      (!prefix.empty() && EVP_DigestUpdate(context.get(), prefix.data(), prefix.size()) != 1) ||
      EVP_DigestUpdate(context.get(), first.data(), first.size()) != 1 ||
      (!second.empty() && EVP_DigestUpdate(context.get(), second.data(), second.size()) != 1) ||
      EVP_DigestFinal_ex(context.get(), result.data(), &size) != 1 || size != N) {
    throw std::runtime_error("AirPlay authentication digest failed");
  }
  return result;
}

std::vector<std::uint8_t> encrypt_signature(std::span<const std::uint8_t> signature,
                                            std::span<const std::uint8_t> secret) {
  auto key_digest = digest<20>(EVP_sha1(), "AES-KEY", secret);
  auto iv_digest = digest<20>(EVP_sha1(), "AES-IV", secret);
  const CleanseOnExit clean_key{key_digest.data(), key_digest.size()};
  const CleanseOnExit clean_iv{iv_digest.data(), iv_digest.size()};
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(EVP_CIPHER_CTX_new(),
                                                                          EVP_CIPHER_CTX_free);
  std::vector<std::uint8_t> encrypted(signature.size());
  int written = 0;
  int final = 0;
  if (!context ||
      EVP_EncryptInit_ex(context.get(), EVP_aes_128_ctr(), nullptr, key_digest.data(),
                         iv_digest.data()) != 1 ||
      EVP_EncryptUpdate(context.get(), encrypted.data(), &written, signature.data(),
                        static_cast<int>(signature.size())) != 1 ||
      EVP_EncryptFinal_ex(context.get(), encrypted.data() + written, &final) != 1 ||
      static_cast<std::size_t>(written + final) != signature.size()) {
    throw std::runtime_error("AirPlay authentication encryption failed");
  }
  return encrypted;
}

void append_length(std::vector<std::uint8_t>& bytes, std::size_t length) {
  if (length > 65535) throw std::invalid_argument("AirPlay authentication field too large");
  bytes.push_back(0);
  bytes.push_back(0);
  bytes.push_back(static_cast<std::uint8_t>(length >> 8));
  bytes.push_back(static_cast<std::uint8_t>(length));
}
}  // namespace

std::vector<std::uint8_t> AirplayAuthSetup::respond(std::span<const std::uint8_t> request) {
  if (request.size() != 33 || request[0] != 1) {
    throw std::invalid_argument("invalid AirPlay authentication request");
  }
  HapKey32 controller_public{};
  std::copy(request.begin() + 1, request.end(), controller_public.begin());
  HapX25519Ephemeral ephemeral;
  auto shared = ephemeral.shared_secret(controller_public);
  if (!shared) throw std::invalid_argument("invalid AirPlay authentication key");
  const CleanseOnExit clean_shared{shared->data(), shared->size()};
  const auto accessory_public = ephemeral.public_key();
  if (auth_.protocol_major() != 3) {
    throw std::invalid_argument("AirPlay authentication requires protocol-3 MFi provider");
  }
  auto challenge = digest<32>(EVP_sha256(), {}, accessory_public, controller_public);
  const CleanseOnExit clean_challenge{challenge.data(), challenge.size()};
  const auto signature = auth_.sign_digest(challenge);
  const auto certificate = auth_.read_certificate();
  const auto encrypted = encrypt_signature(signature, *shared);

  std::vector<std::uint8_t> response(accessory_public.begin(), accessory_public.end());
  append_length(response, certificate.size());
  response.insert(response.end(), certificate.begin(), certificate.end());
  append_length(response, encrypted.size());
  response.insert(response.end(), encrypted.begin(), encrypted.end());
  return response;
}

}  // namespace headunit
