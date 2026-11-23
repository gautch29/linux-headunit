// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace audio_retention_fixture {
inline void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
struct Descriptor {
  int fd;
  ~Descriptor() {
    if (fd >= 0) (void)::close(fd);
  }
  void close() {
    const int owned = fd;
    fd = -1;
    check(::close(owned) == 0, "retention descriptor close failed");
  }
};
inline std::string read(const std::filesystem::path& path) {
  Descriptor input{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
  check(input.fd >= 0, "retention input open failed");
  struct stat before{};
  check(fstat(input.fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 &&
            before.st_size <= 1048576,
        "retention input is not a bounded regular file");
  std::string result;
  std::array<char, 4096> bytes{};
  for (;;) {
    const auto count = ::read(input.fd, bytes.data(), bytes.size());
    check(count >= 0, "retention input read failed");
    if (!count) break;
    result.append(bytes.data(), count);
    check(result.size() <= 1048576, "retention input grew beyond bound");
  }
  struct stat after{};
  check(fstat(input.fd, &after) == 0 && before.st_dev == after.st_dev &&
            before.st_ino == after.st_ino && before.st_size == after.st_size &&
            result.size() == std::uint64_t(before.st_size),
        "retention source changed during read");
  input.close();
  return result;
}
// A live collector only appends. Inspect its already-written header without
// treating a concurrently growing trace as a closed retention snapshot.
inline std::string header(const std::filesystem::path& path) {
  Descriptor input{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
  check(input.fd >= 0, "live header input open failed");
  struct stat state{};
  check(fstat(input.fd, &state) == 0 && S_ISREG(state.st_mode), "live header is not regular");
  std::string result;
  for (unsigned i = 0; i < 1024; ++i) {
    char byte = 0;
    check(::read(input.fd, &byte, 1) == 1, "live collector header incomplete");
    result += byte;
    if (byte == '\n') {
      input.close();
      return result;
    }
  }
  throw std::runtime_error("live collector header exceeded bound");
}
inline void verify(const std::filesystem::path& path, std::string_view bytes) {
  check(read(path) == bytes, "closed retention copy does not match original bytes");
}
inline void write_new(const std::filesystem::path& path, std::string_view bytes) {
  check(bytes.size() <= 1048576, "retention output exceeded bound");
  Descriptor output{open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
  check(output.fd >= 0, "retention output open failed");
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto count = ::write(output.fd, bytes.data() + written, bytes.size() - written);
    check(count > 0, "retention output write failed");
    written += count;
  }
  check(fsync(output.fd) == 0, "retention output sync failed");
  output.close();
  verify(path, bytes);
}
// Failure leaves the original directory and any partial target intact. Only
// successful closed copies plus a serialized/re-read manifest permit removal.
inline std::filesystem::path retain(const std::filesystem::path& root,
                                    const std::filesystem::path& evidence,
                                    const std::vector<std::string>& names, bool remove) {
  std::string pattern = (evidence / "hu-pcm-retained-XXXXXX").string();
  check(mkdtemp(pattern.data()) != nullptr, "retention directory creation failed");
  const std::filesystem::path target(pattern);
  std::string manifest = "{\"closedCopiesVerified\":true,\"files\":[";
  bool first = true;
  for (const auto& name : names) {
    check(name == std::filesystem::path(name).filename() && name.find('"') == std::string::npos,
          "retention fixture filename invalid");
    const auto bytes = read(root / name);
    write_new(target / name, bytes);
    verify(root / name, bytes);
    if (!first) manifest += ',';
    first = false;
    manifest += "{\"name\":\"" + name + "\",\"bytes\":" + std::to_string(bytes.size()) + "}";
  }
  manifest += "]}\n";
  write_new(target / "retention.json", manifest);
  verify(target / "retention.json", manifest);
  if (remove) std::filesystem::remove_all(root);
  return target;
}
}  // namespace audio_retention_fixture
