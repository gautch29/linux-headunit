// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/binary_plist.hpp"
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

using V = headunit::PlistValue;
using B = V::Bytes;
namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("binary plist test failed");
}
void rejects(const std::function<void()>& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
}
void integer(B& out, std::uint64_t value, unsigned width) {
  for (unsigned i = width; i > 0; --i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * (i - 1))));
}
B graph(const std::vector<B>& objects) {
  B out{'b', 'p', 'l', 'i', 's', 't', '0', '0'};
  std::vector<std::size_t> offsets;
  for (const auto& object : objects) {
    offsets.push_back(out.size());
    out.insert(out.end(), object.begin(), object.end());
  }
  const auto table = out.size();
  for (auto offset : offsets)
    integer(out, offset, 2);
  out.insert(out.end(), 6, 0);
  out.push_back(2);
  out.push_back(1);
  integer(out, objects.size(), 8);
  integer(out, 0, 8);
  integer(out, table, 8);
  return out;
}
}  // namespace
int main() {
  const V object{V::Dictionary{{"text", V{std::string("été 🗺")}},
                               {"binary", V{B{0, 255, 42}}},
                               {"array", V{V::Array{V{true}, V{std::int64_t{-3}}, V{2.5}}}},
                               {"emptyArray", V{V::Array{}}},
                               {"emptyDictionary", V{V::Dictionary{}}},
                               {"emptyData", V{B{}}},
                               {"emptyString", V{std::string{}}},
                               {"large", V{std::numeric_limits<std::int64_t>::max()}}}};
  // Independently generated with Python plistlib, not this encoder.
  const std::string python_hex =
      "62706c6973743030d80102030405060708090d0e0f1011121355617272617956"
      "62696e6172795a656d707479417272617959656d707479446174615f100f656d"
      "70747944696374696f6e6172795b656d707479537472696e67556c6172676554"
      "74657874a30a0b0c0913fffffffffffffffd2340040000000000004300ff2aa0"
      "40d050137fffffffffffffff6600e9007400e90020d83dddfa08191f26313b4d"
      "595f646869727b7f808182838c00000000000001010000000000000014000000"
      "00000000000000000000000099";
  B python_fixture;
  const auto nibble = [](char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
  };
  for (std::size_t i = 0; i < python_hex.size(); i += 2)
    python_fixture.push_back(
        static_cast<std::uint8_t>((nibble(python_hex[i]) << 4) | nibble(python_hex[i + 1])));
  check(headunit::decode_binary_plist(python_fixture) == object);
  // Exercise changed markers, lengths, references and trailers under sanitizers.
  for (std::size_t i = 0; i < python_fixture.size(); ++i) {
    for (std::uint8_t mask : {std::uint8_t{0x80}, std::uint8_t{0xff}}) {
      auto changed = python_fixture;
      changed[i] ^= mask;
      try {
        (void)headunit::decode_binary_plist(changed);
      } catch (const std::invalid_argument&) {
      }
    }
  }
  const auto encoded = headunit::encode_binary_plist(object);
  check(headunit::decode_binary_plist(encoded) == object);
  check(headunit::decode_binary_plist(graph({B{0x10, 255}})) == V{std::int64_t{255}});
  for (std::size_t i = 0; i < encoded.size(); ++i)
    rejects([&] { (void)headunit::decode_binary_plist(std::span(encoded).first(i)); });
  rejects([&] { (void)headunit::decode_binary_plist(graph({B{0xa1, 0}})); });  // cycle
  rejects([&] { (void)headunit::decode_binary_plist(graph({B{0xa1, 1}})); });  // bad reference
  rejects([&] { (void)headunit::decode_binary_plist(graph({B{0x00}})); });     // null
  rejects([&] { (void)headunit::decode_binary_plist(graph({B{0x80, 0}})); });  // UID
  rejects([&] { (void)headunit::decode_binary_plist(graph({B{0x51, 0}})); });  // embedded NUL
  rejects([&] {
    (void)headunit::decode_binary_plist(graph({B{0x61, 0xd8, 0x00}}));
  });  // lone surrogate
  rejects([&] {
    (void)headunit::decode_binary_plist(
        graph({B{0xdf, 0x13, 255, 255, 255, 255, 255, 255, 255, 255}}));
  });
  // Equivalent text keys in ASCII and UTF-16 must not silently overwrite.
  rejects([&] {
    (void)headunit::decode_binary_plist(
        graph({B{0xd2, 1, 2, 3, 3}, B{0x51, 'x'}, B{0x61, 0, 'x'}, B{0x09}}));
  });
  std::vector<B> deep;
  for (unsigned i = 0; i < 34; ++i)
    deep.push_back(B{0xa1, static_cast<std::uint8_t>(i + 1)});
  deep.push_back(B{0x09});
  rejects([&] { (void)headunit::decode_binary_plist(graph(deep)); });
  std::vector<B> amplified;
  for (unsigned i = 0; i < 14; ++i)
    amplified.push_back(
        B{0xa2, static_cast<std::uint8_t>(i + 1), static_cast<std::uint8_t>(i + 1)});
  amplified.push_back(B{0x09});
  rejects([&] { (void)headunit::decode_binary_plist(graph(amplified)); });
  rejects([&] { (void)headunit::encode_binary_plist(V{std::numeric_limits<double>::infinity()}); });
  rejects([&] { (void)headunit::encode_binary_plist(V{std::string("a\0b", 3)}); });
  rejects([&] { (void)headunit::encode_binary_plist(V{std::string("\xc0\x80", 2)}); });
  rejects([&] { (void)headunit::encode_binary_plist(V{B(headunit::plist_wire_limit, 0)}); });
  return 0;
}
