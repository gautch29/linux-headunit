// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace headunit {

// SRP-6a server for HomeKit's 3072-bit group and SHA-512. This is only the
// proof exchange; pairing policy, identity signing, and session encryption
// belong to higher layers.
class SrpServer {
 public:
  static constexpr std::size_t public_key_size = 384;
  static constexpr std::size_t proof_size = 64;

  struct Challenge {
    std::array<std::uint8_t, 16> salt{};
    std::array<std::uint8_t, public_key_size> public_key{};
  };

  explicit SrpServer(std::string_view password);
  ~SrpServer();
  SrpServer(SrpServer&&) noexcept;
  SrpServer& operator=(SrpServer&&) noexcept;
  SrpServer(const SrpServer&) = delete;
  SrpServer& operator=(const SrpServer&) = delete;

  const Challenge& challenge() const;
  // Returns the accessory proof only when the controller proof is valid.
  // A failed proof consumes this one-time server instance.
  std::optional<std::array<std::uint8_t, proof_size>> verify(
      std::span<const std::uint8_t> client_public_key, std::span<const std::uint8_t> client_proof);
  std::optional<std::array<std::uint8_t, proof_size>> session_key() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace headunit
