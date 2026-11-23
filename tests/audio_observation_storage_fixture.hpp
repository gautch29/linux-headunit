// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

// Explicit Linux-only, owned <=1 MiB Docker tmpfs. Never consume host capacity.
#include "audio_observation_retention_fixture.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace audio_storage_fixture {
inline void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
struct Scratch {
  std::filesystem::path root;
  int filler = -1;
  std::uint64_t filled = 0;
  bool retired = false;
  explicit Scratch() {
    const char* location = std::getenv("HEADUNIT_PCM_STORAGE_FIXTURE");
    check(location && *location, "explicit owned tmpfs fixture is required");
    struct statfs fs{};
    struct stat owner{};
    check(statfs(location, &fs) == 0 && fs.f_type == 0x01021994 && fs.f_bsize > 0 &&
              fs.f_blocks > 0 && std::uint64_t(fs.f_blocks) * fs.f_bsize <= 1048576,
          "storage fixture must be tmpfs bounded at 1 MiB");
    check(lstat(location, &owner) == 0 && S_ISDIR(owner.st_mode) && owner.st_uid == getuid() &&
              (owner.st_mode & 0777) == 0700,
          "storage fixture must be a private owned directory");
    std::string pattern = std::string(location) + "/hu-pcm-storage-XXXXXX";
    check(mkdtemp(pattern.data()) != nullptr, "owned storage directory failed");
    root = pattern;
  }
  ~Scratch() {
    if (filler >= 0) (void)close(filler);
    if (retired) return;
    // Never delete originals during a failed assertion/receipt write. A
    // verified persistent-container copy survives the ephemeral tmpfs mount;
    // the host runner must retain it before removing this owned container.
    try {
      const auto fallback = audio_retention_fixture::retain(root, "/tmp", {"trace.jsonl"}, false);
      std::cerr << "PCM_RETENTION_FALLBACK=" << fallback.string() << '\n';
    } catch (const std::exception& error) {
      std::cerr << "PCM_RETENTION_FAILED_ORIGINAL=" << root.string() << " reason=" << error.what()
                << '\n';
    }
  }
  void retire(const std::filesystem::path& evidence) {
    if (filler >= 0) {
      const int owned = filler;
      filler = -1;
      check(close(owned) == 0, "owned filler close failed");
    }
    (void)audio_retention_fixture::retain(root, evidence, {"trace.jsonl"}, true);
    retired = true;
  }
  void fill() {
    filler = open((root / "filler").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    check(filler >= 0, "owned filler open failed");
    std::array<char, 4096> bytes{};
    for (;;) {
      const auto count = write(filler, bytes.data(), bytes.size());
      if (count < 0) {
        check(errno == ENOSPC, "actual tmpfs exhaustion was not ENOSPC");
        break;
      }
      check(count > 0, "owned filler made no progress");
      filled += count;
      check(filled <= 1048576, "owned filler exceeded mount bound");
    }
    char probe = 0;
    check(write(filler, &probe, 1) == -1 && errno == ENOSPC,
          "exhausted tmpfs accepted a byte or returned another error");
  }
};
inline std::filesystem::path receipts() {
  const char* path = std::getenv("HEADUNIT_PCM_STORAGE_RECEIPTS");
  check(path && *path, "outside-tmpfs evidence directory required");
  struct statfs fs{};
  check(statfs(path, &fs) == 0 && fs.f_type != 0x01021994,
        "receipts must survive outside the exhausted tmpfs");
  return path;
}
inline std::string read(const std::filesystem::path& path) {
  return audio_retention_fixture::read(path);
}
}  // namespace audio_storage_fixture
