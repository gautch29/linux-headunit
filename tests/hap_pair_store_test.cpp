// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_pair_store.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() try {
  char directory[] = "/tmp/headunit-pair-store-XXXXXX";
  check(mkdtemp(directory) != nullptr, "cannot create temporary pair store");
  try {
    headunit::HapPairStore store(directory);
    headunit::HapPairRecord record{"synthetic-phone-id", {}};
    for (std::size_t i = 0; i < record.public_key.size(); ++i) {
      record.public_key[i] = static_cast<std::uint8_t>(i);
    }
    check(!store.load(record.identifier), "unexpected prior pairing record");
    store.save(record);
    const auto loaded = store.load(record.identifier);
    check(loaded && loaded->identifier == record.identifier &&
              loaded->public_key == record.public_key,
          "paired controller did not round-trip");
    check(!store.load("another-phone"), "pairing store returned wrong record");
    check(!store.load("../not-a-phone"), "hashed identifier unexpectedly found a pairing record");
    DIR* dir = opendir(directory);
    check(dir != nullptr, "cannot inspect pairing permissions");
    unsigned count = 0;
    while (auto* entry = readdir(dir)) {
      const std::string name(entry->d_name);
      if (name == "." || name == "..") continue;
      struct stat info{};
      const std::string path = std::string(directory) + "/" + name;
      check(lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 077) == 0,
            "pairing record permissions are not private");
      unlink(path.c_str());
      ++count;
    }
    closedir(dir);
    check(count == 1, "unexpected number of pairing records");
  } catch (...) {
    rmdir(directory);
    throw;
  }
  rmdir(directory);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
