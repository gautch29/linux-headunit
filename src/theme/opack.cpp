// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/opack.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <type_traits>

// Format research: Xcode CarPlaySDK's exported vehicle-data codec, independently
// generated fixtures, and pyatv's public OPACK format implementation (see README).
namespace headunit::theme::opack {
namespace {
[[noreturn]] void bad() {
  throw std::invalid_argument("invalid or oversized OPACK value");
}
bool utf8(std::span<const std::uint8_t> s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto first = s[i++];
    if (first < 0x80) continue;
    unsigned n = first >= 0xc2 && first <= 0xdf   ? 1
                 : first >= 0xe0 && first <= 0xef ? 2
                 : first >= 0xf0 && first <= 0xf4 ? 3
                                                  : 0;
    if (!n || n > s.size() - i) return false;
    std::uint32_t cp = first & (0x7f >> (n + 1));
    for (unsigned j = 0; j < n; ++j) {
      const auto c = s[i++];
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3f);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  return true;
}
std::size_t scalar_bytes(const Value& v) {
  if (auto s = std::get_if<std::string>(&v.data)) return s->size();
  if (auto b = std::get_if<Bytes>(&v.data)) return b->size();
  return 16;
}
struct Decoder {
  std::span<const std::uint8_t> wire;
  std::size_t position = 0, nodes = 0, expanded = 0;
  std::vector<Value> references;
  std::uint8_t byte() {
    if (position == wire.size()) bad();
    return wire[position++];
  }
  std::uint64_t integer(unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
      value |= std::uint64_t(byte()) << (i * 8);
    return value;
  }
  std::span<const std::uint8_t> take(std::uint64_t size) {
    if (size > wire.size() - position || size > max_expanded_bytes - expanded) bad();
    auto result = wire.subspan(position, size);
    position += size;
    expanded += size;
    return result;
  }
  Value value(std::size_t depth) {
    if (depth > max_depth || ++nodes > max_nodes) bad();
    auto tag = byte();
    if (tag >= 0xa0 && tag <= 0xc4) {
      auto index = tag <= 0xc0 ? tag - 0xa0 : integer(1u << (tag - 0xc1));
      if (index >= references.size()) bad();
      const auto& ref = references[index];
      const auto size = scalar_bytes(ref);
      if (size > max_expanded_bytes - expanded) bad();
      expanded += size;
      return ref;
    }
    Value out;
    bool remember = false;
    if (tag == 1 || tag == 2)
      out.data = tag == 1;
    else if (tag == 4)
      out.data = std::monostate{};
    else if (tag == 7)
      out.data = std::int64_t(-1);
    else if (tag >= 8 && tag <= 0x2f)
      out.data = std::int64_t(tag - 8);
    else if (tag >= 0x30 && tag <= 0x33) {
      const unsigned width = 1u << (tag - 0x30);
      auto bits = integer(width);
      if (width < 8 && (bits & (std::uint64_t(1) << (width * 8 - 1))))
        bits |= (~std::uint64_t(0)) << (width * 8);
      out.data = std::bit_cast<std::int64_t>(bits);
      remember = true;
    } else if (tag == 0x35 || tag == 0x36) {
      const auto bits = integer(tag == 0x35 ? 4 : 8);
      double d = tag == 0x35 ? std::bit_cast<float>(static_cast<std::uint32_t>(bits))
                             : std::bit_cast<double>(bits);
      if (!std::isfinite(d)) bad();
      out.data = d;
      remember = true;
    } else if (tag == 5) {
      UUID id{};
      auto s = take(16);
      std::copy(s.begin(), s.end(), id.bytes.begin());
      out.data = id;
      remember = true;
    } else if (tag == 6) {
      out.data = DateBits{integer(8)};
      remember = true;
    } else if (tag >= 0x40 && tag <= 0x64) {
      auto n = tag <= 0x60 ? tag - 0x40 : integer(1u << (tag - 0x61));
      auto s = take(n);
      if (!utf8(s)) bad();
      out.data = std::string(reinterpret_cast<const char*>(s.data()), s.size());
      remember = n != 0;
    } else if (tag >= 0x70 && tag <= 0x94) {
      auto n = tag <= 0x90 ? tag - 0x70 : integer(1u << (tag - 0x91));
      auto s = take(n);
      out.data = Bytes(s.begin(), s.end());
      remember = n != 0;
    } else if ((tag >= 0xd0 && tag <= 0xdf) || (tag >= 0xe0 && tag <= 0xef)) {
      bool dict = tag >= 0xe0;
      auto count = tag & 15;
      Array array;
      Dictionary dictionary;
      for (unsigned i = 0; count == 15 || i < unsigned(count); ++i) {
        if (count == 15 && position < wire.size() && wire[position] == 3) {
          ++position;
          break;
        }
        auto key = value(depth + 1);
        if (dict) {
          // Vehicle protocol keys are scalar. Reject duplicate keys to avoid
          // different interpretations between implementations.
          if (std::holds_alternative<Array>(key.data) ||
              std::holds_alternative<Dictionary>(key.data))
            bad();
          if (std::any_of(dictionary.begin(), dictionary.end(),
                          [&](auto& p) { return p.first == key; }))
            bad();
          dictionary.emplace_back(std::move(key), value(depth + 1));
        } else
          array.push_back(std::move(key));
      }
      if (dict)
        out.data = std::move(dictionary);
      else
        out.data = std::move(array);
    } else
      bad();
    if (remember) {
      if (references.size() >= max_nodes) bad();
      references.push_back(out);
    }
    return out;
  }
};
struct Encoder {
  Bytes wire;
  std::size_t nodes = 0;
  void byte(std::uint8_t b) {
    if (wire.size() >= max_packet_bytes) bad();
    wire.push_back(b);
  }
  void integer(std::uint64_t n, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
      byte(n >> (i * 8));
  }
  void blob(std::span<const std::uint8_t> b) {
    if (b.size() > max_packet_bytes - wire.size()) bad();
    wire.insert(wire.end(), b.begin(), b.end());
  }
  void size(std::size_t n, bool str) {
    if (n <= 32) {
      byte((str ? 0x40 : 0x70) + n);
      return;
    }
    unsigned width = n <= 255 ? 1 : n <= 65535 ? 2 : 4;
    unsigned suffix = width == 1 ? 1 : width == 2 ? 2 : 3;
    byte((str ? 0x60 : 0x90) + suffix);
    integer(n, width);
  }
  void value(const Value& v, std::size_t depth) {
    if (depth > max_depth || ++nodes > max_nodes) bad();
    std::visit(
        [&](const auto& item) {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, std::monostate>)
            byte(4);
          else if constexpr (std::is_same_v<T, bool>)
            byte(item ? 1 : 2);
          else if constexpr (std::is_same_v<T, std::int64_t>) {
            if (item == -1)
              byte(7);
            else if (item >= 0 && item <= 39)
              byte(item + 8);
            else {
              unsigned w = item >= -128 && item <= 127                     ? 1
                           : item >= -32768 && item <= 32767               ? 2
                           : item >= -2147483648LL && item <= 2147483647LL ? 4
                                                                           : 8;
              byte(w == 1 ? 0x30 : w == 2 ? 0x31 : w == 4 ? 0x32 : 0x33);
              integer(std::bit_cast<std::uint64_t>(item), w);
            }
          } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(item)) bad();
            byte(0x36);
            integer(std::bit_cast<std::uint64_t>(item), 8);
          } else if constexpr (std::is_same_v<T, std::string>) {
            auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(item.data()), item.size());
            if (!utf8(bytes)) bad();
            size(item.size(), true);
            blob(bytes);
          } else if constexpr (std::is_same_v<T, Bytes>) {
            size(item.size(), false);
            blob(item);
          } else if constexpr (std::is_same_v<T, UUID>) {
            byte(5);
            blob(item.bytes);
          } else if constexpr (std::is_same_v<T, DateBits>) {
            byte(6);
            integer(item.bits, 8);
          } else if constexpr (std::is_same_v<T, Array>) {
            byte(0xd0 + std::min<std::size_t>(item.size(), 15));
            for (auto& child : item)
              value(child, depth + 1);
            if (item.size() >= 15) byte(3);
          } else if constexpr (std::is_same_v<T, Dictionary>) {
            byte(0xe0 + std::min<std::size_t>(item.size(), 15));
            for (std::size_t i = 0; i < item.size(); ++i) {
              const auto& [k, val] = item[i];
              if (std::holds_alternative<Array>(k.data) ||
                  std::holds_alternative<Dictionary>(k.data))
                bad();
              for (std::size_t j = 0; j < i; ++j)
                if (item[j].first == k) bad();
              value(k, depth + 1);
              value(val, depth + 1);
            }
            if (item.size() >= 15) byte(3);
          }
        },
        v.data);
  }
};
}  // namespace
Value decode(std::span<const std::uint8_t> wire) {
  if (wire.empty() || wire.size() > max_packet_bytes) bad();
  Decoder decoder{wire, 0, 0, 0, {}};
  auto result = decoder.value(0);
  if (decoder.position != wire.size()) bad();
  return result;
}
Bytes encode(const Value& v) {
  Encoder e;
  e.value(v, 0);
  return std::move(e.wire);
}
}  // namespace headunit::theme::opack
