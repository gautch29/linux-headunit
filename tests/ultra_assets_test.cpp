// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/ultra_asset_transfer.hpp"
#include "headunit/asset_signature.hpp"
#include "headunit/opack.hpp"
#include "headunit/asset_bundle.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Value = headunit::OpackValue;
using Dict = Value::Dictionary;
using Bytes = Value::Bytes;
using Transfer = headunit::UltraAssetTransfer;
using Result = headunit::AssetSignatureResult;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
void check(bool value) {
  if (!value) throw std::runtime_error("Ultra asset check failed");
}
Key key() {
  return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free);
}
void extension(X509* cert, X509* issuer, int nid, const char* value) {
  X509V3_CTX context{};
  X509V3_set_ctx(&context, issuer, cert, nullptr, nullptr, 0);
  std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> ext(
      X509V3_EXT_conf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
  check(ext && X509_add_ext(cert, ext.get(), -1) == 1);
}
Cert certificate(EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key, bool ca = false,
                 bool expired = false, bool signing = true, bool future = false) {
  Cert cert(X509_new(), X509_free);
  check(cert && X509_set_version(cert.get(), 2) == 1);
  check(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), ca ? 1 : 2) == 1);
  check(X509_gmtime_adj(X509_getm_notBefore(cert.get()), future ? 3600 : -86400) != nullptr);
  check(X509_gmtime_adj(X509_getm_notAfter(cert.get()), expired ? -3600 : 86400) != nullptr);
  check(X509_set_pubkey(cert.get(), key) == 1);
  std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> name(X509_NAME_new(), X509_NAME_free);
  check(name != nullptr);
  check(X509_NAME_add_entry_by_txt(
            name.get(), "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(ca ? "Synthetic Root" : "Synthetic Theme"), -1,
            -1, 0) == 1);
  check(X509_set_subject_name(cert.get(), name.get()) == 1);
  check(X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer) : name.get()) == 1);
  extension(cert.get(), issuer ? issuer : cert.get(), NID_basic_constraints,
            ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
  extension(cert.get(), issuer ? issuer : cert.get(), NID_key_usage,
            ca        ? "critical,keyCertSign,cRLSign"
            : signing ? "critical,digitalSignature"
                      : "critical,keyAgreement");
  extension(cert.get(), issuer ? issuer : cert.get(), NID_subject_key_identifier, "hash");
  if (issuer) extension(cert.get(), issuer, NID_authority_key_identifier, "keyid:always");
  check(X509_sign(cert.get(), issuer_key ? issuer_key : key, EVP_sha256()) > 0);
  return cert;
}
std::string pem(X509* cert) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
  check(bio && PEM_write_bio_X509(bio.get(), cert) == 1);
  char* data = nullptr;
  const auto length = BIO_get_mem_data(bio.get(), &data);
  check(length > 0);
  return {data, static_cast<std::size_t>(length)};
}
std::array<std::uint8_t, 32> digest(const Bytes& value) {
  std::array<std::uint8_t, 32> out{};
  unsigned length = 0;
  check(EVP_Digest(value.data(), value.size(), out.data(), &length, EVP_sha256(), nullptr) == 1);
  check(length == out.size());
  return out;
}
Bytes sign(EVP_PKEY* key, const std::array<std::uint8_t, 32>& digest) {
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
      EVP_PKEY_CTX_new(key, nullptr), EVP_PKEY_CTX_free);
  check(context && EVP_PKEY_sign_init(context.get()) == 1);
  check(EVP_PKEY_CTX_set_signature_md(context.get(), EVP_sha256()) == 1);
  std::size_t length = 0;
  check(EVP_PKEY_sign(context.get(), nullptr, &length, digest.data(), digest.size()) == 1);
  Bytes result(length);
  check(EVP_PKEY_sign(context.get(), result.data(), &length, digest.data(), digest.size()) == 1);
  result.resize(length);
  return result;
}
std::string hex(const std::array<std::uint8_t, 32>& digest) {
  std::string out;
  for (auto byte : digest) {
    out += "0123456789abcdef"[byte >> 4];
    out += "0123456789abcdef"[byte & 15];
  }
  return out;
}
struct ZipEntry {
  std::string name;
  Bytes data;
  bool link = false;
};
Bytes zip(const std::vector<ZipEntry>& entries) {
  std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(),
                                                                 archive_write_free);
  check(writer && archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
  Bytes output(2 * 1024 * 1024);
  std::size_t used = 0;
  check(archive_write_open_memory(writer.get(), output.data(), output.size(), &used) == ARCHIVE_OK);
  for (const auto& source : entries) {
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(),
                                                                        archive_entry_free);
    archive_entry_set_pathname(entry.get(), source.name.c_str());
    archive_entry_set_filetype(entry.get(), source.link ? AE_IFLNK : AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(source.data.size()));
    if (source.link) archive_entry_set_symlink(entry.get(), "main.lua");
    check(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    if (!source.link)
      check(archive_write_data(writer.get(), source.data.data(), source.data.size()) ==
            static_cast<la_ssize_t>(source.data.size()));
  }
  check(archive_write_close(writer.get()) == ARCHIVE_OK);
  output.resize(used);
  return output;
}
struct Temp {
  std::filesystem::path path;
  Temp() {
    char name[] = "/tmp/ultra-assets-test-XXXXXX";
    const auto* made = mkdtemp(name);
    check(made != nullptr);
    path = made;
  }
  ~Temp() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::string put(const char* name, const Bytes& bytes) {
    auto file = path / name;
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    check(out.good());
    return file.string();
  }
};
Bytes bytes(const std::string& value) {
  return {value.begin(), value.end()};
}
std::int64_t status(const Transfer::Reply& reply) {
  check(reply.has_value());
  const auto value = headunit::decode_opack(*reply);
  const auto& dict = std::get<Dict>(value.value);
  check(dict.size() == 2 && std::get<std::int64_t>(dict.at("messageType").value) == 5);
  return std::get<std::int64_t>(dict.at("accepted").value);
}
struct Sink : headunit::AssetSink {
  Bytes stored;
  std::string root;
  unsigned begins = 0, commits = 0, rollbacks = 0;
  bool committed = false;
  int fail_at = 0;
  void begin(const headunit::AssetHeader&) override {
    ++begins;
    if (fail_at == 1) throw std::runtime_error("storage unavailable");
  }
  void append(std::span<const std::uint8_t> bytes) override {
    if (fail_at == 2) throw std::runtime_error("disk full");
    stored.insert(stored.end(), bytes.begin(), bytes.end());
  }
  bool commit(const headunit::AssetHeader& header,
              const std::array<std::uint8_t, 32>& hash) override {
    ++commits;
    if (fail_at == 3) throw std::runtime_error("commit failed");
    const std::string chain(header.certificate.begin(), header.certificate.end());
    // Synthetic sink only: a real installer must also inspect signed ZIP metadata.
    committed =
        fail_at != 4 && header.additions.empty() &&
        headunit::verify_asset_signature(hash, header.signature, chain, root) == Result::valid;
    return committed;
  }
  void rollback() noexcept override {
    ++rollbacks;
    stored.clear();
  }
};
}  // namespace
int main() {
  auto root_key = key(), leaf_key = key(), other_key = key();
  check(root_key && leaf_key && other_key);
  auto root_cert = certificate(root_key.get(), nullptr, nullptr, true);
  auto other_root = certificate(other_key.get(), nullptr, nullptr, true);
  auto leaf = certificate(leaf_key.get(), root_cert.get(), root_key.get());
  auto expired = certificate(leaf_key.get(), root_cert.get(), root_key.get(), false, true);
  auto wrong_usage =
      certificate(leaf_key.get(), root_cert.get(), root_key.get(), false, false, false);
  const auto root = pem(root_cert.get());
  const auto chain = pem(leaf.get()) + root;
  const Bytes archive{'a', 'b', 'c'};
  const auto hash = digest(archive);
  check(hex(hash) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto signature = sign(leaf_key.get(), hash);
  check(headunit::verify_asset_signature(hash, signature, chain, root) == Result::valid);
  check(headunit::verify_asset_signature(hash, signature, chain, pem(other_root.get())) ==
        Result::untrusted_chain);
  check(headunit::verify_asset_signature(hash, signature, pem(expired.get()), root) ==
        Result::invalid_certificate_time);
  check(headunit::verify_asset_signature(hash, signature, pem(wrong_usage.get()), root) ==
        Result::invalid_signer_usage);
  check(headunit::verify_asset_signature(hash, signature, root, root) ==
        Result::invalid_signer_usage);
  auto changed_hash = hash;
  changed_hash[0] ^= 1;
  check(headunit::verify_asset_signature(changed_hash, signature, chain, root) ==
        Result::invalid_signature);
  auto changed_signature = signature;
  changed_signature.back() ^= 1;
  check(headunit::verify_asset_signature(hash, changed_signature, chain, root) ==
        Result::invalid_signature);
  check(headunit::verify_asset_signature(hash, signature, chain + "garbage", root) ==
        Result::malformed);
  check(headunit::verify_asset_signature(hash, signature, chain, chain) == Result::malformed);

  // Development mode relaxes only an expired leaf, never authentication or
  // issuer validity. Generate signing material independently of the verifier.
  constexpr auto relaxed = headunit::AssetCertificateTimePolicy::allow_expired_leaf;
  check(headunit::verify_asset_signature(hash, signature, pem(expired.get()), root, relaxed) ==
        Result::valid);
  check(headunit::verify_asset_signature(changed_hash, signature, pem(expired.get()), root,
                                         relaxed) == Result::invalid_signature);
  check(headunit::verify_asset_signature(hash, changed_signature, pem(expired.get()), root,
                                         relaxed) == Result::invalid_signature);
  check(headunit::verify_asset_signature(hash, signature, pem(expired.get()), pem(other_root.get()),
                                         relaxed) == Result::untrusted_chain);
  check(headunit::verify_asset_signature(hash, signature, pem(wrong_usage.get()), root, relaxed) ==
        Result::invalid_signer_usage);
  check(headunit::verify_asset_signature(hash, signature, pem(expired.get()) + "garbage", root,
                                         relaxed) == Result::malformed);
  check(headunit::verify_asset_signature(hash, signature, chain, root,
                                         static_cast<headunit::AssetCertificateTimePolicy>(255)) ==
        Result::malformed);
  auto future_leaf =
      certificate(leaf_key.get(), root_cert.get(), root_key.get(), false, false, true, true);
  check(headunit::verify_asset_signature(hash, signature, pem(future_leaf.get()), root, relaxed) ==
        Result::invalid_certificate_time);
  auto reversed_time =
      certificate(leaf_key.get(), root_cert.get(), root_key.get(), false, true, true, true);
  check(headunit::verify_asset_signature(hash, signature, pem(reversed_time.get()), root,
                                         relaxed) == Result::invalid_certificate_time);
  auto malformed_time = certificate(leaf_key.get(), root_cert.get(), root_key.get(), false, true);
  check(ASN1_STRING_set(X509_getm_notBefore(malformed_time.get()), "invalid", 7) == 1);
  check(X509_sign(malformed_time.get(), root_key.get(), EVP_sha256()) > 0);
  check(headunit::verify_asset_signature(hash, signature, pem(malformed_time.get()), root,
                                         relaxed) == Result::malformed);
  auto bad_leaf_signature =
      certificate(leaf_key.get(), root_cert.get(), other_key.get(), false, true);
  check(headunit::verify_asset_signature(hash, signature, pem(bad_leaf_signature.get()), root,
                                         relaxed) == Result::untrusted_chain);
  auto expired_root = certificate(root_key.get(), nullptr, nullptr, true, true);
  check(headunit::verify_asset_signature(hash, signature, pem(expired.get()),
                                         pem(expired_root.get()),
                                         relaxed) == Result::invalid_certificate_time);
  auto future_root = certificate(root_key.get(), nullptr, nullptr, true, false, true, true);
  check(headunit::verify_asset_signature(hash, signature, pem(expired.get()),
                                         pem(future_root.get()),
                                         relaxed) == Result::invalid_certificate_time);
  auto intermediate_key = key();
  auto expired_intermediate =
      certificate(intermediate_key.get(), root_cert.get(), root_key.get(), true, true);
  auto intermediate_leaf =
      certificate(leaf_key.get(), expired_intermediate.get(), intermediate_key.get(), false, true);
  check(headunit::verify_asset_signature(
            hash, signature, pem(intermediate_leaf.get()) + pem(expired_intermediate.get()), root,
            relaxed) == Result::invalid_certificate_time);

  const auto expired_chain = pem(expired.get()) + root;
  headunit::AcceptedAssetReference accepted{hash, digest(bytes(expired_chain)), digest(signature),
                                            std::time(nullptr) - 7200};
  const auto cached = [&](const headunit::AcceptedAssetReference& reference) {
    return headunit::verify_previously_accepted_asset(hash, signature, expired_chain, root,
                                                      reference);
  };
  check(cached(accepted) == Result::valid);
  auto bad_reference = accepted;
  bad_reference.archive_digest[0] ^= 1;
  check(cached(bad_reference) == Result::malformed);
  bad_reference = accepted;
  bad_reference.certificate_digest[0] ^= 1;
  check(cached(bad_reference) == Result::malformed);
  bad_reference = accepted;
  bad_reference.signature_digest[0] ^= 1;
  check(cached(bad_reference) == Result::malformed);
  bad_reference = accepted;
  bad_reference.accepted_at = std::time(nullptr) + 60;
  check(cached(bad_reference) == Result::malformed);
  bad_reference = accepted;
  bad_reference.accepted_at -= 3 * 86400;
  check(cached(bad_reference) == Result::invalid_certificate_time);
  bad_reference = accepted;
  bad_reference.signature_digest = digest(changed_signature);
  check(headunit::verify_previously_accepted_asset(hash, changed_signature, expired_chain, root,
                                                   bad_reference) == Result::invalid_signature);

  const std::string metadata = R"(<?xml version="1.0"?><plist version="1.0"><dict>
    <key>CFBundleIdentifier</key><string>synthetic-theme</string>
    <key>AccessoryContentVersion</key><integer>7</integer>
    <key>MinimumSDKVersion</key><string>19.0</string></dict></plist>)";
  Temp fixture;
  const auto root_path = fixture.put("root.pem", bytes(root));
  const auto chain_path = fixture.put("chain.pem", bytes(chain));
  headunit::AssetBundlePolicy policy{"synthetic-theme", 7, {}};
  const std::vector<ZipEntry> files{{"Info.plist", bytes(metadata)},
                                    {"main.lua", bytes("return 1")}};
  const auto open_zip = [&](const std::vector<ZipEntry>& entries) {
    const auto zip_bytes = zip(entries);
    const auto archive_path = fixture.put("archive.zip", zip_bytes);
    const auto sig_path = fixture.put("signature.der", sign(leaf_key.get(), digest(zip_bytes)));
    return headunit::AssetBundle::open(archive_path, sig_path, chain_path, root_path, policy);
  };
  auto bundle = open_zip(files);
  check(bundle.info().identifier == "synthetic-theme" && bundle.info().version == 7 &&
        bundle.info().minimum_sdk == "19.0" && bundle.info().files == 2 &&
        !bundle.info().expired_signer_exception);
  policy.certificate_time_policy = relaxed;
  check(!open_zip(files).info().expired_signer_exception);  // Opt-in is not evidence of expiry.
  policy.certificate_time_policy = headunit::AssetCertificateTimePolicy::strict;
  std::filesystem::create_symlink(fixture.path / "archive.zip", fixture.path / "linked.zip");
  bool link_rejected = false;
  try {
    (void)headunit::AssetBundle::open((fixture.path / "linked.zip").string(),
                                      (fixture.path / "signature.der").string(), chain_path,
                                      root_path, policy);
  } catch (const std::invalid_argument&) {
    link_rejected = true;
  }
  check(link_rejected);
  fixture.put("archive.zip", bytes("replaced on disk"));
  check(bundle.resource("main.lua") == bytes("return 1"));  // Immutable verified snapshot.
  auto mutable_copy = bundle.resource("main.lua");
  mutable_copy.front() = 'X';
  check(bundle.resource("main.lua") == bytes("return 1"));
  check(bundle.resource("Info.plist") == bytes(metadata));
  bool missing = false;
  try {
    (void)bundle.resource("../main.lua");
  } catch (const std::invalid_argument&) {
    missing = true;
  }
  check(missing);
  for (int invalid = 0; invalid < 6; ++invalid) {
    auto bad_files = files;
    if (invalid == 0) bad_files.push_back({"../escape", {1}});
    if (invalid == 1) bad_files.push_back({"/absolute", {1}});
    if (invalid == 2) bad_files.push_back({"main.lua", {1}});
    if (invalid == 3) bad_files.push_back({"linked", {}, true});
    if (invalid == 4) bad_files.push_back({"main.lua/child", {1}});
    if (invalid == 5) bad_files.erase(bad_files.begin());
    bool rejected = false;
    try {
      (void)open_zip(bad_files);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected);
  }
  policy.version = 8;
  bool wrong_version = false;
  try {
    (void)open_zip(files);
  } catch (const std::invalid_argument&) {
    wrong_version = true;
  }
  check(wrong_version);
  policy.version = 7;
  fixture.put("chain.pem", bytes(expired_chain));
  bool expired_fresh = false;
  try {
    (void)open_zip(files);
  } catch (const std::invalid_argument&) {
    expired_fresh = true;
  }
  check(expired_fresh);
  policy.certificate_time_policy = relaxed;
  const auto development = open_zip(files);
  check(development.info().expired_signer_exception &&
        development.resource("main.lua") == bytes("return 1"));
  auto unsafe_files = files;
  unsafe_files.push_back({"../escape", {1}});
  bool unsafe_relaxed = false;
  try {
    (void)open_zip(unsafe_files);
  } catch (const std::invalid_argument&) {
    unsafe_relaxed = true;
  }
  check(unsafe_relaxed);
  policy.version = 8;
  bool identity_relaxed = false;
  try {
    (void)open_zip(files);
  } catch (const std::invalid_argument&) {
    identity_relaxed = true;
  }
  check(identity_relaxed);
  policy.version = 7;
  // Turning the option off restores fresh-expiry rejection.
  policy.certificate_time_policy = headunit::AssetCertificateTimePolicy::strict;
  bool strict_again = false;
  try {
    (void)open_zip(files);
  } catch (const std::invalid_argument&) {
    strict_again = true;
  }
  check(strict_again);
  const auto installed_bytes = zip(files);
  const auto installed_signature = sign(leaf_key.get(), digest(installed_bytes));
  policy.previously_accepted =
      headunit::AcceptedAssetReference{digest(installed_bytes), digest(bytes(expired_chain)),
                                       digest(installed_signature), accepted.accepted_at};
  auto installed = headunit::AssetBundle::open(fixture.put("archive.zip", installed_bytes),
                                               fixture.put("signature.der", installed_signature),
                                               chain_path, root_path, policy);
  check(installed.resource("main.lua") == bytes("return 1"));
  check(!installed.info().expired_signer_exception);
  policy.certificate_time_policy = relaxed;
  policy.previously_accepted->archive_digest[0] ^= 1;
  bool bad_pin_relaxed = false;
  try {
    (void)headunit::AssetBundle::open((fixture.path / "archive.zip").string(),
                                      (fixture.path / "signature.der").string(), chain_path,
                                      root_path, policy);
  } catch (const std::invalid_argument&) {
    bad_pin_relaxed = true;
  }
  check(bad_pin_relaxed);  // A supplied receipt cannot fall back to development mode.

  const auto header = [&] {
    return Dict{{"messageType", Value{std::int64_t{1}}},
                {"payloadType", Value{std::int64_t{1}}},
                {"sessionID", Value{std::string("test-session")}},
                {"version", Value{std::int64_t{985}}},
                {"chunkCount", Value{std::int64_t{2}}},
                {"certificate", Value{Bytes(chain.begin(), chain.end())}},
                {"signature", Value{signature}}};
  };
  const auto chunk = [&](std::int64_t index) {
    Dict result{{"messageType", Value{std::int64_t{2}}},
                {"sessionID", Value{std::string("test-session")}},
                {"chunkIndex", Value{index}},
                {"data", Value{index == 0 ? Bytes{'a'} : Bytes{'b', 'c'}}}};
    if (index == 1) result.emplace("checksum", Value{hex(hash)});
    return result;
  };
  const auto wire = [](Dict dict) {
    return headunit::encode_opack(Value{std::move(dict)});
  };
  const auto now = Transfer::Clock::now();
  {
    Sink sink;
    sink.root = root;
    Transfer transfer(sink, now);
    check(!transfer.receive(wire(header()), now));
    check(!transfer.receive(wire(chunk(0)), now));
    check(sink.commits == 0);
    check(status(transfer.receive(wire(chunk(1)), now)) == 0);
    check(sink.committed && sink.stored == archive && sink.rollbacks == 0);
    check(transfer.state() == Transfer::State::complete && transfer.received_bytes() == 3);
    check(!transfer.receive(wire(header()), now));
  }
  for (int failure = 1; failure <= 4; ++failure) {
    Sink sink;
    sink.root = root;
    sink.fail_at = failure;
    Transfer transfer(sink, now);
    auto reply = transfer.receive(wire(header()), now);
    if (!reply) reply = transfer.receive(wire(chunk(0)), now);
    if (!reply) reply = transfer.receive(wire(chunk(1)), now);
    check(status(reply) == 5 && sink.rollbacks == 1 && !sink.committed);
    transfer.disconnect();
    check(sink.rollbacks == 1);
  }
  for (int failure = 0; failure < 6; ++failure) {
    Sink sink;
    sink.root = root;
    Transfer transfer(sink, now);
    check(!transfer.receive(wire(header()), now));
    auto bad = chunk(0);
    if (failure == 0) bad["sessionID"] = Value{std::string("different")};
    if (failure == 1) bad["chunkIndex"] = Value{std::int64_t{1}};
    if (failure == 2) bad["data"] = Value{Bytes(256 * 1024 + 1)};
    if (failure == 3) bad["checksum"] = Value{hex(hash)};
    if (failure == 4) bad["chunkIndex"] = Value{false};
    if (failure == 5) bad["data"] = Value{Bytes{}};
    check(status(transfer.receive(wire(bad), now)) == 5 && sink.rollbacks == 1);
  }
  for (int failure = 0; failure < 4; ++failure) {
    Sink sink;
    sink.root = root;
    Transfer transfer(sink, now);
    check(!transfer.receive(wire(header()), now));
    check(!transfer.receive(wire(chunk(0)), now));
    auto final = chunk(1);
    if (failure == 0) final.erase("checksum");
    if (failure == 1) final["checksum"] = Value{std::string(64, '0')};
    if (failure == 2) final["checksum"] = Value{std::string(64, 'g')};
    if (failure == 3) final["checksum"] = Value{std::string(63, '0')};
    check(status(transfer.receive(wire(final), now)) == 5 && sink.commits == 0);
  }
  {
    Sink sink;
    sink.root = root;
    {
      Transfer transfer(sink, now);
      check(!transfer.receive(wire(header()), now));
    }
    check(sink.rollbacks == 1);  // Disconnect/destruction removes staging.
  }
  for (int kind = 0; kind < 3; ++kind) {
    Sink sink;
    sink.root = root;
    Transfer transfer(sink, now);
    if (kind != 0) check(!transfer.receive(wire(header()), now));
    check(status(transfer.poll(kind == 2 ? now - std::chrono::seconds(1)
                                         : now + std::chrono::seconds(30))) == 5);
    check(sink.rollbacks == (kind == 0 ? 0u : 1u));
  }
  {
    Sink sink;
    sink.root = root;
    Transfer transfer(sink, now);
    auto many = header();
    many["chunkCount"] = Value{std::int64_t{100}};
    check(!transfer.receive(wire(many), now));
    for (int index = 0; index < 10; ++index) {
      auto part = chunk(0);
      part["chunkIndex"] = Value{static_cast<std::int64_t>(index)};
      check(!transfer.receive(wire(part), now + std::chrono::seconds(29 * (index + 1))));
    }
    check(status(transfer.poll(now + std::chrono::seconds(300))) == 5);
  }
  for (int kind = 0; kind < 4; ++kind) {
    Sink sink;
    sink.root = root;
    Transfer transfer(sink, now);
    auto bad = header();
    if (kind == 0) bad.erase("certificate");
    if (kind == 1) bad["chunkCount"] = Value{std::int64_t{0}};
    if (kind == 2) bad["chunkCount"] = Value{std::int64_t{4097}};
    if (kind == 3) bad["version"] = Value{true};
    check(status(transfer.receive(wire(bad), now)) == 5 && sink.begins == 0);
  }
  {
    struct CountingSink : Sink {
      void append(std::span<const std::uint8_t>) override {}
    } sink;
    Transfer transfer(sink, now);
    auto many = header();
    many["chunkCount"] = Value{std::int64_t{258}};
    check(!transfer.receive(wire(many), now));
    auto part = chunk(0);
    part["data"] = Value{Bytes(256 * 1024)};
    for (int index = 0; index < 256; ++index) {
      part["chunkIndex"] = Value{static_cast<std::int64_t>(index)};
      check(!transfer.receive(wire(part), now));
    }
    part["chunkIndex"] = Value{std::int64_t{256}};
    check(status(transfer.receive(wire(part), now)) == 5);
    check(sink.rollbacks == 1 && sink.commits == 0 &&
          transfer.received_bytes() == 64 * 1024 * 1024);
  }
  return 0;
}
