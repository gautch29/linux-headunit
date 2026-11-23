// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/srp_server.hpp"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace headunit {
namespace {
struct BnDeleter {
  void operator()(BIGNUM* value) const { BN_clear_free(value); }
};
using Bn = std::unique_ptr<BIGNUM, BnDeleter>;
using Context = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>;

Bn number() {
  Bn value(BN_new());
  if (!value) throw std::runtime_error("SRP allocation failed");
  return value;
}

Bn from_bytes(std::span<const std::uint8_t> bytes) {
  Bn value(BN_bin2bn(bytes.data(), static_cast<int>(bytes.size()), nullptr));
  if (!value) throw std::runtime_error("SRP number conversion failed");
  return value;
}

std::array<std::uint8_t, SrpServer::public_key_size> padded(const BIGNUM* n) {
  std::array<std::uint8_t, SrpServer::public_key_size> bytes{};
  if (BN_bn2binpad(n, bytes.data(), static_cast<int>(bytes.size())) !=
      static_cast<int>(bytes.size())) {
    throw std::runtime_error("SRP group value exceeds 3072 bits");
  }
  return bytes;
}

std::span<const std::uint8_t> as_bytes(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::array<std::uint8_t, SrpServer::proof_size> sha512(
    std::initializer_list<std::span<const std::uint8_t>> parts) {
  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha512(), nullptr) != 1) {
    throw std::runtime_error("SRP SHA-512 initialization failed");
  }
  for (const auto part : parts) {
    if (!part.empty() && EVP_DigestUpdate(context.get(), part.data(), part.size()) != 1) {
      throw std::runtime_error("SRP SHA-512 update failed");
    }
  }
  std::array<std::uint8_t, SrpServer::proof_size> digest{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1 || length != digest.size()) {
    throw std::runtime_error("SRP SHA-512 finalization failed");
  }
  return digest;
}

void require(bool success) {
  if (!success) throw std::runtime_error("SRP calculation failed");
}
}  // namespace

struct SrpServer::Impl {
  Challenge challenge;
  Bn private_b;
  Bn verifier;
  std::optional<std::array<std::uint8_t, proof_size>> key;
  bool used = false;

  explicit Impl(std::string_view password) : private_b(number()), verifier(number()) {
    if (password.empty() || password.size() > 128) {
      throw std::invalid_argument("invalid SRP password length");
    }
    Context context(BN_CTX_new(), BN_CTX_free);
    Bn prime(BN_get_rfc3526_prime_3072(nullptr));
    Bn generator(number());
    if (!context || !prime || BN_set_word(generator.get(), 5) != 1 ||
        BN_num_bits(prime.get()) != 3072) {
      throw std::runtime_error("SRP group unavailable");
    }
    require(RAND_bytes(challenge.salt.data(), challenge.salt.size()) == 1);
    std::array<std::uint8_t, 32> random_b{};
    require(RAND_priv_bytes(random_b.data(), random_b.size()) == 1);
    require(BN_bin2bn(random_b.data(), static_cast<int>(random_b.size()), private_b.get()) !=
            nullptr);
    OPENSSL_cleanse(random_b.data(), random_b.size());
    BN_set_flags(private_b.get(), BN_FLG_CONSTTIME);

    auto inner = sha512({as_bytes("Pair-Setup:"), as_bytes(password)});
    auto x_digest = sha512({challenge.salt, inner});
    Bn x(from_bytes(x_digest));
    OPENSSL_cleanse(inner.data(), inner.size());
    OPENSSL_cleanse(x_digest.data(), x_digest.size());
    BN_set_flags(x.get(), BN_FLG_CONSTTIME);
    require(BN_mod_exp_mont_consttime(verifier.get(), generator.get(), x.get(), prime.get(),
                                      context.get(), nullptr) == 1);

    const auto prime_bytes = padded(prime.get());
    const auto generator_bytes = padded(generator.get());
    const auto k_digest = sha512({prime_bytes, generator_bytes});
    Bn k(from_bytes(k_digest));
    Bn gb(number()), kv(number()), public_b(number());
    require(BN_mod_exp_mont_consttime(gb.get(), generator.get(), private_b.get(), prime.get(),
                                      context.get(), nullptr) == 1);
    require(BN_mod_mul(kv.get(), k.get(), verifier.get(), prime.get(), context.get()) == 1);
    require(BN_mod_add(public_b.get(), kv.get(), gb.get(), prime.get(), context.get()) == 1);
    challenge.public_key = padded(public_b.get());
  }

  ~Impl() {
    if (key) OPENSSL_cleanse(key->data(), key->size());
  }

  std::optional<std::array<std::uint8_t, proof_size>> verify(
      std::span<const std::uint8_t> client_public, std::span<const std::uint8_t> client_proof) {
    if (used) return std::nullopt;
    used = true;
    if (client_public.empty() || client_public.size() > public_key_size ||
        client_proof.size() != proof_size)
      return std::nullopt;

    Context context(BN_CTX_new(), BN_CTX_free);
    Bn prime(BN_get_rfc3526_prime_3072(nullptr));
    if (!context || !prime) throw std::runtime_error("SRP group unavailable");
    Bn a(from_bytes(client_public));
    if (BN_is_zero(a.get()) || BN_is_negative(a.get()) || BN_cmp(a.get(), prime.get()) >= 0)
      return std::nullopt;
    const auto a_bytes = padded(a.get());
    const auto u_digest = sha512({a_bytes, challenge.public_key});
    Bn u(from_bytes(u_digest));
    if (BN_is_zero(u.get())) return std::nullopt;

    Bn vu(number()), base(number()), premaster(number());
    require(BN_mod_exp(vu.get(), verifier.get(), u.get(), prime.get(), context.get()) == 1);
    require(BN_mod_mul(base.get(), a.get(), vu.get(), prime.get(), context.get()) == 1);
    require(BN_mod_exp_mont_consttime(premaster.get(), base.get(), private_b.get(), prime.get(),
                                      context.get(), nullptr) == 1);
    auto premaster_bytes = padded(premaster.get());
    auto calculated_key = sha512({premaster_bytes});
    OPENSSL_cleanse(premaster_bytes.data(), premaster_bytes.size());

    const auto prime_bytes = padded(prime.get());
    const std::array<std::uint8_t, 1> generator_bytes{5};
    const auto hn = sha512({prime_bytes});
    const auto hg = sha512({generator_bytes});
    const auto hi = sha512({as_bytes("Pair-Setup")});
    std::array<std::uint8_t, proof_size> group_xor{};
    for (std::size_t i = 0; i < group_xor.size(); ++i) {
      group_xor[i] = hn[i] ^ hg[i];
    }
    const auto expected =
        sha512({group_xor, hi, challenge.salt, a_bytes, challenge.public_key, calculated_key});
    if (CRYPTO_memcmp(expected.data(), client_proof.data(), proof_size) != 0) {
      OPENSSL_cleanse(calculated_key.data(), calculated_key.size());
      return std::nullopt;
    }
    const auto server_proof = sha512({a_bytes, expected, calculated_key});
    key = calculated_key;
    OPENSSL_cleanse(calculated_key.data(), calculated_key.size());
    return server_proof;
  }
};

SrpServer::SrpServer(std::string_view password) : impl_(std::make_unique<Impl>(password)) {}
SrpServer::~SrpServer() = default;
SrpServer::SrpServer(SrpServer&&) noexcept = default;
SrpServer& SrpServer::operator=(SrpServer&&) noexcept = default;
const SrpServer::Challenge& SrpServer::challenge() const {
  return impl_->challenge;
}
std::optional<std::array<std::uint8_t, SrpServer::proof_size>> SrpServer::verify(
    std::span<const std::uint8_t> client_public_key, std::span<const std::uint8_t> client_proof) {
  return impl_->verify(client_public_key, client_proof);
}
std::optional<std::array<std::uint8_t, SrpServer::proof_size>> SrpServer::session_key() const {
  return impl_->key;
}

}  // namespace headunit
