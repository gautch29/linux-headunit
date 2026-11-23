// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_pair_store.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace headunit {
namespace {
void validate_identifier(std::string_view identifier) {
  if (identifier.empty() || identifier.size() > 64) {
    throw std::invalid_argument("invalid controller pairing identifier");
  }
  for (const unsigned char character : identifier) {
    if (character < 0x21 || character > 0x7e) {
      throw std::invalid_argument("invalid controller pairing identifier");
    }
  }
}

std::string filename_for(std::string_view identifier) {
  validate_identifier(identifier);
  std::array<std::uint8_t, 32> digest{};
  unsigned length = 0;
  if (EVP_Digest(identifier.data(), identifier.size(), digest.data(), &length, EVP_sha256(),
                 nullptr) != 1 ||
      length != digest.size()) {
    throw std::runtime_error("cannot hash controller pairing identifier");
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string filename;
  filename.reserve(69);
  for (const auto byte : digest) {
    filename.push_back(hex[byte >> 4]);
    filename.push_back(hex[byte & 15]);
  }
  filename += ".pair";
  return filename;
}

int open_private_directory(const std::string& path) {
  if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    throw std::runtime_error("cannot create private pairing directory");
  }
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("cannot open private pairing directory");
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) || (info.st_mode & 077) != 0 ||
      (info.st_uid != getuid() && getuid() != 0)) {
    close(fd);
    throw std::runtime_error("pairing directory must be private");
  }
  return fd;
}

void write_all(int fd, const std::uint8_t* bytes, std::size_t length) {
  while (length) {
    const auto written = write(fd, bytes, length);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("cannot save paired controller");
    bytes += written;
    length -= static_cast<std::size_t>(written);
  }
}
}  // namespace

HapPairStore::HapPairStore(const std::string& directory) : directory_(directory) {
  const int fd = open_private_directory(directory_);
  close(fd);
}

void HapPairStore::save(const HapPairRecord& record) const {
  const auto filename = filename_for(record.identifier);
  const int directory = open_private_directory(directory_);
  std::array<std::uint8_t, 8> randomness{};
  if (RAND_bytes(randomness.data(), randomness.size()) != 1) {
    close(directory);
    throw std::runtime_error("cannot create pairing storage nonce");
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string temporary = ".tmp-";
  for (const auto byte : randomness) {
    temporary.push_back(hex[byte >> 4]);
    temporary.push_back(hex[byte & 15]);
  }
  const int fd = openat(directory, temporary.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    close(directory);
    throw std::runtime_error("cannot create private pairing record");
  }
  bool file_open = true;
  try {
    std::vector<std::uint8_t> data{'H', 'P', 'R', '1',
                                   static_cast<std::uint8_t>(record.identifier.size())};
    data.insert(data.end(), record.identifier.begin(), record.identifier.end());
    data.insert(data.end(), record.public_key.begin(), record.public_key.end());
    write_all(fd, data.data(), data.size());
    const int synced = fsync(fd);
    const int closed = close(fd);
    file_open = false;
    if (synced != 0 || closed != 0) {
      throw std::runtime_error("cannot sync private pairing record");
    }
    if (renameat(directory, temporary.c_str(), directory, filename.c_str()) != 0 ||
        fsync(directory) != 0) {
      throw std::runtime_error("cannot install private pairing record");
    }
  } catch (...) {
    if (file_open) close(fd);
    unlinkat(directory, temporary.c_str(), 0);
    close(directory);
    throw;
  }
  close(directory);
}

std::optional<HapPairRecord> HapPairStore::load(std::string_view identifier) const {
  const auto filename = filename_for(identifier);
  const int directory = open_private_directory(directory_);
  const int fd = openat(directory, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    const int error = errno;
    close(directory);
    if (error == ENOENT) return std::nullopt;
    throw std::runtime_error("cannot open paired controller record");
  }
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_mode & 077) != 0 ||
      (info.st_uid != getuid() && getuid() != 0) || info.st_size < 38 || info.st_size > 101) {
    close(fd);
    close(directory);
    throw std::runtime_error("invalid paired controller record");
  }
  std::vector<std::uint8_t> data(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto count = read(fd, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      close(fd);
      close(directory);
      throw std::runtime_error("short paired controller record");
    }
    offset += static_cast<std::size_t>(count);
  }
  close(fd);
  close(directory);
  if (data[0] != 'H' || data[1] != 'P' || data[2] != 'R' || data[3] != '1' ||
      data[4] != identifier.size() || data.size() != 5 + identifier.size() + 32 ||
      !std::equal(identifier.begin(), identifier.end(), data.begin() + 5)) {
    throw std::runtime_error("paired controller record mismatch");
  }
  HapPairRecord record{std::string(identifier), {}};
  std::copy_n(data.begin() + 5 + identifier.size(), 32, record.public_key.begin());
  return record;
}

}  // namespace headunit
