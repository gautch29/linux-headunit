// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/opack.hpp>
#include <plist/plist.h>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <set>

namespace headunit::theme::vehicle::detail {
// Strict bounded JSON reader. libplist handles UTF-8/escape decoding only;
// structure, duplicate decoded keys and exact numeric types are checked here.
class Json {
  std::string_view input_;
  std::size_t pos_ = 0, nodes_ = 0;
  [[noreturn]] void bad() const {
    throw std::invalid_argument("invalid vehicle JSON near byte " + std::to_string(pos_));
  }
  void space() {
    while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\r' ||
                                    input_[pos_] == '\n' || input_[pos_] == '\t'))
      ++pos_;
  }
  bool take(char c) {
    space();
    if (pos_ < input_.size() && input_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  unsigned hex4() {
    unsigned value = 0;
    for (unsigned n = 0; n < 4; ++n) {
      if (pos_ >= input_.size()) bad();
      const char h = input_[pos_++];
      const int digit = h >= '0' && h <= '9'   ? h - '0'
                        : h >= 'a' && h <= 'f' ? h - 'a' + 10
                        : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                               : -1;
      if (digit < 0) bad();
      value = (value << 4) | static_cast<unsigned>(digit);
    }
    return value;
  }
  bool utf8(std::string_view s) const {
    for (std::size_t i = 0; i < s.size();) {
      const auto first = static_cast<unsigned char>(s[i++]);
      if (first < 128) continue;
      const unsigned count = first >= 0xc2 && first <= 0xdf   ? 1
                             : first >= 0xe0 && first <= 0xef ? 2
                             : first >= 0xf0 && first <= 0xf4 ? 3
                                                              : 0;
      if (!count || s.size() - i < count) return false;
      unsigned code = first & ((1u << (6 - count)) - 1u);
      for (unsigned n = 0; n < count; ++n) {
        const auto next = static_cast<unsigned char>(s[i++]);
        if ((next & 0xc0) != 0x80) return false;
        code = (code << 6) | (next & 0x3f);
      }
      if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) ||
          (count == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return false;
    }
    return true;
  }
  std::string string() {
    space();
    const auto start = pos_;
    if (pos_ >= input_.size() || input_[pos_++] != '"') bad();
    bool closed = false;
    while (pos_ < input_.size()) {
      const auto c = static_cast<unsigned char>(input_[pos_++]);
      if (c < 32) bad();
      if (c == '"') {
        closed = true;
        break;
      }
      if (c == '\\') {
        if (pos_ >= input_.size()) bad();
        const auto escape = input_[pos_++];
        if (escape == 'u') {
          const auto code = hex4();
          if (!code || (code >= 0xdc00 && code <= 0xdfff)) bad();
          if (code >= 0xd800 && code <= 0xdbff) {
            if (input_.substr(pos_, 2) != "\\u") bad();
            pos_ += 2;
            const auto low = hex4();
            if (low < 0xdc00 || low > 0xdfff) bad();
          }
        } else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
          bad();
      }
    }
    if (!closed) bad();
    std::string wrapped = "[" + std::string(input_.substr(start, pos_ - start)) + "]";
    plist_t p = nullptr;
    plist_from_json(wrapped.data(), static_cast<std::uint32_t>(wrapped.size()), &p);
    if (!p) bad();
    const auto node = plist_array_get_item(p, 0);
    char* raw = nullptr;
    std::uint64_t length = 0;
    const auto* bytes = plist_get_string_ptr(node, &length);
    std::string result;
    if (bytes)
      result.assign(bytes, length);
    else {
      plist_get_string_val(node, &raw);
      if (raw) result = raw;
      std::free(raw);
    }
    const bool valid = plist_get_node_type(node) == PLIST_STRING &&
                       result.find('\0') == std::string::npos && utf8(result);
    plist_free(p);
    if (!valid) bad();
    return result;
  }
  opack::Value value(unsigned depth) {
    space();
    if (depth > 16 || ++nodes_ > 4096 || pos_ >= input_.size()) bad();
    if (input_[pos_] == '"') return opack::Value{string()};
    if (take('{')) {
      opack::Dictionary pairs;
      std::set<std::string> keys;
      if (take('}')) return opack::Value{std::move(pairs)};
      do {
        auto key = string();
        if (!keys.insert(key).second) bad();
        if (!take(':')) bad();
        pairs.emplace_back(opack::Value{std::move(key)}, value(depth + 1));
        if (take('}')) return opack::Value{std::move(pairs)};
      } while (take(','));
      bad();
    }
    if (take('[')) {
      opack::Array elements;
      if (take(']')) return opack::Value{std::move(elements)};
      do {
        elements.push_back(value(depth + 1));
        if (take(']')) return opack::Value{std::move(elements)};
      } while (take(','));
      bad();
    }
    for (const auto literal :
         {std::string_view("true"), std::string_view("false"), std::string_view("null")})
      if (input_.substr(pos_, literal.size()) == literal) {
        pos_ += literal.size();
        if (literal == "null") return {};
        return opack::Value{literal == "true"};
      }
    const auto start = pos_;
    if (input_[pos_] == '-') ++pos_;
    if (pos_ >= input_.size()) bad();
    const auto digit = [&] {
      return pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9';
    };
    if (input_[pos_] == '0')
      ++pos_;
    else {
      if (!digit()) bad();
      while (digit())
        ++pos_;
    }
    bool real = false;
    if (pos_ < input_.size() && input_[pos_] == '.') {
      real = true;
      ++pos_;
      if (!digit()) bad();
      while (digit())
        ++pos_;
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      real = true;
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
      if (!digit()) bad();
      while (digit())
        ++pos_;
    }
    const auto token = input_.substr(start, pos_ - start);
    if (!real) {
      std::int64_t n = 0;
      const auto r = std::from_chars(token.data(), token.data() + token.size(), n);
      if (r.ec != std::errc{} || r.ptr != token.data() + token.size()) bad();
      return opack::Value{n};
    }
    std::istringstream stream{std::string(token)};
    stream.imbue(std::locale::classic());
    double n = 0;
    stream >> n;
    if (!stream || !stream.eof() || !std::isfinite(n)) bad();
    return opack::Value{n};
  }

 public:
  explicit Json(std::string_view input) : input_(input) {
    if (input.size() > 65536) throw std::invalid_argument("vehicle configuration exceeds 64 KiB");
  }
  opack::Value parse() {
    auto result = value(0);
    space();
    if (pos_ != input_.size()) bad();
    return result;
  }
};
}  // namespace headunit::theme::vehicle::detail
