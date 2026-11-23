// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_iap_stream.hpp"

#include <openssl/crypto.h>

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace headunit {
namespace {
constexpr std::size_t max_record = 16384;
constexpr std::size_t max_wire_buffer = 65536;
constexpr std::size_t package_header = 32;
constexpr std::size_t max_package = 4 * 1024 * 1024;
constexpr std::uint32_t command_message = 0x636f6d6d;

std::uint32_t big32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

struct Wipe {
  std::vector<std::uint8_t>& bytes;
  ~Wipe() {
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
  }
};

std::string nonce_bytes(std::uint64_t counter) {
  std::string result(8, '\0');
  for (unsigned index = 0; index < 8; ++index) {
    result[index] = static_cast<char>(counter >> (index * 8));
  }
  return result;
}
}  // namespace

AirplayIapStream::AirplayIapStream(std::span<const std::uint8_t> pair_secret, std::uint64_t seed)
    : key_(hap_hkdf_sha512(pair_secret, "DataStream-Salt" + std::to_string(seed),
                           "DataStream-Output-Encryption-Key")) {}

AirplayIapStream::~AirplayIapStream() {
  fail();
}

void AirplayIapStream::retire() noexcept {
  fail();
}

void AirplayIapStream::fail() noexcept {
  failed_ = true;
  OPENSSL_cleanse(key_.data(), key_.size());
  if (!plain_.empty()) OPENSSL_cleanse(plain_.data(), plain_.size());
  std::vector<std::uint8_t>().swap(plain_);
  if (!encrypted_.empty()) OPENSSL_cleanse(encrypted_.data(), encrypted_.size());
  std::vector<std::uint8_t>().swap(encrypted_);
}

void AirplayIapStream::packages(std::vector<std::vector<std::uint8_t>>& messages) {
  std::size_t consumed = 0;
  while (plain_.size() - consumed >= package_header) {
    const auto* package = plain_.data() + consumed;
    const auto length = big32(package);
    if (length < package_header || length > max_package) {
      throw std::invalid_argument("invalid iAP package length");
    }
    if (plain_.size() - consumed < length) break;
    if (big32(package + 16) == command_message) {
      messages.emplace_back(package + package_header, package + length);
      ++messages_;
      message_bytes_ += length - package_header;
    }
    consumed += length;
  }
  if (consumed) {
    const auto remaining = plain_.size() - consumed;
    std::memmove(plain_.data(), plain_.data() + consumed, remaining);
    OPENSSL_cleanse(plain_.data() + remaining, consumed);
    plain_.resize(remaining);
  }
}

std::vector<std::vector<std::uint8_t>> AirplayIapStream::feed(std::span<const std::uint8_t> bytes) {
  if (failed_) throw std::invalid_argument("iAP stream is closed after a fault");
  try {
    // The prior fragment and this read have independent bounds. A read can
    // complete the retained record and contain subsequent records.
    if (encrypted_.size() > max_record + 18 || bytes.size() > max_wire_buffer) {
      throw std::invalid_argument("iAP encrypted record limit exceeded");
    }
    encrypted_.insert(encrypted_.end(), bytes.begin(), bytes.end());
    std::vector<std::vector<std::uint8_t>> messages;
    std::size_t consumed = 0;
    while (encrypted_.size() - consumed >= 2) {
      const auto length = static_cast<std::size_t>(encrypted_[consumed]) |
                          (static_cast<std::size_t>(encrypted_[consumed + 1]) << 8);
      if (length > max_record) throw std::invalid_argument("oversized iAP record");
      const auto total = length + 18;
      if (encrypted_.size() - consumed < total) break;
      if (nonce_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("iAP record nonce exhausted");
      }
      const auto aad = std::span<const std::uint8_t>(encrypted_.data() + consumed, 2);
      const auto sealed =
          std::span<const std::uint8_t>(encrypted_.data() + consumed + 2, length + 16);
      auto clear = hap_decrypt(key_, nonce_bytes(nonce_), sealed, aad);
      if (!clear) throw std::invalid_argument("iAP record authentication failed");
      Wipe wipe{*clear};
      ++nonce_;
      if (plain_.size() > max_package + max_record - clear->size()) {
        throw std::invalid_argument("iAP package buffer limit exceeded");
      }
      plain_.insert(plain_.end(), clear->begin(), clear->end());
      // Drain completed packages before another authenticated record arrives.
      packages(messages);
      consumed += total;
    }
    encrypted_.erase(encrypted_.begin(),
                     encrypted_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return messages;
  } catch (...) {
    fail();
    throw;
  }
}
}  // namespace headunit
