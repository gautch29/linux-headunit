// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/display_configuration.hpp>
#include <charconv>
#include <fstream>
#include <map>
#include <stdexcept>

namespace headunit {
ReceiverDisplays default_receiver_displays() {
  return {
      {{{1280, 720, 60}, "Center_Display", {}, {}}, {{1920, 720, 60}, "Cluster_Display", {}, {}}}};
}
ReceiverDisplays parse_display_configuration(std::string_view text) {
  if (text.empty() || text.size() > 8192 || text.find('\0') != text.npos)
    throw std::invalid_argument("invalid display configuration size");
  std::map<std::string, std::string> fields;
  while (!text.empty()) {
    auto end = text.find('\n');
    auto line = text.substr(0, end);
    text = end == text.npos ? std::string_view{} : text.substr(end + 1);
    if (line.empty() || line.front() == '#') continue;
    auto equal = line.find('=');
    if (equal == line.npos || equal == 0 || equal + 1 == line.size() ||
        !fields.emplace(line.substr(0, equal), line.substr(equal + 1)).second)
      throw std::invalid_argument("invalid or duplicate display configuration field");
  }
  auto result = default_receiver_displays();
  for (unsigned i = 0; i < 2; ++i) {
    const std::string prefix = i ? "cluster." : "main.";
    auto take = [&](const char* key, bool required = true) {
      auto entry = fields.find(prefix + key);
      if (entry == fields.end()) {
        if (required) throw std::invalid_argument("missing display field: " + prefix + key);
        return std::string{};
      }
      auto value = entry->second;
      fields.erase(entry);
      return value;
    };
    auto number = [&](const char* key, unsigned low, unsigned high, bool required = true) {
      auto value = take(key, required);
      unsigned n = 0;
      if (value.empty()) return n;
      auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
      if (error != std::errc{} || end != value.data() + value.size() || n < low || n > high)
        throw std::invalid_argument("invalid display number: " + prefix + key);
      return n;
    };
    auto name = [&](const char* key, bool required = true) {
      auto value = take(key, required);
      if ((required && value.empty()) || value.size() > 100 || value == "." || value == ".." ||
          value.find_first_not_of(
              "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != value.npos)
        throw std::invalid_argument("invalid display name: " + prefix + key);
      return value;
    };
    auto& d = result[i];
    d.size.width = number("width", 320, 4096);
    d.size.height = number("height", 240, 2160);
    d.size.max_fps = number("fps", 15, 120);
    d.size.width_mm = number("width_mm", 0, 2000, false);
    d.size.height_mm = number("height_mm", 0, 2000, false);
    if (bool(d.size.width_mm) != bool(d.size.height_mm))
      throw std::invalid_argument("physical dimensions must both be known or both zero");
    d.asset_width = number("asset_width", 320, 4096, false);
    d.asset_height = number("asset_height", 240, 2160, false);
    if (bool(d.asset_width) != bool(d.asset_height))
      throw std::invalid_argument("asset canvas dimensions must be supplied together");
    d.panel_name = name("panel");
    d.wayland_display = name("wayland", false);
    d.output_name = name("output", false);
  }
  if (!fields.empty())
    throw std::invalid_argument("unknown display field: " + fields.begin()->first);
  if (result[0].panel_name == result[1].panel_name)
    throw std::invalid_argument("display panel names must differ");
  return result;
}
ReceiverDisplays load_display_configuration(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open display configuration");
  std::string text(8193, '\0');
  input.read(text.data(), text.size());
  text.resize(static_cast<std::size_t>(input.gcount()));
  if (input.bad()) throw std::runtime_error("cannot read display configuration");
  return parse_display_configuration(text);
}
}  // namespace headunit
