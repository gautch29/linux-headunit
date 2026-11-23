// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_data_stream.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace headunit {
namespace {
constexpr std::size_t record_limit = 16384;
constexpr std::size_t ap_header = 32;
using Bytes = std::vector<std::uint8_t>;

bool known_kind(DataMessageKind kind) {
  return kind == DataMessageKind::asynchronous || kind == DataMessageKind::synchronous ||
         kind == DataMessageKind::reply;
}
std::uint64_t big(std::span<const std::uint8_t> input) {
  std::uint64_t value = 0;
  for (const auto byte : input)
    value = (value << 8) | byte;
  return value;
}
void put_big(std::span<std::uint8_t> output, std::uint64_t value) {
  for (std::size_t i = 0; i < output.size(); ++i)
    output[output.size() - i - 1] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::string nonce(std::uint64_t value) {
  std::string bytes(8, '\0');
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = static_cast<char>(value >> (8 * i));
  return bytes;
}
struct Wipe {
  Bytes& bytes;
  ~Wipe() {
    if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
  }
};
}  // namespace

AirplayDataStream::AirplayDataStream(std::span<const std::uint8_t> secret, std::uint64_t seed) {
  if (secret.size() != 32 && secret.size() != 64)
    throw std::invalid_argument("invalid DataStream secret size");
  try {
    const auto salt = "DataStream-Salt" + std::to_string(seed);
    read_key_ = hap_hkdf_sha512(secret, salt, "DataStream-Output-Encryption-Key");
    write_key_ = hap_hkdf_sha512(secret, salt, "DataStream-Input-Encryption-Key");
  } catch (...) {
    fail();
    throw;
  }
}
AirplayDataStream::~AirplayDataStream() {
  fail();
}

void AirplayDataStream::fail() {
  failed_ = true;
  OPENSSL_cleanse(read_key_.data(), read_key_.size());
  OPENSSL_cleanse(write_key_.data(), write_key_.size());
  if (!plain_.empty()) OPENSSL_cleanse(plain_.data(), plain_.size());
  plain_.clear();
  encrypted_.clear();
}

void AirplayDataStream::packages(std::vector<DataMessage>& output) {
  std::size_t used = 0;
  while (plain_.size() - used >= 4) {
    const auto bytes = std::span<const std::uint8_t>(plain_).subspan(used);
    const auto length = big(bytes.first(4));
    if (length < ap_header || length > max_package)
      throw std::invalid_argument("invalid DataStream package size");
    if (bytes.size() < length) break;
    const auto kind = static_cast<DataMessageKind>(big(bytes.subspan(4, 4)));
    if (!known_kind(kind)) throw std::invalid_argument("unsupported DataStream package kind");
    DataMessage message;
    message.kind = kind;
    message.group = big(bytes.subspan(8, 8));
    message.type = static_cast<std::uint32_t>(big(bytes.subspan(16, 4)));
    message.token = big(bytes.subspan(20, 8));
    message.error =
        std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(big(bytes.subspan(28, 4))));
    message.payload.assign(bytes.begin() + ap_header,
                           bytes.begin() + static_cast<std::ptrdiff_t>(length));
    output.push_back(std::move(message));
    used += static_cast<std::size_t>(length);
  }
  if (used) {
    const auto remaining = plain_.size() - used;
    std::memmove(plain_.data(), plain_.data() + used, remaining);
    OPENSSL_cleanse(plain_.data() + remaining, used);
    plain_.resize(remaining);
  }
}

std::vector<DataMessage> AirplayDataStream::feed(std::span<const std::uint8_t> bytes) {
  if (failed_) throw std::invalid_argument("DataStream is closed after a fault");
  try {
    if (bytes.size() > max_feed || encrypted_.size() > record_limit + 18)
      throw std::invalid_argument("DataStream input budget exceeded");
    encrypted_.insert(encrypted_.end(), bytes.begin(), bytes.end());
    std::vector<DataMessage> messages;
    std::size_t used = 0;
    while (encrypted_.size() - used >= 2) {
      const auto record = std::span<const std::uint8_t>(encrypted_).subspan(used);
      const std::size_t length = record[0] | (static_cast<std::size_t>(record[1]) << 8);
      if (length > record_limit || read_nonce_ == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("invalid DataStream record size or nonce");
      if (record.size() < length + 18) break;
      auto clear = hap_decrypt(read_key_, nonce(read_nonce_), record.subspan(2, length + 16),
                               record.first(2));
      if (!clear) throw std::invalid_argument("DataStream authentication failed");
      Wipe wipe{*clear};
      ++read_nonce_;
      if (clear->size() > max_package + record_limit - plain_.size())
        throw std::invalid_argument("DataStream plaintext budget exceeded");
      plain_.insert(plain_.end(), clear->begin(), clear->end());
      packages(messages);
      used += length + 18;
    }
    encrypted_.erase(encrypted_.begin(), encrypted_.begin() + static_cast<std::ptrdiff_t>(used));
    return messages;
  } catch (...) {
    fail();
    throw;
  }
}

std::vector<std::uint8_t> AirplayDataStream::encode(const DataMessage& message) {
  if (failed_) throw std::invalid_argument("DataStream is closed after a fault");
  if (!known_kind(message.kind) || message.payload.size() > max_package - ap_header)
    throw std::invalid_argument("invalid outgoing DataStream package");
  try {
    Bytes package(ap_header + message.payload.size());
    Wipe wipe{package};
    auto header = std::span<std::uint8_t>(package);
    put_big(header.subspan(0, 4), package.size());
    put_big(header.subspan(4, 4), static_cast<std::uint32_t>(message.kind));
    put_big(header.subspan(8, 8), message.group);
    put_big(header.subspan(16, 4), message.type);
    put_big(header.subspan(20, 8), message.token);
    put_big(header.subspan(28, 4), std::bit_cast<std::uint32_t>(message.error));
    std::copy(message.payload.begin(), message.payload.end(), package.begin() + ap_header);
    Bytes result;
    result.reserve(package.size() + ((package.size() + record_limit - 1) / record_limit) * 18);
    for (std::size_t offset = 0; offset < package.size();) {
      if (write_nonce_ == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("DataStream output nonce exhausted");
      const auto count = std::min(record_limit, package.size() - offset);
      const std::array<std::uint8_t, 2> length{static_cast<std::uint8_t>(count),
                                               static_cast<std::uint8_t>(count >> 8)};
      const auto sealed = hap_encrypt(write_key_, nonce(write_nonce_),
                                      std::span(package).subspan(offset, count), length);
      ++write_nonce_;
      result.insert(result.end(), length.begin(), length.end());
      result.insert(result.end(), sealed.begin(), sealed.end());
      offset += count;
    }
    return result;
  } catch (...) {
    fail();
    throw;
  }
}
}  // namespace headunit
