// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/hud_configuration.hpp>

#include <charconv>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <stdexcept>

namespace headunit {
HudConfiguration parse_hud_configuration(std::string_view text) {
  if (text.empty() || text.size() > 4096 || text.find('\0') != text.npos)
    throw std::invalid_argument("invalid HUD configuration size");
  std::map<std::string, std::string> fields;
  while (!text.empty()) {
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text = end == text.npos ? std::string_view{} : text.substr(end + 1);
    if (line.empty() || line.front() == '#') continue;
    const auto equal = line.find('=');
    if (equal == line.npos || equal == 0 || equal + 1 == line.size() ||
        !fields.emplace(line.substr(0, equal), line.substr(equal + 1)).second)
      throw std::invalid_argument("invalid or duplicate HUD field");
  }
  auto take = [&](const char* key, std::string fallback) {
    const auto entry = fields.find(key);
    if (entry == fields.end()) return fallback;
    auto result = entry->second;
    fields.erase(entry);
    return result;
  };
  auto name = [&](const char* key, const std::string& fallback) {
    auto result = take(key, fallback);
    if (result.empty() || result.size() > 100 || result == "." || result == ".." ||
        result.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != result.npos)
      throw std::invalid_argument(std::string("invalid HUD name: ") + key);
    return result;
  };
  auto integer = [&](const char* key, unsigned fallback, unsigned low, unsigned high) {
    const auto value = take(key, std::to_string(fallback));
    unsigned result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result < low || result > high)
      throw std::invalid_argument(std::string("invalid HUD number: ") + key);
    return result;
  };
  auto real = [&](const char* key, double fallback, double low, double high) {
    const auto value = take(key, std::to_string(fallback));
    double result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result) ||
        result < low || result > high)
      throw std::invalid_argument(std::string("invalid HUD calibration: ") + key);
    return result;
  };
  auto boolean = [&](const char* key, bool fallback) {
    const auto value = take(key, fallback ? "true" : "false");
    if (value != "true" && value != "false")
      throw std::invalid_argument(std::string("invalid HUD boolean: ") + key);
    return value == "true";
  };
  HudConfiguration config;
  config.output_name = name("output", "");
  config.wayland_display = name("wayland", config.wayland_display);
  config.main_output = name("main_output", config.main_output);
  config.cluster_output = name("cluster_output", config.cluster_output);
  if (config.output_name == config.main_output || config.output_name == config.cluster_output ||
      config.main_output == config.cluster_output)
    throw std::invalid_argument("HUD, main and cluster need distinct output names");
  config.width = integer("width", config.width, 320, 1920);
  config.height = integer("height", config.height, 160, 1080);
  config.fps = integer("fps", config.fps, 5, 30);
  config.country = take("country", "auto");
  if (config.country == "auto") config.country.clear();
  if (!valid_hud_country(config.country)) throw std::invalid_argument("invalid HUD country");
  auto& calibration = config.calibration;
  calibration.reflect_horizontal = boolean("reflect_horizontal", true);
  calibration.reflect_vertical = boolean("reflect_vertical", false);
  calibration.quarter_turns = integer("quarter_turns", 0, 0, 3);
  calibration.scale = real("scale", 1, 0.25, 1);
  calibration.offset_x = real("offset_x", 0, -0.5, 0.5);
  calibration.offset_y = real("offset_y", 0, -0.5, 0.5);
  const char* corner_names[]{"top_left", "top_right", "bottom_right", "bottom_left"};
  unsigned present = 0;
  for (const auto* corner : corner_names)
    present +=
        fields.contains(std::string(corner) + "_x") + fields.contains(std::string(corner) + "_y");
  if (present && present != 8)
    throw std::invalid_argument("HUD correction requires all four complete corners");
  if (present) {
    std::array<HudPoint, 4> corners;
    for (unsigned i = 0; i < 4; ++i) {
      corners[i].x = real((std::string(corner_names[i]) + "_x").c_str(), 0, 0, 1);
      corners[i].y = real((std::string(corner_names[i]) + "_y").c_str(), 0, 0, 1);
    }
    calibration.corners = corners;
  }
  if (!fields.empty()) throw std::invalid_argument("unknown HUD field: " + fields.begin()->first);
  // The renderer shares the same geometry validation with snapshots and live output.
  if (!valid_hud_calibration(calibration))
    throw std::invalid_argument("invalid HUD correction geometry");
  return config;
}

HudConfiguration load_hud_configuration(const std::string& path) {
  // HUD configuration is read before presentation begins. A pipe or device
  // must not hold optional display startup in an unbounded read.
  // Explicit source symlinks remain supported; pin their resolved target.
  std::error_code error;
  const auto origin = std::filesystem::canonical(path, error);
  if (error) throw std::runtime_error("cannot resolve HUD configuration");
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) throw std::runtime_error("cannot open HUD configuration");
  struct Close {
    int descriptor;
    ~Close() { close(descriptor); }
  } owner{fd};
  struct stat before{}, after{}, named{};
  if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
      before.st_size > 4096)
    throw std::invalid_argument("HUD configuration must be a bounded regular file");
  std::string text(std::size_t(before.st_size), '\0');
  std::size_t offset = 0;
  unsigned interrupted = 0;
  while (offset < text.size()) {
    const auto count = read(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR && ++interrupted <= 3) continue;
    if (count <= 0) throw std::runtime_error("cannot read HUD configuration");
    offset += std::size_t(count);
  }
  const auto same = [](const struct stat& a, const struct stat& b) {
    const bool identity = a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
#ifdef __APPLE__
    return identity && a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
           a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
           a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec &&
           a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
#else
    return identity && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
           a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
#endif
  };
  const auto final_origin = std::filesystem::canonical(path, error);
  if (error || final_origin != origin || fstat(fd, &after) != 0 ||
      stat(path.c_str(), &named) != 0 || !S_ISREG(named.st_mode) || !same(before, after) ||
      !same(after, named))
    throw std::runtime_error("HUD configuration changed during acquisition");
  return parse_hud_configuration(text);
}
}  // namespace headunit
