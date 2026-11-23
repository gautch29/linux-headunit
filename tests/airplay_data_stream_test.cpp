// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_data_stream.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Bytes = std::vector<std::uint8_t>;
using Stream = headunit::AirplayDataStream;
using Message = headunit::DataMessage;
using Kind = headunit::DataMessageKind;
void check(bool value) {
  if (!value) throw std::runtime_error("DataStream test failed");
}
template <class F>
void rejects(F&& action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
}
Bytes hex(const std::string& value) {
  Bytes bytes;
  for (std::size_t i = 0; i < value.size(); i += 2)
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(value.substr(i, 2), nullptr, 16)));
  return bytes;
}
std::string nonce(std::uint64_t value) {
  std::string bytes(8, 0);
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = static_cast<char>(value >> (8 * i));
  return bytes;
}
Bytes seal(const headunit::HapKey32& key, std::uint64_t counter,
           std::span<const std::uint8_t> plain) {
  Bytes wire{static_cast<std::uint8_t>(plain.size()), static_cast<std::uint8_t>(plain.size() >> 8)};
  const auto cipher = headunit::hap_encrypt(key, nonce(counter), plain, wire);
  wire.insert(wire.end(), cipher.begin(), cipher.end());
  return wire;
}
}  // namespace

int main() try {
  std::array<std::uint8_t, 32> secret{};
  for (unsigned i = 0; i < secret.size(); ++i)
    secret[i] = i;
  // Independent Python cryptography fixtures: synthetic secret bytes 0..31,
  // seed 19, signed status -6705 and distinct 64-bit group/token fields.
  const auto incoming0 =
      hex("23004c2102a53e2cf3c0192cbd20351c4a7e8cb6e0d7167e18b18d9e3a48ad34557ec315cfc73ef5bb3e5f2c"
          "780eca861074e9e0c3");
  const auto incoming1 =
      hex("23003e4778ac8bc164f1eeee0b375f0c506252be68775eb7ccab1c35ffdbee6506f5b874b71fed8a450a8e5b"
          "147df329a0eac5408d");
  const auto outgoing0 =
      hex("23009251174a0cc1f02bcf22419bcb8e14d6ce31b2f1b7609772faa91d82419bff600fbc91164964ed05e917"
          "ed0fea1e846889b6bd");
  const auto outgoing1 =
      hex("23008758186f84cf012902369a0d7add7b5f859c3e243aa634f30d4ba5dc23ae9ee07b7d53fc99997cc1f6ad"
          "a19a157ba0d8bcf882");
  Message expected{
      Kind::synchronous, 0x0102030405060708ULL, 0x636d6e64, 0xfedcba9876543210ULL, -6705,
      {1, 2, 3}};
  for (std::size_t split = 0; split < incoming0.size(); ++split) {
    Stream stream(secret, 19);
    check(stream.feed(std::span(incoming0).first(split)).empty());
    check(stream.feed(std::span(incoming0).subspan(split)) == std::vector<Message>{expected});
    check(stream.feed(incoming1) == std::vector<Message>{expected});
    auto response = expected;
    response.kind = Kind::reply;
    check(stream.encode(response) == outgoing0);
    check(stream.encode(response) == outgoing1);
  }
  Stream replay(secret, 19);
  check(replay.feed(incoming0).size() == 1);
  rejects([&] { (void)replay.feed(incoming0); });
  check(replay.failed());
  rejects([&] { (void)replay.feed(incoming1); });
  rejects([&] { (void)replay.encode(expected); });
  for (std::size_t index : {std::size_t{2}, incoming0.size() - 1}) {
    auto tampered = incoming0;
    tampered[index] ^= 1;
    Stream stream(secret, 19);
    rejects([&] { (void)stream.feed(tampered); });
    check(stream.failed());
    rejects([&] { (void)stream.feed(incoming0); });
  }
  Stream wrong_seed(secret, 20);
  rejects([&] { (void)wrong_seed.feed(incoming0); });
  rejects([&] { Stream invalid{std::span<const std::uint8_t>(secret).first(16), 19}; });

  const auto read_key =
      headunit::hap_hkdf_sha512(secret, "DataStream-Salt19", "DataStream-Output-Encryption-Key");
  const auto raw = hex("0000002373796e630102030405060708636d6e64fedcba9876543210ffffe5cf010203");
  Bytes many;
  for (unsigned i = 0; i < 200; ++i)
    many.insert(many.end(), raw.begin(), raw.end());
  Stream coalesced(secret, 19);
  check(coalesced.feed(seal(read_key, 0, std::span(many).first(7))).empty());
  const auto result = coalesced.feed(seal(read_key, 1, std::span(many).subspan(7)));
  check(result.size() == 200 && std::all_of(result.begin(), result.end(),
                                            [&](const auto& value) { return value == expected; }));
  auto invalid_header = raw;
  invalid_header[4] = '?';
  Stream unknown_kind(secret, 19);
  rejects([&] { (void)unknown_kind.feed(seal(read_key, 0, invalid_header)); });
  check(unknown_kind.failed());
  Stream excessive(secret, 19);
  rejects([&] { (void)excessive.feed(seal(read_key, 0, hex("00100001"))); });
  Stream huge_record(secret, 19);
  rejects([&] { (void)huge_record.feed(hex("0140")); });
  check(huge_record.failed());
  Stream input_limit(secret, 19);
  rejects([&] { (void)input_limit.feed(Bytes(Stream::max_feed + 1)); });
  check(input_limit.failed());
  Stream empty_record(secret, 19);
  check(empty_record.feed(seal(read_key, 0, {})).empty());
  check(empty_record.feed(incoming1).size() == 1);

  // Multi-record sending is independently opened with the receiver's write
  // key. It must retain every payload byte and increment the nonce once per record.
  Stream large(secret, 19);
  Message message;
  message.payload = Bytes(Stream::max_package - 32, 0x9c);
  const auto encrypted = large.encode(message);
  const auto write_key =
      headunit::hap_hkdf_sha512(secret, "DataStream-Salt19", "DataStream-Input-Encryption-Key");
  Bytes recovered;
  std::size_t offset = 0;
  std::uint64_t counter = 0;
  while (offset < encrypted.size()) {
    const auto record = std::span(encrypted).subspan(offset);
    const std::size_t size = record[0] | (static_cast<std::size_t>(record[1]) << 8);
    check(size <= 16384 && size + 18 <= record.size());
    const auto clear = headunit::hap_decrypt(write_key, nonce(counter++),
                                             record.subspan(2, size + 16), record.first(2));
    check(clear.has_value());
    recovered.insert(recovered.end(), clear->begin(), clear->end());
    offset += size + 18;
  }
  check(counter == 64 && recovered.size() == Stream::max_package);
  check(recovered[0] == 0 && recovered[1] == 0x10 && recovered[2] == 0 && recovered[3] == 0);
  check(std::equal(recovered.begin() + 32, recovered.end(), message.payload.begin()));
  Stream large_reader(secret, 19);
  std::vector<Message> delivered;
  for (std::size_t position = 0, sequence = 0; position < recovered.size(); ++sequence) {
    const auto count = std::min<std::size_t>(16384, recovered.size() - position);
    auto items =
        large_reader.feed(seal(read_key, sequence, std::span(recovered).subspan(position, count)));
    delivered.insert(delivered.end(), items.begin(), items.end());
    position += count;
  }
  check(delivered == std::vector<Message>{message});
  message.payload.push_back(1);
  rejects([&] { (void)large.encode(message); });
  check(!large.failed());  // Invalid caller input did not consume a nonce.
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
