// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include <cstdint>

namespace headunit {

// Single-owner bounded socket output. Partial writes advance a cursor without
// shifting queued ciphertext. Append sources must not alias this queue.
class PendingBytes {
 public:
  explicit PendingBytes(std::size_t maximum) : maximum_(maximum) {}
  PendingBytes(const PendingBytes&) = delete;
  PendingBytes& operator=(const PendingBytes&) = delete;
  PendingBytes(PendingBytes&& other) noexcept
      : maximum_(other.maximum_),
        storage_(std::move(other.storage_)),
        offset_(std::exchange(other.offset_, 0)) {
    other.clear();
  }
  PendingBytes& operator=(PendingBytes&& other) noexcept {
    if (this != &other) {
      maximum_ = other.maximum_;
      storage_ = std::move(other.storage_);
      offset_ = std::exchange(other.offset_, 0);
      other.clear();
    }
    return *this;
  }
  std::size_t size() const { return storage_.size() - offset_; }
  std::size_t available() const { return maximum_ - size(); }
  bool empty() const { return size() == 0; }
  std::span<const std::uint8_t> pending() const {
    return std::span<const std::uint8_t>(storage_).subspan(offset_);
  }
  void clear() noexcept {
    storage_.clear();
    offset_ = 0;
  }
  void consume(std::size_t count) {
    if (count > size()) throw std::invalid_argument("socket output consumption exceeds queue");
    offset_ += count;
    if (empty()) clear();
  }
  void append(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > available()) throw std::invalid_argument("socket output queue is full");
    if (bytes.empty()) return;
    if (bytes.size() > maximum_ - storage_.size()) {
      const auto retained = size();
      std::memmove(storage_.data(), storage_.data() + offset_, retained);
      storage_.resize(retained);
      offset_ = 0;
    }
    const auto required = storage_.size() + bytes.size();
    if (required > storage_.capacity()) {
      const auto capacity = storage_.capacity();
      const auto grown = capacity + std::min(capacity, maximum_ - capacity);
      storage_.reserve(std::max(required, grown));
    }
    storage_.insert(storage_.end(), bytes.begin(), bytes.end());
  }

 private:
  std::size_t maximum_;
  std::vector<std::uint8_t> storage_;
  std::size_t offset_ = 0;
};

}  // namespace headunit
