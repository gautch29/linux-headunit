// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/asset_bundle.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <plist/plist.h>
#include <openssl/evp.h>
#include <array>
#include <charconv>
#include <sstream>
#include <limits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <memory>
#include <set>
#include <stdexcept>

namespace headunit {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t file_limit = 16 * 1024 * 1024;
Bytes read_file(const std::string& path, std::size_t maximum) {
  struct File {
    int fd;
    ~File() {
      if (fd >= 0) close(fd);
    }
  } file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  struct stat stat{};
  if (file.fd < 0 || fstat(file.fd, &stat) != 0 || !S_ISREG(stat.st_mode) || stat.st_size <= 0 ||
      static_cast<std::uint64_t>(stat.st_size) > maximum)
    throw std::invalid_argument("asset input must be a bounded regular file");
  Bytes data(static_cast<std::size_t>(stat.st_size));
  std::size_t used = 0;
  while (used < data.size()) {
    const auto count = ::read(file.fd, data.data() + used, data.size() - used);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::invalid_argument("asset input changed or could not be read");
    used += static_cast<std::size_t>(count);
  }
  char extra{};
  ssize_t count;
  do {
    count = ::read(file.fd, &extra, 1);
  } while (count < 0 && errno == EINTR);
  if (count != 0) throw std::invalid_argument("asset input grew or could not be read");
  return data;
}
struct Reader {
  struct archive* value = archive_read_new();
  explicit Reader(const Bytes& bytes) {
    if (!value) throw std::runtime_error("archive reader allocation failed");
    if (archive_read_support_format_zip(value) != ARCHIVE_OK ||
        archive_read_open_memory(value, bytes.data(), bytes.size()) != ARCHIVE_OK) {
      archive_read_free(value);
      value = nullptr;
      throw std::invalid_argument("invalid ZIP archive");
    }
  }
  ~Reader() {
    if (value) archive_read_free(value);
  }
};
std::string path_name(archive_entry* entry) {
  const char* raw = archive_entry_pathname_utf8(entry);
  if (!raw) throw std::invalid_argument("invalid archive pathname encoding");
  std::string name(raw);
  if (!name.empty() && name.back() == '/') name.pop_back();
  if (name.empty() || name.size() > 1024 || name.front() == '/' ||
      name.find_first_of("\\:") != std::string::npos)
    throw std::invalid_argument("unsafe archive pathname");
  std::size_t start = 0;
  while (start < name.size()) {
    auto end = name.find('/', start);
    if (end == std::string::npos) end = name.size();
    const auto part = name.substr(start, end - start);
    if (part.empty() || part == "." || part == "..")
      throw std::invalid_argument("unsafe archive path component");
    start = end + 1;
  }
  if (name.back() == '/') throw std::invalid_argument("empty archive path component");
  return name;
}
Bytes contents(archive* reader, std::size_t size) {
  Bytes result(size);
  std::size_t used = 0;
  while (used < size) {
    const auto got = archive_read_data(reader, result.data() + used, size - used);
    if (got <= 0) throw std::invalid_argument("truncated archive entry");
    used += static_cast<std::size_t>(got);
  }
  char extra{};
  if (archive_read_data(reader, &extra, 1) != 0)
    throw std::invalid_argument("invalid archive entry size or checksum");
  return result;
}
std::string string(plist_t root, const char* key) {
  auto value = plist_dict_get_item(root, key);
  if (!value || plist_get_node_type(value) != PLIST_STRING)
    throw std::invalid_argument("missing signed asset string");
  std::uint64_t length = 0;
  const auto* bytes = plist_get_string_ptr(value, &length);
  if (!bytes || !length || length > 128) throw std::invalid_argument("invalid signed asset string");
  std::string result(bytes, static_cast<std::size_t>(length));
  if (result.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid signed asset string");
  return result;
}
}  // namespace
AcceptedAssetReference read_asset_reference(const std::string& path) {
  const auto bytes = read_file(path, 4096);
  std::istringstream lines(std::string(bytes.begin(), bytes.end()));
  std::map<std::string, std::string> fields;
  std::string line;
  while (std::getline(lines, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos ||
        !fields.emplace(line.substr(0, separator), line.substr(separator + 1)).second)
      throw std::invalid_argument("invalid acceptance reference field");
  }
  if (fields.size() != 4) throw std::invalid_argument("acceptance reference needs four fields");
  const auto parse_hash = [](const std::string& text) {
    if (text.size() != 64) throw std::invalid_argument("invalid reference hash length");
    std::array<std::uint8_t, 32> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
      unsigned byte = 0;
      const auto* start = text.data() + 2 * i;
      const auto [end, error] = std::from_chars(start, start + 2, byte, 16);
      if (error != std::errc{} || end != start + 2 || byte > 255)
        throw std::invalid_argument("invalid reference hash");
      result[i] = static_cast<std::uint8_t>(byte);
    }
    return result;
  };
  const auto& timestamp = fields.at("acceptedAt");
  std::uint64_t at = 0;
  const auto [end, error] =
      std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), at);
  if (error != std::errc{} || end != timestamp.data() + timestamp.size() || !at ||
      at > static_cast<std::uint64_t>(std::numeric_limits<std::time_t>::max()))
    throw std::invalid_argument("invalid acceptance time");
  return {parse_hash(fields.at("archiveSha256")), parse_hash(fields.at("certificateSha256")),
          parse_hash(fields.at("signatureSha256")), static_cast<std::time_t>(at)};
}
AssetBundle AssetBundle::open(const std::string& path, const std::string& signature_path,
                              const std::string& certificate_path, const std::string& root_path,
                              const AssetBundlePolicy& policy) {
  if (policy.identifier.empty() || policy.identifier.size() > 128)
    throw std::invalid_argument("asset identity policy is required");
  if (policy.certificate_time_policy != AssetCertificateTimePolicy::strict &&
      policy.certificate_time_policy != AssetCertificateTimePolicy::allow_expired_leaf)
    throw std::invalid_argument("invalid asset certificate time policy");
  AssetBundle bundle;
  const auto archive = read_file(path, 64 * 1024 * 1024);
  const auto signature = read_file(signature_path, 4096);
  const auto certificate = read_file(certificate_path, 32768);
  const auto root = read_file(root_path, 32768);
  std::array<std::uint8_t, 32> digest{};
  unsigned length = 0;
  if (EVP_Digest(archive.data(), archive.size(), digest.data(), &length, EVP_sha256(), nullptr) !=
          1 ||
      length != digest.size())
    throw std::runtime_error("asset snapshot hash failed");
  const std::string_view certificate_text(reinterpret_cast<const char*>(certificate.data()),
                                          certificate.size());
  const std::string_view root_text(reinterpret_cast<const char*>(root.data()), root.size());
  auto verified = policy.previously_accepted
                      ? verify_previously_accepted_asset(digest, signature, certificate_text,
                                                         root_text, *policy.previously_accepted)
                      : verify_asset_signature(digest, signature, certificate_text, root_text);
  if (!policy.previously_accepted && verified == AssetSignatureResult::invalid_certificate_time &&
      policy.certificate_time_policy == AssetCertificateTimePolicy::allow_expired_leaf) {
    verified = verify_asset_signature(digest, signature, certificate_text, root_text,
                                      policy.certificate_time_policy);
    bundle.info_.expired_signer_exception = verified == AssetSignatureResult::valid;
  }
  if (verified != AssetSignatureResult::valid)
    throw std::invalid_argument("asset signing trust or acceptance receipt rejected");
  Reader reader(archive);
  archive_entry* entry = nullptr;
  std::set<std::string> paths, regular;
  Bytes metadata;
  int result;
  while ((result = archive_read_next_header(reader.value, &entry)) == ARCHIVE_OK) {
    if (paths.size() >= 8192) throw std::invalid_argument("too many archive entries");
    const auto name = path_name(entry);
    if (!paths.insert(name).second || archive_entry_hardlink(entry) ||
        archive_entry_symlink(entry) || archive_entry_is_encrypted(entry) != 0)
      throw std::invalid_argument("duplicate, linked or encrypted archive entry");
    const auto type = archive_entry_filetype(entry);
    const auto size = archive_entry_size(entry);
    if (size < 0 || static_cast<std::uint64_t>(size) > file_limit)
      throw std::invalid_argument("archive entry exceeds limit");
    if (type == AE_IFDIR) {
      if (size != 0) throw std::invalid_argument("directory contains data");
      continue;
    }
    if (type != AE_IFREG) throw std::invalid_argument("unsupported archive entry type");
    if (static_cast<std::size_t>(size) > 256 * 1024 * 1024 - bundle.info_.expanded_bytes)
      throw std::invalid_argument("expanded archive exceeds limit");
    bundle.info_.expanded_bytes += static_cast<std::size_t>(size);
    auto bytes = contents(reader.value, static_cast<std::size_t>(size));
    if (name == "Info.plist") {
      if (bytes.empty() || bytes.size() > 1024 * 1024)
        throw std::invalid_argument("invalid signed metadata size");
      metadata = bytes;
    }
    // Admission already reads/checks every entry. Keep those exact bytes rather
    // than repeating ZIP traversal when a deferred animation needs a resource.
    bundle.resources_.emplace(name, std::move(bytes));
    regular.insert(name);
  }
  if (result != ARCHIVE_EOF || metadata.empty())
    throw std::invalid_argument("invalid ZIP or missing signed metadata");
  for (const auto& name : paths) {
    for (auto slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1))
      if (regular.contains(name.substr(0, slash)))
        throw std::invalid_argument("archive file/directory conflict");
  }
  plist_t raw = nullptr;
  if (plist_from_memory(reinterpret_cast<const char*>(metadata.data()),
                        static_cast<std::uint32_t>(metadata.size()), &raw,
                        nullptr) != PLIST_ERR_SUCCESS ||
      !raw)
    throw std::invalid_argument("invalid signed metadata plist");
  std::unique_ptr<void, decltype(&plist_free)> info(raw, plist_free);
  if (plist_get_node_type(raw) != PLIST_DICT)
    throw std::invalid_argument("signed metadata is not a dictionary");
  bundle.info_.identifier = string(raw, "CFBundleIdentifier");
  bundle.info_.minimum_sdk = string(raw, "MinimumSDKVersion");
  auto version = plist_dict_get_item(raw, "AccessoryContentVersion");
  if (!version || plist_get_node_type(version) != PLIST_UINT)
    throw std::invalid_argument("missing signed asset version");
  plist_get_uint_val(version, &bundle.info_.version);
  if (bundle.info_.identifier != policy.identifier || bundle.info_.version != policy.version)
    throw std::invalid_argument("signed asset identity or version differs from policy");
  bundle.info_.files = bundle.resources_.size();
  return bundle;
}
std::vector<std::uint8_t> AssetBundle::resource(const std::string& name) const {
  const auto wanted = resources_.find(name);
  if (wanted == resources_.end()) throw std::invalid_argument("unknown asset resource");
  return wanted->second;
}
}  // namespace headunit
