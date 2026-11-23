// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/srp_server.hpp"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Bytes = std::span<const std::uint8_t>;
struct BnDeleter {
  void operator()(BIGNUM* n) const { BN_free(n); }
};
using Bn = std::unique_ptr<BIGNUM, BnDeleter>;
using Context = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>;

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
Bytes text_bytes(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
std::array<std::uint8_t, 64> digest(std::initializer_list<Bytes> parts) {
  std::vector<std::uint8_t> input;
  for (auto part : parts)
    input.insert(input.end(), part.begin(), part.end());
  std::array<std::uint8_t, 64> result{};
  unsigned length = 0;
  check(
      EVP_Digest(input.data(), input.size(), result.data(), &length, EVP_sha512(), nullptr) == 1 &&
          length == result.size(),
      "SHA-512 unavailable");
  return result;
}
Bn number() {
  return Bn(BN_new());
}
Bn from(Bytes bytes) {
  return Bn(BN_bin2bn(bytes.data(), bytes.size(), nullptr));
}
std::array<std::uint8_t, 384> pad(const BIGNUM* number) {
  std::array<std::uint8_t, 384> bytes{};
  check(BN_bn2binpad(number, bytes.data(), bytes.size()) == static_cast<int>(bytes.size()),
        "SRP group size wrong");
  return bytes;
}
}  // namespace

int main() try {
  constexpr std::string_view password = "123-45-678";
  headunit::SrpServer server(password);
  const auto challenge = server.challenge();
  check(challenge.salt.size() == 16 && challenge.public_key.size() == 384,
        "SRP challenge dimensions wrong");
  check(!server.session_key(), "SRP key exposed before authentication");

  // Independently calculate the controller half of SRP-6a. The server must
  // accept this proof and return the matching accessory proof.
  Context context(BN_CTX_new(), BN_CTX_free);
  Bn n(BN_get_rfc3526_prime_3072(nullptr)), g(number()), a(number());
  check(context && n && g && a && BN_set_word(g.get(), 5) == 1 &&
            BN_set_word(a.get(), 0x12345678) == 1,
        "SRP setup failed");
  auto n_bytes = pad(n.get());
  auto g_bytes = pad(g.get());
  const auto k_hash = digest({n_bytes, g_bytes});
  Bn multiplier(from(k_hash));
  const auto inner = digest({text_bytes("Pair-Setup:"), text_bytes(password)});
  const auto x_hash = digest({challenge.salt, inner});
  Bn x(from(x_hash)), public_a(number()), verifier(number());
  check(BN_mod_exp(public_a.get(), g.get(), a.get(), n.get(), context.get()) == 1 &&
            BN_mod_exp(verifier.get(), g.get(), x.get(), n.get(), context.get()) == 1,
        "SRP client exponent failed");
  const auto a_bytes = pad(public_a.get());
  const auto u_hash = digest({a_bytes, challenge.public_key});
  Bn u(from(u_hash)), public_b(from(challenge.public_key));
  Bn kv(number()), base(number()), ux(number()), exponent(number()), secret(number());
  check(BN_mod_mul(kv.get(), multiplier.get(), verifier.get(), n.get(), context.get()) == 1 &&
            BN_mod_sub(base.get(), public_b.get(), kv.get(), n.get(), context.get()) == 1 &&
            BN_mul(ux.get(), u.get(), x.get(), context.get()) == 1 &&
            BN_add(exponent.get(), a.get(), ux.get()) == 1 &&
            BN_mod_exp(secret.get(), base.get(), exponent.get(), n.get(), context.get()) == 1,
        "SRP client secret failed");
  auto secret_bytes = pad(secret.get());
  const auto key = digest({secret_bytes});
  OPENSSL_cleanse(secret_bytes.data(), secret_bytes.size());
  const auto hn = digest({n_bytes});
  const std::array<std::uint8_t, 1> g_short{5};
  const auto hg = digest({g_short});
  const auto hi = digest({text_bytes("Pair-Setup")});
  std::array<std::uint8_t, 64> group_xor{};
  for (std::size_t i = 0; i < group_xor.size(); ++i) {
    group_xor[i] = hn[i] ^ hg[i];
  }
  const auto proof = digest({group_xor, hi, challenge.salt, a_bytes, challenge.public_key, key});
  const auto expected_server = digest({a_bytes, proof, key});
  const auto actual_server = server.verify(a_bytes, proof);
  check(actual_server && *actual_server == expected_server,
        "SRP server rejected correct controller proof");
  check(server.session_key() && *server.session_key() == key, "SRP shared key mismatch");
  check(!server.verify(a_bytes, proof), "SRP accepted replayed proof");

  headunit::SrpServer invalid(password);
  auto bad_proof = proof;
  bad_proof[0] ^= 1;
  check(!invalid.verify(a_bytes, bad_proof), "SRP accepted wrong proof");
  check(!invalid.verify(a_bytes, proof), "SRP retried after wrong proof");
  check(!invalid.session_key(), "SRP kept a key after failed proof");
  headunit::SrpServer zero_key(password);
  const std::array<std::uint8_t, 1> zero{0};
  check(!zero_key.verify(zero, proof), "SRP accepted zero public key");
  headunit::SrpServer out_of_range(password);
  check(!out_of_range.verify(n_bytes, proof), "SRP accepted public key equal to group prime");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
