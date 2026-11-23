// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/hap_pair_setup.hpp"

#include "headunit/hap_tlv.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace headunit {
namespace {
struct CleanseOnExit {
  void* data;
  std::size_t size;
  ~CleanseOnExit() { OPENSSL_cleanse(data, size); }
};

const HapTlv* only_field(const std::vector<HapTlv>& fields, std::uint8_t type) {
  const HapTlv* result = nullptr;
  for (const auto& field : fields) {
    if (field.type != type) continue;
    if (result) throw std::invalid_argument("duplicate pairing TLV field");
    result = &field;
  }
  return result;
}

std::uint8_t byte_field(const std::vector<HapTlv>& fields, std::uint8_t type) {
  const auto* field = only_field(fields, type);
  if (!field || field->value.size() != 1) {
    throw std::invalid_argument("missing or invalid pairing TLV field");
  }
  return field->value[0];
}
}  // namespace

HapPairSetup::HapPairSetup(std::string_view secret, std::string_view accessory_identifier,
                           const std::string& private_key_path, MfiAuthenticator* mfi_auth)
    : accessory_identifier_(accessory_identifier), mfi_auth_(mfi_auth) {
  if (secret.empty() || secret.size() > 128) {
    throw std::invalid_argument("invalid pairing setup secret length");
  }
  if (!private_key_path.empty()) {
    if (accessory_identifier.empty() || accessory_identifier.size() > 64) {
      throw std::invalid_argument("invalid accessory pairing identifier");
    }
    identity_ = std::make_unique<HapEd25519Identity>(private_key_path);
  }
  // Assign last so a throwing key load cannot leave an uncleansed copy.
  setup_secret_.assign(secret);
}
HapPairSetup::~HapPairSetup() {
  if (!setup_secret_.empty()) {
    OPENSSL_cleanse(setup_secret_.data(), setup_secret_.size());
  }
}
std::vector<std::uint8_t> HapPairSetup::respond(std::span<const std::uint8_t> request) {
  const auto fields = decode_hap_tlv(request);
  const auto state = byte_field(fields, 6);
  if (state == 1 && !srp_) {
    const auto method = byte_field(fields, 0);
    if (method != 0 && method != 1) {
      throw std::invalid_argument("unsupported pairing method");
    }
    if (method == 1 && !mfi_auth_) {
      throw std::invalid_argument("authenticated pairing requires MFi provider");
    }
    if (const auto* flags = only_field(fields, 0x13)) {
      if (flags->value.empty() || flags->value.size() > 4) {
        throw std::invalid_argument("invalid pairing flags");
      }
      std::uint32_t flags_value = 0;
      for (std::size_t index = 0; index < flags->value.size(); ++index) {
        flags_value |= std::uint32_t(flags->value[index]) << (8 * index);
      }
      if (flags_value & (std::uint32_t{1} << 24)) {
        throw std::invalid_argument("split pairing is unsupported");
      }
      transient_ = method == 0 && (flags_value & 0x10) != 0;
    }
    method_ = method;
    srp_ = std::make_unique<SrpServer>(setup_secret_);
    OPENSSL_cleanse(setup_secret_.data(), setup_secret_.size());
    setup_secret_.clear();
    const auto& challenge = srp_->challenge();
    const auto first_nonzero =
        std::find_if(challenge.public_key.begin(), challenge.public_key.end(),
                     [](std::uint8_t value) { return value != 0; });
    return encode_hap_tlv(std::vector<HapTlv>{
        {6, {2}},
        {3, {first_nonzero, challenge.public_key.end()}},
        {2, {challenge.salt.begin(), challenge.salt.end()}},
    });
  }
  if (state == 3 && srp_ && !verified_) {
    const auto* public_key = only_field(fields, 3);
    const auto* proof = only_field(fields, 4);
    if (!public_key || !proof) {
      throw std::invalid_argument("missing controller SRP proof");
    }
    const auto response = srp_->verify(public_key->value, proof->value);
    if (!response) {
      throw std::invalid_argument("controller SRP proof failed");
    }
    std::vector<HapTlv> reply{
        {6, {4}},
        {4, {response->begin(), response->end()}},
    };
    if (method_ == 1) {
      auto session_key = srp_->session_key();
      if (!session_key || !mfi_auth_) {
        throw std::runtime_error("authenticated pairing lost SRP session");
      }
      const CleanseOnExit clean_session{session_key->data(), session_key->size()};
      auto challenge = hap_hkdf_sha512(*session_key, "MFi-Pair-Setup-Salt", "MFi-Pair-Setup-Info");
      const CleanseOnExit clean_challenge{challenge.data(), challenge.size()};
      const auto signature = mfi_auth_->sign_digest(challenge);
      const auto certificate = mfi_auth_->read_certificate();
      const auto identity_proof = encode_hap_tlv(std::vector<HapTlv>{
          {0x0a, {signature.begin(), signature.end()}},
          {0x09, certificate},
      });
      auto encryption_key =
          hap_hkdf_sha512(*session_key, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info");
      const CleanseOnExit clean_encryption{encryption_key.data(), encryption_key.size()};
      reply.push_back({5, hap_encrypt(encryption_key, "PS-Msg04", identity_proof)});
    }
    verified_ = true;
    return encode_hap_tlv(reply);
  }
  if (state == 5 && srp_ && verified_ && identity_ && !transient_ && !paired_controller_) {
    const auto* encrypted_field = only_field(fields, 5);
    auto session_key = srp_->session_key();
    if (!encrypted_field || !session_key) {
      throw std::invalid_argument("missing pair-setup encrypted identity");
    }
    const CleanseOnExit clean_session{session_key->data(), session_key->size()};
    auto encryption_key =
        hap_hkdf_sha512(*session_key, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info");
    const CleanseOnExit clean_encryption{encryption_key.data(), encryption_key.size()};
    const auto plaintext = hap_decrypt(encryption_key, "PS-Msg05", encrypted_field->value);
    if (!plaintext) {
      throw std::invalid_argument("pair-setup identity authentication failed");
    }
    const auto inner = decode_hap_tlv(*plaintext);
    const auto* controller_id = only_field(inner, 1);
    const auto* controller_key = only_field(inner, 3);
    const auto* signature = only_field(inner, 0x0a);
    if (!controller_id || controller_id->value.empty() || controller_id->value.size() > 64 ||
        !controller_key || controller_key->value.size() != 32 || !signature ||
        signature->value.size() != 64 ||
        std::any_of(controller_id->value.begin(), controller_id->value.end(),
                    [](std::uint8_t value) { return value < 0x21 || value > 0x7e; })) {
      throw std::invalid_argument("invalid controller pairing identity");
    }
    HapKey32 controller_public{};
    std::copy(controller_key->value.begin(), controller_key->value.end(),
              controller_public.begin());
    auto controller_x = hap_hkdf_sha512(*session_key, "Pair-Setup-Controller-Sign-Salt",
                                        "Pair-Setup-Controller-Sign-Info");
    const CleanseOnExit clean_controller{controller_x.data(), controller_x.size()};
    std::vector<std::uint8_t> signed_message(controller_x.begin(), controller_x.end());
    signed_message.insert(signed_message.end(), controller_id->value.begin(),
                          controller_id->value.end());
    signed_message.insert(signed_message.end(), controller_public.begin(), controller_public.end());
    if (!hap_verify_ed25519(controller_public, signed_message, signature->value)) {
      throw std::invalid_argument("controller pairing signature failed");
    }

    auto accessory_x = hap_hkdf_sha512(*session_key, "Pair-Setup-Accessory-Sign-Salt",
                                       "Pair-Setup-Accessory-Sign-Info");
    const CleanseOnExit clean_accessory{accessory_x.data(), accessory_x.size()};
    const auto accessory_public = identity_->public_key();
    signed_message.assign(accessory_x.begin(), accessory_x.end());
    signed_message.insert(signed_message.end(), accessory_identifier_.begin(),
                          accessory_identifier_.end());
    signed_message.insert(signed_message.end(), accessory_public.begin(), accessory_public.end());
    const auto accessory_signature = identity_->sign(signed_message);
    const auto inner_response = encode_hap_tlv(std::vector<HapTlv>{
        {1, {accessory_identifier_.begin(), accessory_identifier_.end()}},
        {3, {accessory_public.begin(), accessory_public.end()}},
        {0x0a, {accessory_signature.begin(), accessory_signature.end()}},
    });
    const auto encrypted_response = hap_encrypt(encryption_key, "PS-Msg06", inner_response);
    paired_controller_ = HapPairRecord{{controller_id->value.begin(), controller_id->value.end()},
                                       controller_public};
    return encode_hap_tlv(std::vector<HapTlv>{
        {6, {6}},
        {5, encrypted_response},
    });
  }
  throw std::invalid_argument("unexpected or unsupported pairing state");
}

bool HapPairSetup::proof_verified() const {
  return verified_;
}
std::optional<std::array<std::uint8_t, SrpServer::proof_size>>
HapPairSetup::transient_control_secret() const {
  return verified_ && transient_ ? srp_->session_key() : std::nullopt;
}
const std::optional<HapPairRecord>& HapPairSetup::paired_controller() const {
  return paired_controller_;
}

}  // namespace headunit
