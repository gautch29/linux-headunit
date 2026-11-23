// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/startup_warning_lights.hpp>
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
  CHECK(parse_startup_warning_lights("hide") == true);
  CHECK(parse_startup_warning_lights("show\n") == false);
  CHECK(!parse_startup_warning_lights(""));
  CHECK(!parse_startup_warning_lights("hide\n\n"));
  CHECK(!parse_startup_warning_lights("Hide"));
  CHECK(!parse_startup_warning_lights(" hide"));

  bool rejected = false;
  try {
    StartupWarningLightsFile relative("relative/path");
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);

  const auto directory = std::filesystem::temp_directory_path() /
                         ("startup-warning-lights-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directory(directory);
  const auto path = directory / "startup-warning-lights";
  StartupWarningLightsFile file(path.string());
  auto now = StartupWarningLightsFile::Clock::now();
  const auto next = [&] {
    now += StartupWarningLightsFile::poll_interval;
    return file.poll(now);
  };

  CHECK(!file.poll(now));  // Missing file keeps the release policy.
  write_private(path, "hide\n");
  CHECK(!file.poll(now));  // Rate limited within the poll interval.
  CHECK(next() == true);
  CHECK(!next());  // Unchanged file reports nothing new.
  write_private(path, "garbage\n");
  CHECK(!next());  // Malformed content keeps the previous choice.
  write_private(path, "hide\n", 0644);
  CHECK(!next());  // Group/world-readable files are not trusted.
  write_private(path, "show\n");
  CHECK(next() == false);
  const auto modified = std::filesystem::last_write_time(path);
  {
    std::ofstream output(path, std::ios::trunc);
    output << "hide\n";
  }
  std::filesystem::last_write_time(path, modified);
  CHECK(next() == true);  // Same inode, size and mtime cannot hide a change.
  {
    std::ofstream output(path, std::ios::trunc);
    output << "show\n";
  }
  std::filesystem::last_write_time(path, modified);
  CHECK(next() == false);
  write_private(path, "show\n");
  CHECK(!next());  // Rewriting the same mode is not a change.
  std::filesystem::remove(path);
  std::filesystem::create_symlink(directory / "elsewhere", path);
  write_private(directory / "elsewhere", "hide\n");
  CHECK(!next());  // Symlinks are refused.
  std::filesystem::remove(path);
  write_private(path, "hide\n");
  CHECK(next() == true);
  std::filesystem::remove(path);
  CHECK(!next());  // Removal preserves the previous valid choice.
  write_private(path, "show\n");
  CHECK(next() == false);
  std::filesystem::create_hard_link(path, directory / "alias");
  CHECK(!next());
  std::filesystem::remove(path);
  CHECK(::mkfifo(path.c_str(), 0600) == 0);
  CHECK(!next());  // Nonblocking special-file rejection.
  std::filesystem::remove_all(directory);
  std::cout
      << "startup warning lights: hide/show, bounded private polling and refusal checks passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
