// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/display_response.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace headunit::theme;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)

namespace {
void write_private(const std::filesystem::path& path, const std::string& content,
                   mode_t mode = 0600) {
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << content;
  }
  CHECK(::chmod(temporary.c_str(), mode) == 0);
  std::filesystem::rename(temporary, path);
}
}  // namespace

int main() try {
  CHECK(parse_display_response("smooth") == DisplayResponse::smooth);
  CHECK(parse_display_response("balanced\n") == DisplayResponse::balanced);
  CHECK(parse_display_response("fast\n") == DisplayResponse::fast);
  CHECK(!parse_display_response(""));
  CHECK(!parse_display_response("fast\n\n"));
  CHECK(!parse_display_response("Fast"));
  CHECK(!parse_display_response(" fast"));

  const auto smooth = display_response_policy(DisplayResponse::smooth);
  const auto balanced = display_response_policy(DisplayResponse::balanced);
  const auto fast = display_response_policy(DisplayResponse::fast);
  CHECK(smooth.presentation_advance_ns == 0 && smooth.main_queue_depth == 2);
  CHECK(balanced.presentation_advance_ns == 50000000 && balanced.main_queue_depth == 2);
  CHECK(fast.presentation_advance_ns == 80000000 && fast.main_queue_depth == 1);

  bool rejected = false;
  try {
    DisplayResponseFile relative("relative/path");
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);

  const auto directory =
      std::filesystem::temp_directory_path() / ("display-response-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directory(directory);
  const auto path = directory / "display-response";
  DisplayResponseFile file(path.string());
  auto now = DisplayResponseFile::Clock::now();
  const auto next = [&] {
    now += DisplayResponseFile::poll_interval;
    return file.poll(now);
  };

  CHECK(!file.poll(now));  // Missing file keeps the release policy.
  write_private(path, "fast\n");
  CHECK(!file.poll(now));  // Rate limited within the poll interval.
  CHECK(next() == DisplayResponse::fast);
  CHECK(!next());  // Unchanged file reports nothing new.
  write_private(path, "garbage\n");
  CHECK(!next());  // Malformed content keeps the previous choice.
  write_private(path, "smooth\n", 0644);
  CHECK(!next());  // Group/world-readable files are not trusted.
  write_private(path, "balanced\n");
  CHECK(next() == DisplayResponse::balanced);
  write_private(path, "balanced\n");
  CHECK(!next());  // Rewriting the same mode is not a change.
  std::filesystem::remove(path);
  std::filesystem::create_symlink(directory / "elsewhere", path);
  write_private(directory / "elsewhere", "smooth\n");
  CHECK(!next());  // Symlinks are refused.
  std::filesystem::remove(path);
  write_private(path, "smooth\n");
  CHECK(next() == DisplayResponse::smooth);
  std::filesystem::remove_all(directory);
  std::cout
      << "display response: modes, policies, private file polling and refusal checks passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
