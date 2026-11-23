// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/gst_video_preview.hpp"

#include <chrono>
#include <charconv>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

int main(int argc, char** argv) try {
  if (argc < 3 || argc > 5)
    throw std::invalid_argument(
        "usage: headunit-video-replay PRIVATE_PACKETS_FILE fake|x11|wayland|wayland-touch "
        "[STARTUP_BURST_FRAMES [v4l2h264dec|avdec_h264]]");
  unsigned startup_burst = 0;
  if (argc >= 4) {
    const std::string_view value(argv[3]);
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), startup_burst);
    if (error != std::errc{} || end != value.data() + value.size() || startup_burst > 128)
      throw std::invalid_argument("startup burst must be 0..128 frames");
  }
  if (std::filesystem::file_size(argv[1]) > 8 * 1024 * 1024 + 1024)
    throw std::invalid_argument("video capture exceeds limit");
  std::ifstream source(argv[1], std::ios::binary);
  if (!source) throw std::runtime_error("cannot open video capture");
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(source),
                                        std::istreambuf_iterator<char>()};
  const std::vector<std::uint8_t> magic{'U', 'H', 'V', 'P', 1};
  if (bytes.size() < magic.size() || !std::equal(magic.begin(), magic.end(), bytes.begin()))
    throw std::invalid_argument("invalid video capture header");
  headunit::GstVideoPreview preview(argv[2], 1280, 720, argc == 5 ? argv[4] : "v4l2h264dec");
  std::size_t offset = magic.size();
  std::size_t frames = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 13) throw std::invalid_argument("truncated video capture record");
    const auto kind = bytes[offset];
    std::uint64_t timestamp = 0;
    std::uint32_t length = 0;
    for (unsigned i = 0; i < 8; ++i)
      timestamp |= static_cast<std::uint64_t>(bytes[offset + 1 + i]) << (8 * i);
    for (unsigned i = 0; i < 4; ++i)
      length |= static_cast<std::uint32_t>(bytes[offset + 9 + i]) << (8 * i);
    offset += 13;
    if (kind > 1 || bytes.size() - offset < length)
      throw std::invalid_argument("invalid video capture record");
    headunit::AirplayVideoPacket packet{
        kind == 1 ? headunit::AirplayVideoPacket::Kind::config
                  : headunit::AirplayVideoPacket::Kind::frame,
        timestamp,
        {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
         bytes.begin() + static_cast<std::ptrdiff_t>(offset + length)}};
    preview.submit(packet);
    if (kind == 0) {
      ++frames;
      if (frames >= startup_burst) std::this_thread::sleep_for(std::chrono::milliseconds(17));
    }
    offset += length;
  }
  const auto decoded = preview.finish();
  std::cout << "submitted_frames=" << frames << " decoded_frames=" << decoded << '\n';
  return decoded == frames ? 0 : 2;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
