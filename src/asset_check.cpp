// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/asset_signature.hpp"
#include <openssl/evp.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <span>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
class InputFile {
 public:
  InputFile(const char* path, std::size_t maximum)
      : file_{::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)} {
    struct stat info{};
    if (file_.fd < 0 || fstat(file_.fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || static_cast<std::uint64_t>(info.st_size) > maximum)
      throw std::runtime_error("verification input must be a bounded regular file");
    size_ = static_cast<std::size_t>(info.st_size);
  }
  InputFile(const InputFile&) = delete;
  InputFile& operator=(const InputFile&) = delete;
  std::size_t size() const { return size_; }
  std::size_t read(std::span<std::uint8_t> buffer) {
    ssize_t count;
    do {
      count = ::read(file_.fd, buffer.data(), buffer.size());
    } while (count < 0 && errno == EINTR);
    if (count < 0 || (count == 0 && used_ != size_) ||
        (count > 0 && static_cast<std::size_t>(count) > size_ - used_))
      throw std::runtime_error("verification input changed or could not be read");
    used_ += static_cast<std::size_t>(count);
    return static_cast<std::size_t>(count);
  }

 private:
  struct File {
    int fd;
    ~File() {
      if (fd >= 0) close(fd);
    }
  } file_;
  std::size_t size_ = 0, used_ = 0;
};
std::vector<std::uint8_t> read(const char* path, std::size_t maximum) {
  InputFile input(path, maximum);
  std::vector<std::uint8_t> bytes(input.size());
  std::size_t used = 0;
  while (used < bytes.size())
    used += input.read(std::span(bytes).subspan(used));
  std::array<std::uint8_t, 1> extra{};
  (void)input.read(extra);
  return bytes;
}
}  // namespace
int main(int argc, char** argv) {
  const bool allow_expired = argc > 1 && std::string_view(argv[argc - 1]) == "--allow-expired-leaf";
  if (allow_expired) --argc;
  if (argc != 5) {
    std::cerr << "usage: headunit-asset-check ARCHIVE SIGNATURE_DER CHAIN_PEM TRUSTED_ROOT_PEM "
                 "[--allow-expired-leaf]\n";
    return 1;
  }
  try {
    const auto signature = read(argv[2], 4096);
    const auto chain = read(argv[3], 32768);
    const auto root = read(argv[4], 32768);
    InputFile archive(argv[1], 64 * 1024 * 1024);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("cannot initialize archive hash");
    std::array<std::uint8_t, 65536> block{};
    std::size_t total = 0;
    while (const auto length = archive.read(block)) {
      total += length;
      if (EVP_DigestUpdate(hash.get(), block.data(), length) != 1)
        throw std::runtime_error("archive hash failed");
    }
    if (!total) throw std::runtime_error("empty archive");
    std::array<std::uint8_t, 32> digest{};
    unsigned length = 0;
    if (EVP_DigestFinal_ex(hash.get(), digest.data(), &length) != 1 || length != 32)
      throw std::runtime_error("archive hash failed");
    const auto chain_text =
        std::string_view(reinterpret_cast<const char*>(chain.data()), chain.size());
    const auto root_text =
        std::string_view(reinterpret_cast<const char*>(root.data()), root.size());
    auto result = headunit::verify_asset_signature(digest, signature, chain_text, root_text);
    bool expired_exception = false;
    if (allow_expired && result == headunit::AssetSignatureResult::invalid_certificate_time) {
      result = headunit::verify_asset_signature(
          digest, signature, chain_text, root_text,
          headunit::AssetCertificateTimePolicy::allow_expired_leaf);
      expired_exception = result == headunit::AssetSignatureResult::valid;
    }
    using R = headunit::AssetSignatureResult;
    const char* status = "malformed signing material";
    switch (result) {
      case R::valid:
        status = expired_exception
                     ? "signature and trusted chain verified; expired signing leaf allowed"
                     : "signature and current chain validity verified";
        break;
      case R::malformed:
        break;
      case R::untrusted_chain:
        status = "untrusted certificate chain";
        break;
      case R::invalid_certificate_time:
        status = "certificate outside validity period";
        break;
      case R::invalid_signer_usage:
        status = "certificate not a permitted archive signer";
        break;
      case R::invalid_signature:
        status = "archive signature rejected";
        break;
    }
    std::cout << "Archive bytes=" << total << ": " << status << '\n';
    std::cout << "Expired signing leaf exception used=" << (expired_exception ? "true" : "false")
              << '\n';
    std::cout << "This check does not install, extract, execute, or authorize a theme.\n";
    return result == R::valid ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
