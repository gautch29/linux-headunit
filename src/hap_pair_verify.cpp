// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_pair_verify.hpp"

#include "headunit/hap_tlv.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace headunit {
namespace {
const HapTlv* only_field(const std::vector<HapTlv>& fields, std::uint8_t type) {
  const HapTlv* result = nullptr;
  for (const auto& field : fields) {
    if (field.type != type) continue;
    if (result) throw std::invalid_argument("duplicate pair-verify TLV field");
    result = &field;
  }
  return result;
}

std::uint8_t state_of(const std::vector<HapTlv>& fields) {
  const auto* field = only_field(fields, 6);
  if (!field || field->value.size() != 1) {
    throw std::invalid_argument("missing pair-verify state");
  }
  return field->value[0];
}
}  // namespace

HapPairVerify::HapPairVerify(std::string identifier, const std::string& private_key_path,
                             const HapPairStore& store)
    : accessory_identifier_(std::move(identifier)),
      identity_(private_key_path),
      pair_store_(store) {
  if (accessory_identifier_.empty() || accessory_identifier_.size() > 64) {
    throw std::invalid_argument("invalid accessory pairing identifier");
  }
}

HapPairVerify::~HapPairVerify() {
  if (shared_) OPENSSL_cleanse(shared_->data(), shared_->size());
  if (encryption_key_) {
    OPENSSL_cleanse(encryption_key_->data(), encryption_key_->size());
  }
}

std::vector<std::uint8_t> HapPairVerify::respond(std::span<const std::uint8_t> request) {
  const auto fields = decode_hap_tlv(request);
  const auto state = state_of(fields);
  if (state == 1 && stage_ == 0) {
    if (only_field(fields, 0)) {
      throw std::invalid_argument("pair-resume method is unsupported");
    }
    const auto* peer = only_field(fields, 3);
    if (!peer || peer->value.size() != 32) {
      throw std::invalid_argument("invalid controller X25519 key");
    }
    std::copy(peer->value.begin(), peer->value.end(), controller_ephemeral_.begin());
    ephemeral_ = std::make_unique<HapX25519Ephemeral>();
    shared_ = ephemeral_->shared_secret(controller_ephemeral_);
    if (!shared_) throw std::invalid_argument("X25519 key exchange failed");
    encryption_key_ =
        hap_hkdf_sha512(*shared_, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
    const auto our_public = ephemeral_->public_key();
    std::vector<std::uint8_t> signed_message(our_public.begin(), our_public.end());
    signed_message.insert(signed_message.end(), accessory_identifier_.begin(),
                          accessory_identifier_.end());
    signed_message.insert(signed_message.end(), controller_ephemeral_.begin(),
                          controller_ephemeral_.end());
    const auto signature = identity_.sign(signed_message);
    const auto inner = encode_hap_tlv(std::vector<HapTlv>{
        {1, {accessory_identifier_.begin(), accessory_identifier_.end()}},
        {0x0a, {signature.begin(), signature.end()}},
    });
    const auto encrypted = hap_encrypt(*encryption_key_, "PV-Msg02", inner);
    stage_ = 1;
    return encode_hap_tlv(std::vector<HapTlv>{
        {6, {2}},
        {3, {our_public.begin(), our_public.end()}},
        {5, encrypted},
    });
  }
  if (state == 3 && stage_ == 1) {
    stage_ = 3;
    const auto* encrypted = only_field(fields, 5);
    if (!encrypted || !encryption_key_) {
      throw std::invalid_argument("missing pair-verify proof");
    }
    const auto plaintext = hap_decrypt(*encryption_key_, "PV-Msg03", encrypted->value);
    if (!plaintext) {
      throw std::invalid_argument("pair-verify message authentication failed");
    }
    const auto inner = decode_hap_tlv(*plaintext);
    const auto* id = only_field(inner, 1);
    const auto* signature = only_field(inner, 0x0a);
    if (!id || id->value.empty() || id->value.size() > 64 || !signature ||
        signature->value.size() != 64) {
      throw std::invalid_argument("invalid controller pair-verify identity");
    }
    const std::string identifier(id->value.begin(), id->value.end());
    const auto record = pair_store_.load(identifier);
    if (!record) {
      throw std::invalid_argument("unpaired controller identity");
    }
    std::vector<std::uint8_t> signed_message(controller_ephemeral_.begin(),
                                             controller_ephemeral_.end());
    signed_message.insert(signed_message.end(), identifier.begin(), identifier.end());
    const auto our_public = ephemeral_->public_key();
    signed_message.insert(signed_message.end(), our_public.begin(), our_public.end());
    if (!hap_verify_ed25519(record->public_key, signed_message, signature->value)) {
      throw std::invalid_argument("controller pair-verify signature failed");
    }
    controller_ = *record;
    stage_ = 2;
    return encode_hap_tlv(std::vector<HapTlv>{{6, {4}}});
  }
  throw std::invalid_argument("unexpected pair-verify state");
}

bool HapPairVerify::verified() const {
  return stage_ == 2;
}
std::optional<HapPairRecord> HapPairVerify::verified_controller() const {
  return verified() ? controller_ : std::nullopt;
}
std::optional<HapKey32> HapPairVerify::shared_secret() const {
  return verified() ? shared_ : std::nullopt;
}

}  // namespace headunit
