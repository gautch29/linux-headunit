// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/software_mfi_authenticator.hpp"
#include "headunit/iap2_mfi_auth.hpp"
#include "headunit/iap2_parameter.hpp"
#include "headunit/airplay_auth_setup.hpp"
#include "headunit/hap_crypto.hpp"
#include "mfi_test_identity.hpp"

#include <openssl/rand.h>

#include <iostream>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <class F>
void rejects(F&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "invalid software MFi input accepted");
}
std::uint32_t be32(std::span<const std::uint8_t> bytes) {
  return std::uint32_t(bytes[0]) << 24 | std::uint32_t(bytes[1]) << 16 |
         std::uint32_t(bytes[2]) << 8 | bytes[3];
}
std::array<std::uint8_t, 20> aes_material(std::string_view label,
                                          std::span<const std::uint8_t> secret) {
  std::vector<std::uint8_t> input(label.begin(), label.end());
  input.insert(input.end(), secret.begin(), secret.end());
  std::array<std::uint8_t, 20> result{};
  unsigned size = 0;
  check(EVP_Digest(input.data(), input.size(), result.data(), &size, EVP_sha1(), nullptr) == 1 &&
            size == 20,
        "AES fixture derivation failed");
  return result;
}
}  // namespace

int main() try {
  mfi_test::Identity identity;
  const auto key_path = (identity.directory / "identity.pk8").string();
  const auto certificate_path = (identity.directory / "certificate.p7b").string();
  headunit::SoftwareMfiAuthenticator auth(key_path, certificate_path);
  check(auth.protocol_major() == 3 && auth.read_certificate() == identity.container,
        "software provider altered the certificate wire bytes");
  auto copy = auth.read_certificate();
  copy[0] ^= 1;
  check(auth.read_certificate() == identity.container, "certificate ownership leaked");
  std::array<std::uint8_t, 32> digest{};
  for (unsigned i = 0; i < 16; ++i) {
    check(RAND_bytes(digest.data(), digest.size()) == 1, "fixture randomness failed");
    const auto signature = auth.sign_digest(digest);
    check(identity.verify(digest, signature), "prehashed software signature failed");
    digest[0] ^= 1;
    check(!identity.verify(digest, signature), "signature accepted another challenge");
  }
  rejects([&] { (void)auth.sign_digest(digest, std::chrono::milliseconds(0)); });
  rejects([&] { (void)auth.sign_digest(digest, std::chrono::milliseconds(2001)); });

  headunit::Iap2MfiAuth iap(auth);
  const auto certificate = iap.on_message({0xaa00, {}});
  check(certificate && headunit::decode_iap2_parameters(certificate->parameters)[0].value ==
                           identity.container,
        "software iAP2 certificate missing");
  const std::array<headunit::Iap2Parameter, 1> challenge{{{0, {digest.begin(), digest.end()}}}};
  const auto response = iap.on_message({0xaa02, headunit::encode_iap2_parameters(challenge)});
  check(
      response && response->identifier == 0xaa03 &&
          identity.verify(digest, headunit::decode_iap2_parameters(response->parameters)[0].value),
      "software iAP2 challenge response failed");
  (void)iap.on_message({0xaa04, {}});
  check(iap.state() == headunit::Iap2MfiAuth::State::Failed,
        "phone rejection did not fail authentication");
  rejects([&] { (void)iap.on_message({0xaa05, {}}); });
  iap.reset();
  (void)iap.on_message({0xaa00, {}});
  (void)iap.on_message({0xaa02, headunit::encode_iap2_parameters(challenge)});
  (void)iap.on_message({0xaa05, {}});
  check(iap.state() == headunit::Iap2MfiAuth::State::Complete, "fresh software exchange failed");

  headunit::AirplayAuthSetup sap(auth);
  headunit::HapX25519Ephemeral controller;
  const auto public_key = controller.public_key();
  std::array<std::uint8_t, 33> request{};
  request[0] = 1;
  std::copy(public_key.begin(), public_key.end(), request.begin() + 1);
  const auto reply = sap.respond(request);
  check(be32(std::span(reply).subspan(32, 4)) == identity.container.size() &&
            std::equal(identity.container.begin(), identity.container.end(), reply.begin() + 36),
        "software auth-setup certificate differs");
  headunit::HapKey32 accessory_key{};
  std::copy_n(reply.begin(), 32, accessory_key.begin());
  const auto secret = controller.shared_secret(accessory_key);
  check(secret.has_value(), "auth-setup shared secret missing");
  const auto aes_key = aes_material("AES-KEY", *secret), iv = aes_material("AES-IV", *secret);
  const auto signature_offset = 36 + identity.container.size();
  check(be32(std::span(reply).subspan(signature_offset, 4)) == 64 &&
            reply.size() == signature_offset + 68,
        "auth-setup signature bounds differ");
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> decryptor(EVP_CIPHER_CTX_new(),
                                                                            EVP_CIPHER_CTX_free);
  std::array<std::uint8_t, 64> signature{};
  int written = 0, final = 0;
  check(decryptor &&
            EVP_DecryptInit_ex(decryptor.get(), EVP_aes_128_ctr(), nullptr, aes_key.data(),
                               iv.data()) == 1 &&
            EVP_DecryptUpdate(decryptor.get(), signature.data(), &written,
                              reply.data() + signature_offset + 4, 64) == 1 &&
            EVP_DecryptFinal_ex(decryptor.get(), signature.data() + written, &final) == 1 &&
            written + final == 64,
        "auth-setup signature could not decrypt");
  std::array<std::uint8_t, 64> joined{};
  std::copy(accessory_key.begin(), accessory_key.end(), joined.begin());
  std::copy(public_key.begin(), public_key.end(), joined.begin() + 32);
  unsigned size = 0;
  check(
      EVP_Digest(joined.data(), joined.size(), digest.data(), &size, EVP_sha256(), nullptr) == 1 &&
          size == 32 && identity.verify(digest, signature),
      "auth-setup signature covers the wrong keys");

  headunit::SoftwareMfiAuthenticator direct(key_path,
                                            (identity.directory / "certificate.der").string());
  check(direct.read_certificate() == identity.certificate, "DER certificate bytes changed");
  mfi_test::Identity other;
  rejects([&] {
    headunit::SoftwareMfiAuthenticator bad((other.directory / "identity.pk8").string(),
                                           certificate_path);
  });
  mfi_test::Identity wrong_curve(NID_secp384r1);
  rejects([&] {
    headunit::SoftwareMfiAuthenticator bad((wrong_curve.directory / "identity.pk8").string(),
                                           (wrong_curve.directory / "certificate.p7b").string());
  });
  rejects([&] { headunit::SoftwareMfiAuthenticator bad(key_path + ".missing", certificate_path); });
  chmod(key_path.c_str(), 0644);
  rejects([&] { headunit::SoftwareMfiAuthenticator bad(key_path, certificate_path); });
  chmod(key_path.c_str(), 0600);
  const auto link = identity.directory / "linked.pk8";
  std::filesystem::create_symlink(key_path, link);
  rejects([&] { headunit::SoftwareMfiAuthenticator bad(link.string(), certificate_path); });
  std::filesystem::remove(link);
  std::filesystem::create_hard_link(key_path, link);
  rejects([&] { headunit::SoftwareMfiAuthenticator bad(key_path, certificate_path); });
  std::filesystem::remove(link);
  mkfifo(link.c_str(), 0600);
  rejects([&] { headunit::SoftwareMfiAuthenticator bad(link.string(), certificate_path); });
  std::filesystem::remove(link);
  for (const auto& bytes : {std::vector<std::uint8_t>{}, std::vector<std::uint8_t>{1, 2, 3},
                            std::vector<std::uint8_t>(16385, 0)}) {
    mfi_test::write(key_path, bytes);
    rejects([&] { headunit::SoftwareMfiAuthenticator bad(key_path, certificate_path); });
  }
  auto trailing = identity.encoded_key;
  trailing.push_back(0);
  mfi_test::write(key_path, trailing);
  rejects([&] { headunit::SoftwareMfiAuthenticator bad(key_path, certificate_path); });
  mfi_test::write(key_path, identity.encoded_key);
  for (auto bytes : {identity.certificate, identity.container}) {
    bytes.push_back(0);
    mfi_test::write(certificate_path, bytes);
    rejects([&] { headunit::SoftwareMfiAuthenticator bad(key_path, certificate_path); });
  }
  std::filesystem::remove(key_path);
  std::filesystem::remove(certificate_path);
  check(identity.verify(digest, auth.sign_digest(digest)),
        "loaded provider reread missing runtime files");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
