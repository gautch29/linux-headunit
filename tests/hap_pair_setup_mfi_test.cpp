// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/mfi_coprocessor.hpp"
#include "headunit/hap_crypto.hpp"
#include "headunit/hap_pair_setup.hpp"
#include "headunit/hap_tlv.hpp"
#include "headunit/software_mfi_authenticator.hpp"
#include "mfi_test_identity.hpp"

#include <openssl/bn.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Bytes = std::span<const std::uint8_t>;
using Bn = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using Context = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>;
constexpr std::string_view test_secret = "123-45-678";

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

const headunit::HapTlv& field(const std::vector<headunit::HapTlv>& fields, std::uint8_t type) {
  for (const auto& item : fields)
    if (item.type == type) return item;
  throw std::runtime_error("expected pairing field missing");
}

std::array<std::uint8_t, 64> sha512(std::initializer_list<Bytes> parts) {
  std::vector<std::uint8_t> input;
  for (const auto part : parts)
    input.insert(input.end(), part.begin(), part.end());
  std::array<std::uint8_t, 64> result{};
  unsigned size = 0;
  check(EVP_Digest(input.data(), input.size(), result.data(), &size, EVP_sha512(), nullptr) == 1 &&
            size == result.size(),
        "SHA-512 failed");
  return result;
}

Bytes text_bytes(std::string_view value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

std::array<std::uint8_t, 384> pad(const BIGNUM* number) {
  std::array<std::uint8_t, 384> bytes{};
  check(BN_bn2binpad(number, bytes.data(), bytes.size()) == static_cast<int>(bytes.size()),
        "SRP padding failed");
  return bytes;
}

struct ClientProof {
  std::array<std::uint8_t, 384> public_key{};
  std::array<std::uint8_t, 64> proof{};
  std::array<std::uint8_t, 64> server_proof{};
  std::array<std::uint8_t, 64> session_key{};
};

ClientProof make_client_proof(const std::vector<headunit::HapTlv>& m2) {
  const auto& salt = field(m2, 2).value;
  const auto& server_key = field(m2, 3).value;
  Context context(BN_CTX_new(), BN_CTX_free);
  Bn n(BN_get_rfc3526_prime_3072(nullptr), BN_free);
  Bn g(BN_new(), BN_free), a(BN_new(), BN_free);
  check(context && n && g && a && BN_set_word(g.get(), 5) == 1 &&
            BN_set_word(a.get(), 0x12345678) == 1,
        "SRP setup failed");
  const auto n_bytes = pad(n.get());
  const auto g_bytes = pad(g.get());
  const auto multiplier_hash = sha512({n_bytes, g_bytes});
  Bn multiplier(BN_bin2bn(multiplier_hash.data(), multiplier_hash.size(), nullptr), BN_free);
  const auto x_inner = sha512({text_bytes("Pair-Setup:"), text_bytes(test_secret)});
  const auto x_hash = sha512({salt, x_inner});
  Bn x(BN_bin2bn(x_hash.data(), x_hash.size(), nullptr), BN_free);
  Bn public_a(BN_new(), BN_free), verifier(BN_new(), BN_free);
  check(BN_mod_exp(public_a.get(), g.get(), a.get(), n.get(), context.get()) == 1 &&
            BN_mod_exp(verifier.get(), g.get(), x.get(), n.get(), context.get()) == 1,
        "SRP client exponent failed");
  ClientProof result;
  result.public_key = pad(public_a.get());
  std::array<std::uint8_t, 384> padded_b{};
  check(server_key.size() <= padded_b.size(), "invalid SRP challenge");
  std::copy(server_key.begin(), server_key.end(),
            padded_b.end() - static_cast<std::ptrdiff_t>(server_key.size()));
  const auto u_hash = sha512({result.public_key, padded_b});
  Bn u(BN_bin2bn(u_hash.data(), u_hash.size(), nullptr), BN_free);
  Bn public_b(BN_bin2bn(padded_b.data(), padded_b.size(), nullptr), BN_free);
  Bn kv(BN_new(), BN_free), base(BN_new(), BN_free);
  Bn ux(BN_new(), BN_free), exponent(BN_new(), BN_free), secret(BN_new(), BN_free);
  check(BN_mod_mul(kv.get(), multiplier.get(), verifier.get(), n.get(), context.get()) == 1 &&
            BN_mod_sub(base.get(), public_b.get(), kv.get(), n.get(), context.get()) == 1 &&
            BN_mul(ux.get(), u.get(), x.get(), context.get()) == 1 &&
            BN_add(exponent.get(), a.get(), ux.get()) == 1 &&
            BN_mod_exp(secret.get(), base.get(), exponent.get(), n.get(), context.get()) == 1,
        "SRP client key failed");
  const auto secret_bytes = pad(secret.get());
  result.session_key = sha512({secret_bytes});
  const auto hn = sha512({n_bytes});
  const std::array<std::uint8_t, 1> short_g{5};
  const auto hg = sha512({short_g});
  const auto hi = sha512({text_bytes("Pair-Setup")});
  std::array<std::uint8_t, 64> group_xor{};
  for (std::size_t i = 0; i < group_xor.size(); ++i) {
    group_xor[i] = hn[i] ^ hg[i];
  }
  result.proof = sha512({group_xor, hi, salt, result.public_key, padded_b, result.session_key});
  result.server_proof = sha512({result.public_key, result.proof, result.session_key});
  return result;
}

struct FakeBus final : headunit::MfiRegisterBus {
  std::map<std::uint8_t, std::vector<std::uint8_t>> registers;
  std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> writes;
  FakeBus() {
    registers[0x02] = {3};
    registers[0x05] = {0};
    registers[0x10] = {0x10};
    registers[0x11] = {0, 64};
    registers[0x12] = std::vector<std::uint8_t>(64, 0x5a);
    registers[0x30] = {0x01, 0x2c};
    registers[0x31] = std::vector<std::uint8_t>(300, 0x5b);
  }
  void read(std::uint8_t reg, std::span<std::uint8_t> out) override {
    const auto& bytes = registers.at(reg);
    check(bytes.size() >= out.size(), "fake chip register too short");
    std::copy_n(bytes.begin(), out.size(), out.begin());
  }
  void write(std::uint8_t reg, std::span<const std::uint8_t> bytes) override {
    writes.emplace_back(reg, std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
  }
};
}  // namespace

int main() try {
  FakeBus bus;
  headunit::MfiCoprocessor chip(bus);
  headunit::HapPairSetup receiver(test_secret, {}, {}, &chip);
  const auto m1 = headunit::encode_hap_tlv(std::vector<headunit::HapTlv>{{6, {1}}, {0, {1}}});
  const auto m2 = headunit::decode_hap_tlv(receiver.respond(m1));
  check(field(m2, 6).value == std::vector<std::uint8_t>{2}, "MFi pair-setup M2 was not sent");
  const auto client = make_client_proof(m2);
  const auto m3 = headunit::encode_hap_tlv(std::vector<headunit::HapTlv>{
      {6, {3}},
      {3, {client.public_key.begin(), client.public_key.end()}},
      {4, {client.proof.begin(), client.proof.end()}},
  });
  const auto m4 = headunit::decode_hap_tlv(receiver.respond(m3));
  check(receiver.proof_verified() &&
            field(m4, 4).value ==
                std::vector<std::uint8_t>(client.server_proof.begin(), client.server_proof.end()),
        "MFi pair-setup SRP proof did not match");
  auto encryption_key = headunit::hap_hkdf_sha512(client.session_key, "Pair-Setup-Encrypt-Salt",
                                                  "Pair-Setup-Encrypt-Info");
  const auto decrypted = headunit::hap_decrypt(encryption_key, "PS-Msg04", field(m4, 5).value);
  check(decrypted.has_value(), "MFi identity proof did not authenticate");
  const auto identity = headunit::decode_hap_tlv(*decrypted);
  check(field(identity, 0x0a).value == std::vector<std::uint8_t>(64, 0x5a) &&
            field(identity, 0x09).value == std::vector<std::uint8_t>(300, 0x5b),
        "MFi signature or certificate missing from M4");
  const auto challenge =
      headunit::hap_hkdf_sha512(client.session_key, "MFi-Pair-Setup-Salt", "MFi-Pair-Setup-Info");
  check(bus.writes.size() == 3 && bus.writes[1].first == 0x21 &&
            bus.writes[1].second == std::vector<std::uint8_t>(challenge.begin(), challenge.end()),
        "MFi chip did not receive SRP-derived challenge");

  headunit::HapPairSetup without_chip(test_secret);
  bool rejected = false;
  try {
    (void)without_chip.respond(m1);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "authenticated pair-setup accepted without MFi chip");
  mfi_test::Identity synthetic;
  headunit::SoftwareMfiAuthenticator software((synthetic.directory / "identity.pk8").string(),
                                              (synthetic.directory / "certificate.p7b").string());
  headunit::HapPairSetup software_receiver(test_secret, {}, {}, &software);
  const auto software_m2 = headunit::decode_hap_tlv(software_receiver.respond(m1));
  const auto software_client = make_client_proof(software_m2);
  const auto software_m3 = headunit::encode_hap_tlv(std::vector<headunit::HapTlv>{
      {6, {3}},
      {3, {software_client.public_key.begin(), software_client.public_key.end()}},
      {4, {software_client.proof.begin(), software_client.proof.end()}},
  });
  const auto software_m4 = headunit::decode_hap_tlv(software_receiver.respond(software_m3));
  const auto software_encryption = headunit::hap_hkdf_sha512(
      software_client.session_key, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info");
  const auto software_decrypted =
      headunit::hap_decrypt(software_encryption, "PS-Msg04", field(software_m4, 5).value);
  check(software_decrypted.has_value() && software_receiver.proof_verified(),
        "software pairing proof failed");
  const auto software_identity = headunit::decode_hap_tlv(*software_decrypted);
  const auto software_challenge = headunit::hap_hkdf_sha512(
      software_client.session_key, "MFi-Pair-Setup-Salt", "MFi-Pair-Setup-Info");
  check(field(software_identity, 0x09).value == synthetic.container &&
            synthetic.verify(software_challenge, field(software_identity, 0x0a).value),
        "software pair-setup signature did not cover the SRP-derived digest");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
