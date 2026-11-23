// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace headunit {

using HapKey32 = std::array<std::uint8_t, 32>;
using HapSignature = std::array<std::uint8_t, 64>;

HapKey32 hap_hkdf_sha512(std::span<const std::uint8_t> secret, std::string_view salt,
                         std::string_view info);

// HomeKit's eight-byte ASCII nonce is left-padded with four zero bytes.
std::vector<std::uint8_t> hap_encrypt(const HapKey32& key, std::string_view nonce,
                                      std::span<const std::uint8_t> plaintext,
                                      std::span<const std::uint8_t> aad = {});
std::optional<std::vector<std::uint8_t>> hap_decrypt(
    const HapKey32& key, std::string_view nonce, std::span<const std::uint8_t> ciphertext_and_tag,
    std::span<const std::uint8_t> aad = {});

bool hap_verify_ed25519(const HapKey32& public_key, std::span<const std::uint8_t> message,
                        std::span<const std::uint8_t> signature);

class HapEd25519Identity {
 public:
  explicit HapEd25519Identity(const std::string& private_pem_path);
  ~HapEd25519Identity();
  HapEd25519Identity(HapEd25519Identity&&) noexcept;
  HapEd25519Identity& operator=(HapEd25519Identity&&) noexcept;
  HapEd25519Identity(const HapEd25519Identity&) = delete;
  HapEd25519Identity& operator=(const HapEd25519Identity&) = delete;

  HapKey32 public_key() const;
  HapSignature sign(std::span<const std::uint8_t> message) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class HapX25519Ephemeral {
 public:
  HapX25519Ephemeral();
  ~HapX25519Ephemeral();
  HapX25519Ephemeral(HapX25519Ephemeral&&) noexcept;
  HapX25519Ephemeral& operator=(HapX25519Ephemeral&&) noexcept;
  HapX25519Ephemeral(const HapX25519Ephemeral&) = delete;
  HapX25519Ephemeral& operator=(const HapX25519Ephemeral&) = delete;

  HapKey32 public_key() const;
  std::optional<HapKey32> shared_secret(const HapKey32& peer_public) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace headunit
