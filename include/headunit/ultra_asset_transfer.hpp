// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace headunit {
struct AssetHeader {
  std::string session;
  std::uint64_t version = 0;
  std::uint32_t chunks = 0;
  std::vector<std::uint8_t> certificate, signature, additions;
};

// The sink must outlive the receiver. commit must verify signing trust, signed
// identity/version and local acceptance policy, then durably activate atomically.
// A checksum alone is never sufficient. rollback must preserve the old asset.
class AssetSink {
 public:
  virtual ~AssetSink() = default;
  virtual void begin(const AssetHeader&) = 0;
  virtual void append(std::span<const std::uint8_t>) = 0;
  virtual bool commit(const AssetHeader&, const std::array<std::uint8_t, 32>& digest) = 0;
  virtual void rollback() noexcept = 0;
};

class UltraAssetTransfer {
 public:
  using Clock = std::chrono::steady_clock;
  using Reply = std::optional<std::vector<std::uint8_t>>;
  enum class State { waiting, receiving, complete, failed };
  explicit UltraAssetTransfer(AssetSink& sink, Clock::time_point now = Clock::now());
  ~UltraAssetTransfer();
  UltraAssetTransfer(const UltraAssetTransfer&) = delete;
  UltraAssetTransfer& operator=(const UltraAssetTransfer&) = delete;
  Reply receive(std::span<const std::uint8_t> opack, Clock::time_point now = Clock::now());
  Reply poll(Clock::time_point now = Clock::now());
  void disconnect() noexcept;
  State state() const { return state_; }
  std::size_t received_bytes() const { return bytes_; }

 private:
  struct Hash;
  Reply fail();
  AssetSink& sink_;
  std::unique_ptr<Hash> hash_;
  AssetHeader header_;
  State state_ = State::waiting;
  Clock::time_point started_, last_activity_, last_observed_;
  std::uint32_t next_chunk_ = 0;
  std::size_t bytes_ = 0;
  bool staging_ = false;
};
}  // namespace headunit
