// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/opack.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace headunit {
namespace {
using Value = OpackValue;
using Bytes = Value::Bytes;

[[noreturn]] void invalid() {
  throw std::invalid_argument("invalid or unsupported OPACK");
}
[[noreturn]] void exhausted() {
  throw std::invalid_argument("OPACK budget exceeded");
}

bool utf8(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80) continue;
    unsigned count;
    std::uint32_t code, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      count = 1;
      code = c & 31;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      count = 2;
      code = c & 15;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      count = 3;
      code = c & 7;
      minimum = 0x10000;
    } else
      return false;
    if (count > text.size() - i) return false;
    while (count--) {
      c = static_cast<unsigned char>(text[i++]);
      if ((c & 0xc0) != 0x80) return false;
      code = (code << 6) | (c & 63);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}

struct Budget {
  OpackLimits limit;
  std::size_t nodes = 0;
  std::size_t bytes = 0;
  void node(unsigned depth) {
    if (depth > limit.depth || nodes >= limit.nodes) exhausted();
    ++nodes;
  }
  void data(std::size_t size) {
    if (bytes > limit.expanded_bytes || size > limit.expanded_bytes - bytes) exhausted();
    bytes += size;
  }
};

class Reader {
 public:
  Reader(std::span<const std::uint8_t> bytes, OpackLimits limits) : input_(bytes), budget_{limits} {
    if (bytes.size() > limits.wire_bytes) exhausted();
  }
  Value run() {
    auto result = read(0);
    if (position_ != input_.size()) invalid();
    return result;
  }

 private:
  std::span<const std::uint8_t> take(std::size_t length) {
    if (length > input_.size() - position_) invalid();
    const auto bytes = input_.subspan(position_, length);
    position_ += length;
    return bytes;
  }
  std::uint64_t little(unsigned width) {
    const auto bytes = take(width);
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i)
      result |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return result;
  }
  bool terminated() {
    if (position_ == input_.size()) invalid();
    if (input_[position_] != 3) return false;
    ++position_;
    return true;
  }
  void account(const Value& value) {
    if (const auto* s = std::get_if<std::string>(&value.value)) budget_.data(s->size());
    if (const auto* b = std::get_if<Bytes>(&value.value)) budget_.data(b->size());
  }
  Value read(unsigned depth) {
    budget_.node(depth);
    const auto tag = take(1)[0];
    Value result;
    bool remember = false;
    if (tag >= 0xa0 && tag <= 0xc4) {
      const auto index = tag <= 0xc0 ? tag - 0xa0 : little(1u << (tag - 0xc1));
      if (index >= references_.size()) invalid();
      // Only scalar objects are in this table. Count expanded data on every
      // dereference before making a copy, to bound repeated large blobs.
      account(references_[index]);
      return references_[index];
    }
    if (tag == 1 || tag == 2)
      result.value = tag == 1;
    else if (tag == 4)
      result.value = std::monostate{};
    else if (tag >= 7 && tag <= 0x2f)
      result.value = static_cast<std::int64_t>(tag) - 8;
    else if (tag >= 0x30 && tag <= 0x33) {
      const unsigned width = 1u << (tag - 0x30);
      auto bits = little(width);
      if (width < 8 && (bits & (std::uint64_t{1} << (8 * width - 1))))
        bits |= (~std::uint64_t{0}) << (8 * width);
      const auto number = std::bit_cast<std::int64_t>(bits);
      result.value = number;
      remember = number < -1 || number > 39;
    } else if (tag == 0x35 || tag == 0x36) {
      double number =
          tag == 0x35
              ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(little(4))))
              : std::bit_cast<double>(little(8));
      if (!std::isfinite(number)) invalid();
      result.value = number;
      remember = true;
    } else if ((tag >= 0x40 && tag <= 0x64) || tag == 0x6f) {
      std::uint64_t length;
      if (tag == 0x6f) {
        const auto start = input_.begin() + static_cast<std::ptrdiff_t>(position_);
        const auto end = std::find(start, input_.end(), 0);
        if (end == input_.end()) invalid();
        length = static_cast<std::uint64_t>(end - start);
      } else
        length = tag <= 0x60 ? tag - 0x40 : little(1u << (tag - 0x61));
      if (length > input_.size() - position_) invalid();
      budget_.data(static_cast<std::size_t>(length));
      const auto data = take(static_cast<std::size_t>(length));
      const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
      if (!utf8(text)) invalid();
      result.value = std::string(text);
      if (tag == 0x6f) (void)take(1);
      remember = length != 0;
    } else if (tag >= 0x70 && tag <= 0x94) {
      const auto length = tag <= 0x90 ? tag - 0x70 : little(1u << (tag - 0x91));
      if (length > input_.size() - position_) invalid();
      budget_.data(static_cast<std::size_t>(length));
      const auto data = take(static_cast<std::size_t>(length));
      result.value = Bytes(data.begin(), data.end());
      // Apple omits zero- and one-byte data objects from its reference table.
      remember = length > 1;
    } else if (tag >= 0xd0 && tag <= 0xdf) {
      Value::Array array;
      const unsigned count = tag & 15;
      for (unsigned i = 0; count == 15 ? !terminated() : i < count; ++i)
        array.push_back(read(depth + 1));
      result.value = std::move(array);
    } else if (tag >= 0xe0 && tag <= 0xef) {
      Value::Dictionary dictionary;
      const unsigned count = tag & 15;
      for (unsigned i = 0; count == 15 ? !terminated() : i < count; ++i) {
        auto key = read(depth + 1);
        const auto* name = std::get_if<std::string>(&key.value);
        if (!name || dictionary.contains(*name)) invalid();
        dictionary.emplace(*name, read(depth + 1));
      }
      result.value = std::move(dictionary);
    } else
      invalid();
    if (remember) references_.push_back(result);
    return result;
  }
  std::span<const std::uint8_t> input_;
  std::size_t position_ = 0;
  Budget budget_;
  std::vector<Value> references_;
};

class Writer {
 public:
  explicit Writer(OpackLimits limits) : budget_{limits} {}
  Bytes run(const Value& value) {
    write(value, 0);
    return std::move(output_);
  }

 private:
  void byte(std::uint8_t value) {
    if (output_.size() >= budget_.limit.wire_bytes) exhausted();
    output_.push_back(value);
  }
  void little(std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
      byte(static_cast<std::uint8_t>(value >> (i * 8)));
  }
  void sized(std::uint8_t base, std::span<const std::uint8_t> data) {
    budget_.data(data.size());
    if (data.size() <= 32)
      byte(static_cast<std::uint8_t>(base + data.size()));
    else {
      if (data.size() > std::numeric_limits<std::uint32_t>::max()) exhausted();
      const unsigned width = data.size() < 256 ? 1 : data.size() < 65536 ? 2 : 4;
      byte(static_cast<std::uint8_t>(base + 32 + (width == 4 ? 3 : width)));
      little(data.size(), width);
    }
    if (data.size() > budget_.limit.wire_bytes - output_.size()) exhausted();
    output_.insert(output_.end(), data.begin(), data.end());
  }
  void write(const Value& value, unsigned depth) {
    budget_.node(depth);
    if (std::holds_alternative<std::monostate>(value.value))
      byte(4);
    else if (const auto* boolean = std::get_if<bool>(&value.value))
      byte(*boolean ? 1 : 2);
    else if (const auto* number = std::get_if<std::int64_t>(&value.value)) {
      if (*number >= -1 && *number <= 39)
        byte(static_cast<std::uint8_t>(*number + 8));
      else {
        unsigned width, tag;
        if (*number >= -128 && *number <= 127) {
          width = 1;
          tag = 0x30;
        } else if (*number >= -32768 && *number <= 32767) {
          width = 2;
          tag = 0x31;
        } else if (*number >= -2147483648LL && *number <= 2147483647LL) {
          width = 4;
          tag = 0x32;
        } else {
          width = 8;
          tag = 0x33;
        }
        byte(static_cast<std::uint8_t>(tag));
        little(std::bit_cast<std::uint64_t>(*number), width);
      }
    } else if (const auto* real = std::get_if<double>(&value.value)) {
      if (!std::isfinite(*real)) invalid();
      byte(0x36);
      little(std::bit_cast<std::uint64_t>(*real), 8);
    } else if (const auto* text = std::get_if<std::string>(&value.value)) {
      if (!utf8(*text)) invalid();
      sized(0x40, {reinterpret_cast<const std::uint8_t*>(text->data()), text->size()});
    } else if (const auto* data = std::get_if<Bytes>(&value.value))
      sized(0x70, *data);
    else if (const auto* array = std::get_if<Value::Array>(&value.value)) {
      byte(static_cast<std::uint8_t>(0xd0 + std::min<std::size_t>(array->size(), 15)));
      for (const auto& item : *array)
        write(item, depth + 1);
      if (array->size() >= 15) byte(3);
    } else {
      const auto& dictionary = std::get<Value::Dictionary>(value.value);
      byte(static_cast<std::uint8_t>(0xe0 + std::min<std::size_t>(dictionary.size(), 15)));
      for (const auto& [key, item] : dictionary) {
        write(Value{key}, depth + 1);
        write(item, depth + 1);
      }
      if (dictionary.size() >= 15) byte(3);
    }
  }
  Budget budget_;
  Bytes output_;
};
}  // namespace

OpackValue decode_opack(std::span<const std::uint8_t> input, OpackLimits limits) {
  return Reader(input, limits).run();
}
std::vector<std::uint8_t> encode_opack(const OpackValue& value, OpackLimits limits) {
  return Writer(limits).run(value);
}
}  // namespace headunit
