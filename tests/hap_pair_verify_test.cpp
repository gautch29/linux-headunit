// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/hap_pair_verify.hpp>
#include <headunit/hap_tlv.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace headunit;
void check(bool ok) {
  if (!ok) throw std::runtime_error("pair-verify identity check failed");
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid pair-verify accepted");
}
struct Fixture {
  std::filesystem::path path;
  Fixture() {
    char name[] = "/tmp/headunit-greeting-pair-XXXXXX";
    check(mkdtemp(name));
    path = name;
  }
  ~Fixture() { std::filesystem::remove_all(path); }
  std::string key(const char* name) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* raw = nullptr;
    check(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
          EVP_PKEY_keygen(context.get(), &raw) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
    const auto file_path = (path / name).string();
    const auto close_file = [](FILE* file) {
      (void)fclose(file);
    };
    std::unique_ptr<FILE, decltype(close_file)> file(fopen(file_path.c_str(), "w"), close_file);
    check(file &&
          PEM_write_PrivateKey(file.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
    std::filesystem::permissions(
        file_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    return file_path;
  }
};
void exchange(const std::string& accessory_key, HapPairStore& store,
              const HapEd25519Identity& controller, const std::string& id, bool valid) {
  HapPairVerify verifier("fixture-accessory", accessory_key, store);
  check(!verifier.verified() && !verifier.verified_controller());
  HapX25519Ephemeral ephemeral;
  const auto peer = ephemeral.public_key();
  const auto response = decode_hap_tlv(verifier.respond(
      encode_hap_tlv(std::vector<HapTlv>{{6, {1}}, {3, {peer.begin(), peer.end()}}})));
  const auto found = std::find_if(response.begin(), response.end(),
                                  [](const auto& item) { return item.type == 3; });
  check(found != response.end() && found->value.size() == 32);
  HapKey32 accessory_ephemeral{};
  std::copy(found->value.begin(), found->value.end(), accessory_ephemeral.begin());
  const auto shared = ephemeral.shared_secret(accessory_ephemeral);
  check(shared.has_value() && !verifier.verified_controller());
  const auto key = hap_hkdf_sha512(*shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
  std::vector<std::uint8_t> signed_bytes(peer.begin(), peer.end());
  signed_bytes.insert(signed_bytes.end(), id.begin(), id.end());
  signed_bytes.insert(signed_bytes.end(), accessory_ephemeral.begin(), accessory_ephemeral.end());
  auto signature = controller.sign(signed_bytes);
  if (!valid) signature.front() ^= 1;
  const auto proof = encode_hap_tlv(std::vector<HapTlv>{
      {1, {id.begin(), id.end()}}, {0x0a, {signature.begin(), signature.end()}}});
  const auto encrypted = hap_encrypt(key, "PV-Msg03", proof);
  const auto request = encode_hap_tlv(std::vector<HapTlv>{{6, {3}}, {5, encrypted}});
  if (valid) {
    (void)verifier.respond(request);
    const auto verified = verifier.verified_controller();
    check(verifier.verified() && verified && verified->identifier == id &&
          verified->public_key == controller.public_key());
    rejects([&] { verifier.respond(request); });
  } else {
    rejects([&] { verifier.respond(request); });
    check(!verifier.verified() && !verifier.verified_controller());
  }
}
int main() {
  try {
    Fixture fixture;
    const auto accessory_key = fixture.key("accessory.pem");
    HapEd25519Identity controller(fixture.key("controller.pem"));
    const auto store_path = fixture.path / "store";
    std::filesystem::create_directory(store_path);
    std::filesystem::permissions(store_path, std::filesystem::perms::owner_all);
    HapPairStore store(store_path.string());
    store.save({"fixture-controller", controller.public_key()});
    exchange(accessory_key, store, controller, "fixture-controller", true);
    exchange(accessory_key, store, controller, "fixture-controller", false);
    exchange(accessory_key, store, controller, "unpaired-controller", false);
    std::cout << "verified controller binding passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
