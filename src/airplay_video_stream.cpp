// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_video_stream.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace headunit {
namespace {
constexpr std::size_t header_size = 128;
constexpr std::size_t max_body = 8 * 1024 * 1024;

std::uint32_t little32(const std::uint8_t* bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t little64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

std::vector<std::uint8_t> open_frame(const HapKey32& key, std::uint64_t counter,
                                     std::span<const std::uint8_t> header,
                                     std::span<const std::uint8_t> sealed) {
  if (sealed.size() < 16 || sealed.size() > max_body) {
    throw std::invalid_argument("invalid sealed video frame length");
  }
  std::array<std::uint8_t, 12> nonce{};
  for (unsigned index = 0; index < 8; ++index) {
    nonce[index + 4] = static_cast<std::uint8_t>(counter >> (index * 8));
  }
  using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
  Context context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!context || EVP_DecryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, key.data(),
                                     nonce.data()) != 1) {
    throw std::runtime_error("video decryption setup failed");
  }
  int written = 0;
  if (EVP_DecryptUpdate(context.get(), nullptr, &written, header.data(),
                        static_cast<int>(header.size())) != 1) {
    throw std::runtime_error("video header authentication setup failed");
  }
  const auto size = sealed.size() - 16;
  std::vector<std::uint8_t> plain(size + 1);
  if (EVP_DecryptUpdate(context.get(), plain.data(), &written, sealed.data(),
                        static_cast<int>(size)) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, 16,
                          const_cast<std::uint8_t*>(sealed.data() + size)) != 1) {
    throw std::runtime_error("video frame decryption failed");
  }
  int final = 0;
  if (EVP_DecryptFinal_ex(context.get(), plain.data() + written, &final) != 1) {
    OPENSSL_cleanse(plain.data(), plain.size());
    throw std::invalid_argument("video frame authentication failed");
  }
  if (static_cast<std::size_t>(written + final) != size) {
    throw std::runtime_error("video frame size changed during decryption");
  }
  plain.resize(size);
  return plain;
}
}  // namespace

HapKey32 derive_airplay_video_key(std::span<const std::uint8_t> pair_secret,
                                  std::uint64_t stream_connection_id) {
  if (pair_secret.empty()) throw std::invalid_argument("missing stream secret");
  return hap_hkdf_sha512(pair_secret, "DataStream-Salt" + std::to_string(stream_connection_id),
                         "DataStream-Output-Encryption-Key");
}

AirplayVideoStream::AirplayVideoStream(HapKey32 key) : key_(key) {}
AirplayVideoStream::~AirplayVideoStream() {
  fail();
}

void AirplayVideoStream::retire() noexcept {
  fail();
}

void AirplayVideoStream::fail() noexcept {
  failed_ = true;
  OPENSSL_cleanse(key_.data(), key_.size());
  if (!pending_.empty()) OPENSSL_cleanse(pending_.data(), pending_.size());
  std::vector<std::uint8_t>().swap(pending_);
}

AirplayVideoPacket AirplayVideoStream::packet(std::span<const std::uint8_t> bytes) {
  const auto* header = bytes.data();
  const auto body_size = little32(header);
  const auto kind = header[4];
  AirplayVideoPacket result{AirplayVideoPacket::Kind::heartbeat, little64(header + 8), {}, 0, {}};
  result.flags = std::uint16_t(header[6]) | (std::uint16_t(header[7]) << 8);
  const auto body = bytes.subspan(header_size, body_size);
  if (kind == 1) {
    result.kind = AirplayVideoPacket::Kind::config;
    result.data.assign(body.begin(), body.end());
    sender_timestamps_ = (result.flags & 2) != 0;
    ++configs_;
  } else if (kind == 0) {
    if (nonce_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::invalid_argument("video frame nonce exhausted");
    }
    result.kind = AirplayVideoPacket::Kind::frame;
    result.data = open_frame(key_, nonce_, bytes.first(header_size), body);
    if (sender_timestamps_) result.sender_ntp = result.timestamp;
    ++nonce_;
    ++frames_;
  } else if (kind == 5 && body_size <= 4096) {
    // Auxiliary records must not advance the frame-decryption counter.
    result.kind = AirplayVideoPacket::Kind::auxiliary;
  } else if (kind != 2) {
    throw std::invalid_argument("unknown video packet type=" + std::to_string(kind) +
                                " body_bytes=" + std::to_string(body_size));
  }
  return result;
}

std::vector<AirplayVideoPacket> AirplayVideoStream::feed(std::span<const std::uint8_t> bytes) {
  if (failed_) throw std::invalid_argument("video stream is closed after a fault");
  try {
    // Bound each call's work independently of the one incomplete packet retained
    // from the preceding call. TCP chunks may straddle a maximum-size packet.
    if (bytes.size() > max_body + header_size) {
      throw std::invalid_argument("video stream input limit exceeded");
    }
    std::vector<AirplayVideoPacket> packets;
    while (!bytes.empty()) {
      if (pending_.empty() && bytes.size() >= header_size) {
        const auto body_size = little32(bytes.data());
        if (body_size > max_body) throw std::invalid_argument("oversized video packet");
        const auto total = header_size + body_size;
        if (bytes.size() >= total) {
          // Complete packets can be authenticated directly from the caller's
          // input. Returned packets own their data; no input span escapes.
          packets.push_back(packet(bytes.first(total)));
          bytes = bytes.subspan(total);
          continue;
        }
      }
      if (pending_.size() < header_size) {
        const auto count = std::min(header_size - pending_.size(), bytes.size());
        pending_.insert(pending_.end(), bytes.begin(), bytes.begin() + count);
        bytes = bytes.subspan(count);
        if (pending_.size() < header_size) break;
      }
      const auto body_size = little32(pending_.data());
      if (body_size > max_body) throw std::invalid_argument("oversized video packet");
      const auto total = header_size + body_size;
      pending_.reserve(total);
      const auto count = std::min(total - pending_.size(), bytes.size());
      pending_.insert(pending_.end(), bytes.begin(), bytes.begin() + count);
      bytes = bytes.subspan(count);
      if (pending_.size() < total) break;
      packets.push_back(packet(pending_));
      pending_.clear();
    }
    return packets;
  } catch (...) {
    fail();
    throw;
  }
}
}  // namespace headunit
