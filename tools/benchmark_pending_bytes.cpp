// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/pending_bytes.hpp"

#include <array>
#include <chrono>
#include <iostream>

namespace {
struct ErasingQueue {
  std::vector<std::uint8_t> storage;
  explicit ErasingQueue(std::size_t) {}
  void append(std::span<const std::uint8_t> bytes) {
    storage.insert(storage.end(), bytes.begin(), bytes.end());
  }
  std::span<const std::uint8_t> pending() const { return storage; }
  bool empty() const { return storage.empty(); }
  void consume(std::size_t bytes) { storage.erase(storage.begin(), storage.begin() + bytes); }
};

template <class Queue>
void measure(const char* label, std::span<const std::uint8_t> wire) {
  std::array<double, 7> times{};
  for (auto& time : times) {
    Queue queue(wire.size());
    std::chrono::steady_clock::duration elapsed{};
    for (unsigned batch = 0; batch < 8; ++batch) {
      queue.append(wire);  // Input construction/allocation is outside drain timing.
      std::size_t offset = 0;
      const auto start = std::chrono::steady_clock::now();
      while (!queue.empty()) {
        const auto pending = queue.pending();
        const auto count = std::min<std::size_t>(4096, pending.size());
        if (!std::ranges::equal(pending.first(count), wire.subspan(offset, count)))
          throw std::runtime_error("queue changed output bytes");
        queue.consume(count);
        offset += count;
      }
      elapsed += std::chrono::steady_clock::now() - start;
      if (offset != wire.size()) throw std::runtime_error("queue lost output bytes");
    }
    time = std::chrono::duration<double, std::milli>(elapsed).count();
  }
  std::sort(times.begin(), times.end());
  std::cout << label << " queue_bytes=" << wire.size()
            << " chunk_bytes=4096 batches=8 median_ms=" << times[3] << " min_ms=" << times.front()
            << " max_ms=" << times.back() << '\n';
}
}  // namespace

int main() try {
  // Synthetic queue CPU cost only; no sockets, encryption, phone or rendering.
  std::vector<std::uint8_t> wire(2 * 1024 * 1024);
  for (std::size_t i = 0; i < wire.size(); ++i)
    wire[i] = static_cast<std::uint8_t>(i);
  measure<ErasingQueue>("erase", wire);
  measure<headunit::PendingBytes>("cursor", wire);
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
