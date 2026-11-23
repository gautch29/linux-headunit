// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/ultra_asset_transfer.hpp"
#include "headunit/opack.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <stdexcept>

namespace headunit {
namespace {
using Dict = OpackValue::Dictionary;
using Bytes = OpackValue::Bytes;
const OpackValue& field(const Dict& value, const char* key) {
  const auto item = value.find(key);
  if (item == value.end()) throw std::invalid_argument("missing asset field");
  return item->second;
}
std::uint64_t integer(const Dict& value, const char* key, std::uint64_t maximum) {
  const auto* number = std::get_if<std::int64_t>(&field(value, key).value);
  if (!number || *number < 0 || static_cast<std::uint64_t>(*number) > maximum)
    throw std::invalid_argument("invalid asset integer");
  return static_cast<std::uint64_t>(*number);
}
std::string string(const Dict& value, const char* key, std::size_t maximum) {
  const auto* text = std::get_if<std::string>(&field(value, key).value);
  if (!text || text->empty() || text->size() > maximum || text->find('\0') != std::string::npos)
    throw std::invalid_argument("invalid asset string");
  return *text;
}
const Bytes& data(const Dict& value, const char* key, std::size_t maximum) {
  const auto* bytes = std::get_if<Bytes>(&field(value, key).value);
  if (!bytes || bytes->empty() || bytes->size() > maximum)
    throw std::invalid_argument("invalid asset data");
  return *bytes;
}
std::array<std::uint8_t, 32> checksum(const std::string& hex) {
  if (hex.size() != 64) throw std::invalid_argument("invalid asset checksum");
  const auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    throw std::invalid_argument("invalid asset checksum digit");
  };
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<std::uint8_t>((digit(hex[2 * i]) << 4) | digit(hex[2 * i + 1]));
  return out;
}
Bytes acceptance(std::int64_t status) {
  return encode_opack(OpackValue{
      Dict{{"messageType", OpackValue{std::int64_t{5}}}, {"accepted", OpackValue{status}}}});
}
}  // namespace
struct UltraAssetTransfer::Hash {
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  Hash() {
    if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
      EVP_MD_CTX_free(context);
      throw std::runtime_error("asset SHA256 initialization failed");
    }
  }
  ~Hash() { EVP_MD_CTX_free(context); }
};
UltraAssetTransfer::UltraAssetTransfer(AssetSink& sink, Clock::time_point now)
    : sink_(sink), started_(now), last_activity_(now), last_observed_(now) {}
UltraAssetTransfer::~UltraAssetTransfer() {
  disconnect();
}
void UltraAssetTransfer::disconnect() noexcept {
  if (staging_) {
    sink_.rollback();
    staging_ = false;
  }
  hash_.reset();
  if (state_ != State::complete) state_ = State::failed;
}
UltraAssetTransfer::Reply UltraAssetTransfer::fail() {
  disconnect();
  state_ = State::failed;
  return acceptance(5);
}
UltraAssetTransfer::Reply UltraAssetTransfer::poll(Clock::time_point now) {
  if (state_ == State::complete || state_ == State::failed) return {};
  if (now < last_observed_ || now - last_activity_ >= std::chrono::seconds(30) ||
      now - started_ >= std::chrono::minutes(5))
    return fail();
  last_observed_ = now;
  return {};
}
UltraAssetTransfer::Reply UltraAssetTransfer::receive(std::span<const std::uint8_t> wire,
                                                      Clock::time_point now) {
  if (auto timeout = poll(now)) return timeout;
  if (state_ == State::complete || state_ == State::failed) return {};
  try {
    auto value = decode_opack(wire, {384 * 1024, 768 * 1024, 64, 4});
    const auto* message = std::get_if<Dict>(&value.value);
    if (!message) throw std::invalid_argument("asset message is not a dictionary");
    const auto type = integer(*message, "messageType", 5);
    if (state_ == State::waiting) {
      if (type != 1 || integer(*message, "payloadType", 1) != 1)
        throw std::invalid_argument("expected theme header");
      header_.session = string(*message, "sessionID", 128);
      header_.version = integer(*message, "version", INT64_MAX);
      header_.chunks = static_cast<std::uint32_t>(integer(*message, "chunkCount", 4096));
      if (!header_.chunks) throw std::invalid_argument("empty theme");
      header_.certificate = data(*message, "certificate", 32768);
      header_.signature = data(*message, "signature", 4096);
      if (message->contains("additions")) header_.additions = data(*message, "additions", 65536);
      hash_ = std::make_unique<Hash>();
      staging_ = true;  // Even a partially failed begin must roll back.
      sink_.begin(header_);
      state_ = State::receiving;
    } else {
      if (type != 2 || string(*message, "sessionID", 128) != header_.session ||
          integer(*message, "chunkIndex", 4095) != next_chunk_)
        throw std::invalid_argument("out-of-order theme chunk");
      const auto& chunk = data(*message, "data", 256 * 1024);
      if (chunk.size() > 64 * 1024 * 1024 - bytes_)
        throw std::invalid_argument("theme archive exceeds limit");
      const bool final = next_chunk_ + 1 == header_.chunks;
      if (message->contains("checksum") != final)
        throw std::invalid_argument("checksum must be on final chunk");
      const auto expected =
          final ? checksum(string(*message, "checksum", 64)) : std::array<std::uint8_t, 32>{};
      sink_.append(chunk);
      if (EVP_DigestUpdate(hash_->context, chunk.data(), chunk.size()) != 1)
        throw std::runtime_error("theme hashing failed");
      bytes_ += chunk.size();
      ++next_chunk_;
      if (final) {
        std::array<std::uint8_t, 32> digest{};
        unsigned length = 0;
        if (EVP_DigestFinal_ex(hash_->context, digest.data(), &length) != 1 ||
            length != digest.size() || CRYPTO_memcmp(digest.data(), expected.data(), 32))
          throw std::invalid_argument("theme checksum mismatch");
        // Allocate the reply before commit, so allocation failure cannot leave
        // an installed asset followed by a reported failure.
        auto reply = acceptance(0);
        if (!sink_.commit(header_, digest)) throw std::invalid_argument("theme policy rejected");
        staging_ = false;
        hash_.reset();
        state_ = State::complete;
        return reply;
      }
    }
    last_activity_ = now;
    return {};
  } catch (const std::exception&) {
    return fail();
  }
}
}  // namespace headunit
