// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_link.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() try {
  // Framing/checksum CPU cost only; this is not Bluetooth or phone timing.
  constexpr std::size_t frame_count = 1024;
  const headunit::LinkFrame expected{0x40, 1, 0, 10, std::vector<std::uint8_t>(16384, 0x5a)};
  const auto packet = headunit::encode_link_frame(expected);
  std::vector<std::uint8_t> wire;
  wire.reserve(frame_count * packet.size());
  for (std::size_t i = 0; i < frame_count; ++i) {
    wire.insert(wire.end(), packet.begin(), packet.end());
  }
  for (const std::size_t chunk : {std::size_t(4096), packet.size(), std::size_t(65536)}) {
    std::array<double, 7> times{};
    for (auto& time : times) {
      headunit::LinkFrameReader reader;
      std::size_t frames = 0;
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t at = 0; at < wire.size(); at += chunk) {
        const auto decoded =
            reader.feed(std::span(wire).subspan(at, std::min(chunk, wire.size() - at)));
        for (const auto& frame : decoded) {
          if (frame != expected) throw std::runtime_error("link parser changed a frame");
          ++frames;
        }
      }
      time = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                 .count();
      if (frames != frame_count || reader.buffered_bytes()) {
        throw std::runtime_error("link parser lost frames");
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
