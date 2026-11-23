// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/video_output.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}  // namespace

int main() try {
  char directory[] = "/tmp/headunit-video-output-XXXXXX";
  check(mkdtemp(directory) != nullptr, "temporary directory failed");
  struct Cleanup {
    const char* path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};

  headunit::VideoOutput::Options options;
  options.capture_directory = directory;
  options.connection_number = 7;
  options.preview_sink = "none";
  options.preview_decoder = "avdec_h264";
  options.cluster_display.width = 1920;
  headunit::VideoOutput output(std::move(options));
  check(output.drain_touches([](headunit::TouchInput) -> bool {
    throw std::runtime_error("headless output invented touch input");
  }) == 0,
        "headless output invented touch input");
  std::vector<pollfd> descriptors;
  output.append_input_poll_descriptor(descriptors);
  check(descriptors.empty(), "headless output added an input descriptor");

  const headunit::AirplayVideoPacket config{
      headunit::AirplayVideoPacket::Kind::config, 0x0102030405060708ull, {0x11, 0x22}};
  const headunit::AirplayVideoPacket frame{
      headunit::AirplayVideoPacket::Kind::frame, 9, {0x33, 0x44, 0x55}};
  output.record(110, config, std::nullopt);
  output.record(110, frame, std::nullopt);
  output.record(111, frame, std::nullopt);
  output.record(112, frame, std::nullopt);
  output.record(110, {headunit::AirplayVideoPacket::Kind::heartbeat, 10, {0x66}}, std::nullopt);

  const auto main = read_bytes(std::filesystem::path(directory) / "video-7-110.packets");
  const std::vector<std::uint8_t> expected{
      'U',  'H',  'V', 'P', 1, 1, 8, 7, 6, 5, 4, 3, 2, 1, 2, 0,    0,    0,
      0x11, 0x22, 0,   9,   0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0x33, 0x44, 0x55};
  check(main == expected, "main stream capture format changed");
  check(read_bytes(std::filesystem::path(directory) / "video-7-111.packets").size() == 21,
        "cluster stream not isolated");
  check(!std::filesystem::exists(std::filesystem::path(directory) / "video-7-112.packets"),
        "unknown stream captured");
  std::cout << "video output capture and stream isolation passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
