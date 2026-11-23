// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_audio_packet.hpp"

#include <openssl/crypto.h>

#include <array>
#include <limits>
#include <string>
#include <stdexcept>

namespace headunit {
namespace {
constexpr std::size_t rtp_header_size = 12;
constexpr std::size_t tag_size = 16;
constexpr std::size_t nonce_size = 8;
constexpr std::size_t maximum_datagram = 4096;

std::uint64_t little_u64(std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index)
    value |= std::uint64_t(bytes[index]) << (index * 8);
  return value;
}

std::uint32_t big_u32(std::span<const std::uint8_t> bytes) {
  return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
         (std::uint32_t(bytes[2]) << 8) | bytes[3];
}
}  // namespace

AirplayAudioPackets::AirplayAudioPackets(std::span<const std::uint8_t> secret,
                                         std::uint64_t connection_id) {
  if (secret.size() != 32 && secret.size() != 64)
    throw std::invalid_argument("invalid audio session secret");
  key_ = hap_hkdf_sha512(secret, "DataStream-Salt" + std::to_string(connection_id),
                         "DataStream-Output-Encryption-Key");
}

AirplayAudioPackets::~AirplayAudioPackets() {
  OPENSSL_cleanse(key_.data(), key_.size());
}

bool AirplayAudioPackets::seen_nonce(std::uint64_t nonce) const {
  if (!received_ || nonce > latest_nonce_) return false;
  const auto distance = latest_nonce_ - nonce;
  return distance >= 64 || (replay_window_ & (std::uint64_t{1} << distance));
}

void AirplayAudioPackets::record_nonce(std::uint64_t nonce) {
  if (!received_) {
    received_ = true;
    latest_nonce_ = nonce;
    replay_window_ = 1;
  } else if (nonce > latest_nonce_) {
    const auto advance = nonce - latest_nonce_;
    replay_window_ = (advance >= 64 ? 0 : replay_window_ << advance) | 1;
    latest_nonce_ = nonce;
  } else {
    replay_window_ |= std::uint64_t{1} << (latest_nonce_ - nonce);
  }
}

std::optional<AudioRtpPacket> AirplayAudioPackets::open(std::span<const std::uint8_t> packet) {
  if (packet.size() < rtp_header_size + tag_size + nonce_size || packet.size() > maximum_datagram ||
      (packet[0] >> 6) != 2 || (packet[0] & 0x3f) != 0)
    return std::nullopt;

  const auto nonce_bytes = packet.last(nonce_size);
  const auto nonce = little_u64(nonce_bytes);
  if (seen_nonce(nonce)) return std::nullopt;
  const auto sealed = packet.subspan(rtp_header_size, packet.size() - rtp_header_size - nonce_size);
  const std::string short_nonce(reinterpret_cast<const char*>(nonce_bytes.data()),
                                nonce_bytes.size());
  const auto clear = hap_decrypt(key_, short_nonce, sealed, packet.subspan(4, 8));
  if (!clear) return std::nullopt;
  AudioRtpPacket result;
  result.sequence = (std::uint16_t(packet[2]) << 8) | packet[3];
  result.sample_time = big_u32(packet.subspan(4, 4));
  result.bytes.reserve(rtp_header_size + clear->size());
  result.bytes.insert(result.bytes.end(), packet.begin(), packet.begin() + rtp_header_size);
  result.bytes.insert(result.bytes.end(), clear->begin(), clear->end());
  record_nonce(nonce);
  return result;
}

AirplayMicrophonePackets::AirplayMicrophonePackets(std::span<const std::uint8_t> secret,
                                                   std::uint64_t connection_id,
                                                   unsigned samples_per_packet) {
  samples_per_packet_ = samples_per_packet;
  if (samples_per_packet == 0 || samples_per_packet > 960)
    throw std::invalid_argument("invalid microphone packet duration");
  if (secret.size() != 32 && secret.size() != 64)
    throw std::invalid_argument("invalid microphone session secret");
  key_ = hap_hkdf_sha512(secret, "DataStream-Salt" + std::to_string(connection_id),
                         "DataStream-Input-Encryption-Key");
}
AirplayMicrophonePackets::~AirplayMicrophonePackets() {
  OPENSSL_cleanse(key_.data(), key_.size());
}
std::vector<std::uint8_t> AirplayMicrophonePackets::seal(std::span<const std::uint8_t> payload,
                                                         std::uint32_t capture_sample_time) {
  sample_time_ = capture_sample_time;
  return seal(payload);
}
std::vector<std::uint8_t> AirplayMicrophonePackets::seal(std::span<const std::uint8_t> opus) {
  if (opus.empty() || opus.size() > 1400)
    throw std::invalid_argument("invalid microphone Opus packet");
  if (nonce_ == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("microphone nonce exhausted");
  // Stream 100 carries its type as the RTP payload identifier, SSRC zero.
  std::array<std::uint8_t, 12> header{0x80,
                                      100,
                                      std::uint8_t(sequence_ >> 8),
                                      std::uint8_t(sequence_),
                                      std::uint8_t(sample_time_ >> 24),
                                      std::uint8_t(sample_time_ >> 16),
                                      std::uint8_t(sample_time_ >> 8),
                                      std::uint8_t(sample_time_),
                                      0,
                                      0,
                                      0,
                                      0};
  std::array<std::uint8_t, 8> nonce{};
  for (std::size_t i = 0; i < nonce.size(); ++i)
    nonce[i] = std::uint8_t(nonce_ >> (8 * i));
  const std::string short_nonce(reinterpret_cast<const char*>(nonce.data()), nonce.size());
  const auto sealed = hap_encrypt(key_, short_nonce, opus, std::span(header).subspan(4, 8));
  std::vector<std::uint8_t> result(header.begin(), header.end());
  result.insert(result.end(), sealed.begin(), sealed.end());
  result.insert(result.end(), nonce.begin(), nonce.end());
  ++nonce_;
  ++sequence_;
  sample_time_ +=
      samples_per_packet_;  // Fixed-duration bench framing; production supplies each capture timestamp.
  return result;
}

}  // namespace headunit
