// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_control_frame.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace headunit {
namespace {
constexpr std::size_t frame_plaintext_max = 1024;
constexpr std::size_t buffer_max = 65536;

struct Wipe {
  std::vector<std::uint8_t>& bytes;
  ~Wipe() {
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
  }
};

std::string_view nonce_for(std::uint64_t count, std::array<char, 8>& bytes) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>(count >> (8 * i));
  }
  return {bytes.data(), bytes.size()};
}
}  // namespace

HapControlFrame::HapControlFrame(std::span<const std::uint8_t> shared_secret,
                                 HapFrameChannel channel)
    : read_key_(hap_hkdf_sha512(
          shared_secret, channel == HapFrameChannel::events ? "Events-Salt" : "Control-Salt",
          channel == HapFrameChannel::events ? "Events-Read-Encryption-Key"
                                             : "Control-Write-Encryption-Key")),
      write_key_(hap_hkdf_sha512(
          shared_secret, channel == HapFrameChannel::events ? "Events-Salt" : "Control-Salt",
          channel == HapFrameChannel::events ? "Events-Write-Encryption-Key"
                                             : "Control-Read-Encryption-Key")) {}

HapControlFrame::~HapControlFrame() {
  fail();
}

void HapControlFrame::fail() noexcept {
  failed_ = true;
  std::vector<std::uint8_t>().swap(buffered_);
  OPENSSL_cleanse(read_key_.data(), read_key_.size());
  OPENSSL_cleanse(write_key_.data(), write_key_.size());
}

std::size_t HapControlFrame::encoded_size(std::size_t plaintext_bytes) {
  if (plaintext_bytes > buffer_max) {
    throw std::invalid_argument("invalid HomeKit control plaintext size");
  }
  return plaintext_bytes + ((plaintext_bytes + frame_plaintext_max - 1) / frame_plaintext_max) * 18;
}

std::vector<std::uint8_t> HapControlFrame::encode(std::span<const std::uint8_t> plaintext) {
  if (failed_ || plaintext.size() > buffer_max) {
    throw std::invalid_argument("invalid HomeKit control plaintext");
  }
  try {
    std::vector<std::uint8_t> framed;
    framed.reserve(encoded_size(plaintext.size()));
    for (std::size_t offset = 0; offset < plaintext.size();) {
      if (write_count_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("HomeKit control nonce exhausted");
      }
      const auto length = std::min(frame_plaintext_max, plaintext.size() - offset);
      const std::array<std::uint8_t, 2> length_le{static_cast<std::uint8_t>(length),
                                                  static_cast<std::uint8_t>(length >> 8)};
      std::array<char, 8> nonce{};
      const auto ciphertext = hap_encrypt(write_key_, nonce_for(write_count_, nonce),
                                          plaintext.subspan(offset, length), length_le);
      framed.insert(framed.end(), length_le.begin(), length_le.end());
      framed.insert(framed.end(), ciphertext.begin(), ciphertext.end());
      ++write_count_;
      offset += length;
    }
    return framed;
  } catch (...) {
    fail();
    throw;
  }
}

std::vector<std::uint8_t> HapControlFrame::receive(std::span<const std::uint8_t> encrypted) {
  if (failed_) throw std::invalid_argument("HomeKit framing is closed after a fault");
  std::vector<std::uint8_t> plaintext;
  try {
    if (encrypted.size() > buffer_max || buffered_.size() > frame_plaintext_max + 18) {
      throw std::invalid_argument("invalid HomeKit control frame buffer");
    }
    buffered_.insert(buffered_.end(), encrypted.begin(), encrypted.end());
    std::size_t consumed = 0;
    while (buffered_.size() - consumed >= 2) {
      const auto length = static_cast<std::size_t>(buffered_[consumed]) |
                          (static_cast<std::size_t>(buffered_[consumed + 1]) << 8);
      if (length > frame_plaintext_max ||
          read_count_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("invalid HomeKit control frame length or nonce");
      }
      const auto total = 2 + length + 16;
      if (buffered_.size() - consumed < total) break;
      const std::array<std::uint8_t, 2> length_le{buffered_[consumed], buffered_[consumed + 1]};
      std::array<char, 8> nonce{};
      auto result = hap_decrypt(read_key_, nonce_for(read_count_, nonce),
                                {buffered_.data() + consumed + 2, length + 16}, length_le);
      if (!result) throw std::invalid_argument("HomeKit control authentication failed");
      Wipe wipe{*result};
      plaintext.insert(plaintext.end(), result->begin(), result->end());
      ++read_count_;
      consumed += total;
    }
    buffered_.erase(buffered_.begin(), buffered_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return plaintext;
  } catch (...) {
    if (!plaintext.empty()) OPENSSL_cleanse(plaintext.data(), plaintext.size());
    fail();
    throw;
  }
}

}  // namespace headunit
