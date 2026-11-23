// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_video_stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

int main() try {
  // Synthetic authenticated packets only: no phone, decoder, GPU or receipts.
  constexpr unsigned frame_count = 512;
  constexpr std::size_t payload_size = 65536;
  headunit::HapKey32 key{};
  const std::vector<std::uint8_t> plain(payload_size, 42);
  std::vector<std::uint8_t> wire;
  std::array<std::uint8_t, 128> header{};
  for (unsigned i = 0; i < 4; ++i) {
    header[i] = static_cast<std::uint8_t>((payload_size + 16) >> (8 * i));
  }
  wire.reserve(frame_count * (payload_size + 144));
  for (unsigned i = 0; i < frame_count; ++i) {
    std::array<char, 8> nonce{};
    for (unsigned j = 0; j < 8; ++j) {
      nonce[j] = static_cast<char>(std::uint64_t(i) >> (8 * j));
    }
    const auto encrypted = headunit::hap_encrypt(key, {nonce.data(), nonce.size()}, plain, header);
    wire.insert(wire.end(), header.begin(), header.end());
    wire.insert(wire.end(), encrypted.begin(), encrypted.end());
  }
  for (const std::size_t chunk : {payload_size + 144, std::size_t(16384), std::size_t(4096)}) {
    std::array<double, 7> times{};
    for (auto& time : times) {
      headunit::AirplayVideoStream parser(key);
      std::size_t frames = 0;
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t offset = 0; offset < wire.size(); offset += chunk) {
        const auto packets =
            parser.feed(std::span(wire).subspan(offset, std::min(chunk, wire.size() - offset)));
        for (const auto& packet : packets) {
          if (packet.data != plain) throw std::runtime_error("video parser changed payload");
          ++frames;
        }
      }
      time = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                 .count();
      if (frames != frame_count || parser.partial()) {
        throw std::runtime_error("video parser lost frames");
      }
    }
    std::sort(times.begin(), times.end());
    std::cout << "chunk_bytes=" << chunk << " frames=" << frame_count << " median_ms=" << times[3]
              << " min_ms=" << times.front() << " max_ms=" << times.back() << '\n';
  }
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
