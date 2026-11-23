// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/binary_plist.hpp"
#include <plist/plist.h>
#include <algorithm>
#include <set>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace headunit {
namespace {
[[noreturn]] void reject() {
  throw std::invalid_argument("invalid or unsupported bounded binary plist");
}
struct Budget {
  BinaryPlistLimits limits;
  std::size_t nodes = 0, bytes = 0;
  void charge(std::size_t size) {
    if (size > limits.expanded_bytes - bytes) reject();
    bytes += size;
  }
  void add(unsigned depth, std::size_t size = 0) {
    if (depth > limits.depth || ++nodes > limits.nodes) reject();
    charge(size);
  }
};
// Check references, cycles, depth and repeated expansion before parsing a tree.
struct Graph {
  std::span<const std::uint8_t> data;
  std::size_t offsets = 0, count = 0, root = 0;
  unsigned offset_size = 0, ref_size = 0;
  Budget budget;
  std::vector<std::size_t> ancestors;
  std::uint64_t integer(std::size_t pos, unsigned size, std::size_t end) const {
    if (!size || size > 8 || pos > end || size > end - pos) reject();
    std::uint64_t result = 0;
    for (unsigned i = 0; i < size; ++i)
      result = (result << 8) | data[pos + i];
    return result;
  }
  std::size_t offset(std::size_t id) const {
    if (id >= count) reject();
    const auto result = integer(offsets + id * offset_size, offset_size, data.size() - 32);
    if (result < 8 || result >= offsets) reject();
    return static_cast<std::size_t>(result);
  }
  void range(std::size_t pos, std::uint64_t length) const {
    if (pos > offsets || length > offsets - pos) reject();
  }
  std::u16string visit(std::size_t id, unsigned depth, bool key = false) {
    std::u16string text;
    budget.add(depth);
    if (std::find(ancestors.begin(), ancestors.end(), id) != ancestors.end()) reject();
    ancestors.push_back(id);
    std::size_t pos = offset(id);
    const unsigned marker = data[pos++], kind = marker >> 4, info = marker & 15;
    if (key && kind != 5 && kind != 6) reject();
    if (kind == 0) {
      if (info != 8 && info != 9) reject();
    } else if (kind == 1) {
      if (info > 3) reject();
      range(pos, std::uint64_t{1} << info);
    } else if (kind == 2) {
      if (info != 2 && info != 3) reject();
      range(pos, std::uint64_t{1} << info);
    } else if (kind == 4 || kind == 5 || kind == 6 || kind == 10 || kind == 13) {
      std::uint64_t length = info;
      if (info == 15) {
        range(pos, 1);
        const unsigned length_marker = data[pos++];
        if ((length_marker >> 4) != 1 || (length_marker & 15) > 3) reject();
        const unsigned width = 1u << (length_marker & 15);
        length = integer(pos, width, offsets);
        pos += width;
      }
      const unsigned width = kind == 6 ? 2 : ((kind == 10 || kind == 13) ? ref_size : 1);
      const unsigned multiplier = kind == 13 ? 2 : 1;
      if (length > budget.limits.expanded_bytes / (width * multiplier)) reject();
      range(pos, length * width * multiplier);
      if (kind == 10 || kind == 13) {
        if (length > budget.limits.nodes) reject();
        std::set<std::u16string> keys;
        for (std::uint64_t i = 0; i < length * multiplier; ++i) {
          const auto child =
              integer(pos + static_cast<std::size_t>(i) * ref_size, ref_size, offsets);
          if (child >= count) reject();
          const bool is_key = kind == 13 && i < length;
          auto key_text = visit(static_cast<std::size_t>(child), depth + 1, is_key);
          if (is_key && !keys.insert(std::move(key_text)).second) reject();
        }
      } else {
        const auto expanded = static_cast<std::size_t>(length) * (kind == 6 ? 3 : 1);
        if (expanded > budget.limits.expanded_bytes - budget.bytes) reject();
        budget.bytes += expanded;
        if (kind == 5 || kind == 6) {
          bool high_surrogate = false;
          for (std::uint64_t i = 0; i < length; ++i) {
            const auto unit = static_cast<char16_t>(
                integer(pos + static_cast<std::size_t>(i) * width, width, offsets));
            if (!unit || (kind == 5 && unit >= 128)) reject();
            if (high_surrogate) {
              if (unit < 0xdc00 || unit > 0xdfff) reject();
              high_surrogate = false;
            } else if (unit >= 0xd800 && unit <= 0xdbff)
              high_surrogate = true;
            else if (unit >= 0xdc00 && unit <= 0xdfff)
              reject();
            if (key) text.push_back(unit);
          }
          if (high_surrogate) reject();
        }
      }
    } else
      reject();
    ancestors.pop_back();
    return text;
  }
  void check() {
    if (data.size() < 40 || data.size() > budget.limits.wire_bytes ||
        std::memcmp(data.data(), "bplist00", 8))
      reject();
    const auto trailer = data.size() - 32;
    offset_size = data[trailer + 6];
    ref_size = data[trailer + 7];
    if (!offset_size || offset_size > 8 || !ref_size || ref_size > 8) reject();
    const auto n = integer(trailer + 8, 8, data.size());
    const auto r = integer(trailer + 16, 8, data.size());
    const auto o = integer(trailer + 24, 8, data.size());
    if (!n || n > budget.limits.nodes || r >= n || o < 8 || o > trailer ||
        n > (trailer - o) / offset_size)
      reject();
    count = static_cast<std::size_t>(n);
    root = static_cast<std::size_t>(r);
    offsets = static_cast<std::size_t>(o);
    for (std::size_t i = 0; i < count; ++i)
      (void)offset(i);
    visit(root, 0);
  }
};
bool utf8(std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < 0x80) {
      if (!first) return false;
      continue;
    }
    unsigned extra;
    std::uint32_t value, minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      extra = 1;
      value = first & 31;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      extra = 2;
      value = first & 15;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      extra = 3;
      value = first & 7;
      minimum = 0x10000;
    } else
      return false;
    if (extra > text.size() - i) return false;
    for (unsigned j = 0; j < extra; ++j) {
      const auto next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 63);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}
struct PlistDeleter {
  void operator()(void* value) const {
    if (value) plist_free(value);
  }
};
using Node = std::unique_ptr<void, PlistDeleter>;
struct MemoryDeleter {
  void operator()(void* value) const { plist_mem_free(value); }
};
std::string string(plist_t node) {
  std::uint64_t length = 0;
  const char* text = plist_get_string_ptr(node, &length);
  if (!text || length > plist_expansion_limit) reject();
  std::string result(text, static_cast<std::size_t>(length));
  if (!utf8(result)) reject();
  return result;
}
PlistValue read(plist_t node, unsigned depth, Budget& budget) {
  if (!node) reject();
  budget.add(depth);
  switch (plist_get_node_type(node)) {
    case PLIST_BOOLEAN: {
      std::uint8_t v = 0;
      plist_get_bool_val(node, &v);
      return PlistValue{v != 0};
    }
    case PLIST_UINT: {
      std::int64_t v = 0;
      plist_get_int_val(node, &v);
      return PlistValue{v};
    }
    case PLIST_REAL: {
      double v = 0;
      plist_get_real_val(node, &v);
      if (!std::isfinite(v)) reject();
      return PlistValue{v};
    }
    case PLIST_STRING: {
      auto v = string(node);
      budget.charge(v.size());
      return PlistValue{std::move(v)};
    }
    case PLIST_DATA: {
      std::uint64_t length = 0;
      const auto* v = plist_get_data_ptr(node, &length);
      if (length > plist_expansion_limit || (!v && length)) reject();
      budget.charge(static_cast<std::size_t>(length));
      PlistValue::Bytes data(static_cast<std::size_t>(length));
      if (length) std::memcpy(data.data(), v, data.size());
      return PlistValue{std::move(data)};
    }
    case PLIST_ARRAY: {
      PlistValue::Array values;
      const auto count = plist_array_get_size(node);
      if (count > plist_node_limit) reject();
      values.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i)
        values.push_back(read(plist_array_get_item(node, i), depth + 1, budget));
      return PlistValue{std::move(values)};
    }
    case PLIST_DICT: {
      PlistValue::Dictionary values;
      plist_dict_iter raw = nullptr;
      plist_dict_new_iter(node, &raw);
      if (!raw) throw std::bad_alloc();
      std::unique_ptr<void, MemoryDeleter> iter(raw);
      for (;;) {
        char* key_raw = nullptr;
        plist_t child = nullptr;
        plist_dict_next_item(node, raw, &key_raw, &child);
        std::unique_ptr<void, MemoryDeleter> key_owner(key_raw);
        if (!child) break;
        if (!key_raw) reject();
        std::string key(key_raw);
        if (!utf8(key)) reject();
        budget.add(depth + 1, key.size());
        if (!values.emplace(std::move(key), read(child, depth + 1, budget)).second) reject();
      }
      return PlistValue{std::move(values)};
    }
    default:
      reject();
  }
}
Node write(const PlistValue& value, unsigned depth, Budget& budget) {
  budget.add(depth);
  Node node;
  if (const auto* v = std::get_if<bool>(&value.value))
    node.reset(plist_new_bool(*v));
  else if (const auto* v = std::get_if<std::int64_t>(&value.value))
    node.reset(plist_new_int(*v));
  else if (const auto* v = std::get_if<double>(&value.value)) {
    if (!std::isfinite(*v)) reject();
    node.reset(plist_new_real(*v));
  } else if (const auto* v = std::get_if<std::string>(&value.value)) {
    if (!utf8(*v)) reject();
    budget.charge(v->size());
    node.reset(plist_new_string(v->c_str()));
  } else if (const auto* v = std::get_if<PlistValue::Bytes>(&value.value)) {
    budget.charge(v->size());
    node.reset(plist_new_data(reinterpret_cast<const char*>(v->data()), v->size()));
  } else if (const auto* v = std::get_if<PlistValue::Array>(&value.value)) {
    node.reset(plist_new_array());
    if (!node) throw std::bad_alloc();
    for (const auto& child : *v) {
      auto item = write(child, depth + 1, budget);
      plist_array_append_item(node.get(), item.release());
    }
  } else if (const auto* v = std::get_if<PlistValue::Dictionary>(&value.value)) {
    node.reset(plist_new_dict());
    if (!node) throw std::bad_alloc();
    for (const auto& [key, child] : *v) {
      if (!utf8(key)) reject();
      budget.add(depth + 1, key.size());
      auto item = write(child, depth + 1, budget);
      plist_dict_set_item(node.get(), key.c_str(), item.release());
    }
  }
  if (!node) throw std::bad_alloc();
  return node;
}
}  // namespace
void preflight_binary_plist(std::span<const std::uint8_t> input, BinaryPlistLimits limits) {
  if (!limits.nodes || !limits.expanded_bytes || limits.depth > 64 ||
      limits.wire_bytes > 4 * 1024 * 1024 || limits.nodes > 16384 ||
      limits.expanded_bytes > 4 * 1024 * 1024)
    reject();
  Graph graph{input, 0, 0, 0, 0, 0, {limits}, {}};
  graph.check();
}
PlistValue decode_binary_plist(std::span<const std::uint8_t> input) {
  preflight_binary_plist(input);
  plist_t raw = nullptr;
  const auto status = plist_from_bin(reinterpret_cast<const char*>(input.data()),
                                     static_cast<std::uint32_t>(input.size()), &raw);
  Node root(raw);
  if (status != PLIST_ERR_SUCCESS || !root) reject();
  Budget budget;
  return read(root.get(), 0, budget);
}
PlistValue::Bytes encode_binary_plist(const PlistValue& value) {
  Budget budget;
  auto root = write(value, 0, budget);
  char* raw = nullptr;
  std::uint32_t length = 0;
  const auto status = plist_to_bin(root.get(), &raw, &length);
  std::unique_ptr<void, MemoryDeleter> encoded(raw);
  if (status != PLIST_ERR_SUCCESS || !raw || length > plist_wire_limit) reject();
  return {reinterpret_cast<const std::uint8_t*>(raw),
          reinterpret_cast<const std::uint8_t*>(raw) + length};
}
}  // namespace headunit
