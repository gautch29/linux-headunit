// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_identity.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace headunit {
namespace {
std::uint64_t parse_hex(const std::string& text) {
  if (text.size() < 3 || text.substr(0, 2) != "0x" || text.size() > 10 ||
      !std::all_of(text.begin() + 2, text.end(),
                   [](unsigned char character) { return std::isxdigit(character) != 0; })) {
    throw std::runtime_error("invalid AirPlay hexadecimal capability");
  }
  return std::stoull(text.substr(2), nullptr, 16);
}

std::uint64_t parse_features(const std::string& text) {
  const auto separator = text.find(',');
  if (separator == std::string::npos) return parse_hex(text);
  if (text.find(',', separator + 1) != std::string::npos) {
    throw std::runtime_error("invalid AirPlay feature pair");
  }
  const auto low = parse_hex(text.substr(0, separator));
  const auto high = parse_hex(text.substr(separator + 1));
  return low | (high << 32);
}
}  // namespace

AirplayIdentity load_airplay_identity(const std::string& path) {
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
      (info.st_mode & 077) != 0 || info.st_size > 4096) {
    throw std::runtime_error("AirPlay identity must be a private regular file");
  }
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read AirPlay identity");
  std::map<std::string, std::string> fields;
  std::string line;
  while (std::getline(input, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos || separator == 0 ||
        !fields.emplace(line.substr(0, separator), line.substr(separator + 1)).second) {
      throw std::runtime_error("invalid AirPlay identity field");
    }
  }
  constexpr std::string_view required[]{"name",  "deviceid",        "features",          "flags",
                                        "model", "protovers",       "srcvers",           "pi",
                                        "pk",    "main_display_id", "cluster_display_id"};
  constexpr std::string_view optional[]{"manufacturer", "serial", "firmware", "hardware",
                                        "touchscreen_name"};
  if (fields.size() < std::size(required)) {
    throw std::runtime_error("incomplete AirPlay identity");
  }
  for (const auto key : required) {
    const auto found = fields.find(std::string(key));
    if (found == fields.end() || found->second.empty() || found->second.size() > 200 ||
        std::any_of(found->second.begin(), found->second.end(),
                    [](unsigned char character) { return character < 0x20 || character > 0x7e; })) {
      throw std::runtime_error("invalid AirPlay identity value");
    }
  }
  if (fields.at("pk").size() != 64 || fields.at("pi").size() != 36 ||
      fields.at("main_display_id").size() != 36 || fields.at("cluster_display_id").size() != 36 ||
      !std::all_of(fields.at("pk").begin(), fields.at("pk").end(),
                   [](unsigned char character) { return std::isxdigit(character) != 0; })) {
    throw std::runtime_error("invalid AirPlay pairing identity shape");
  }
  for (const auto& [key, value] : fields) {
    if (std::find(std::begin(required), std::end(required), key) == std::end(required) &&
        std::find(std::begin(optional), std::end(optional), key) == std::end(optional))
      throw std::runtime_error("unknown AirPlay identity field");
    if (value.empty() || value.size() > 200 ||
        std::any_of(value.begin(), value.end(),
                    [](unsigned char c) { return c < 0x20 || c > 0x7e; }))
      throw std::runtime_error("invalid AirPlay identity value");
  }
  AirplayIdentity identity{fields.at("name"),
                           fields.at("deviceid"),
                           fields.at("features"),
                           fields.at("flags"),
                           fields.at("model"),
                           fields.at("protovers"),
                           fields.at("srcvers"),
                           fields.at("pi"),
                           fields.at("pk"),
                           fields.at("main_display_id"),
                           fields.at("cluster_display_id")};
  identity.features = parse_features(identity.features_text);
  identity.flags = parse_hex(identity.flags_text);
  auto configured = [&](const char* key, std::string& value) {
    if (auto found = fields.find(key); found != fields.end()) {
      if (found->second.size() > 127) throw std::runtime_error("device identity field is too long");
      value = found->second;
    }
  };
  configured("manufacturer", identity.manufacturer);
  configured("serial", identity.serial);
  configured("firmware", identity.firmware);
  configured("hardware", identity.hardware);
  configured("touchscreen_name", identity.touchscreen_name);
  return identity;
}

Iap2Identity make_iap2_identity(const AirplayIdentity& identity) {
  Iap2Identity result;
  result.name = identity.name;
  result.model = identity.model;
  result.manufacturer = identity.manufacturer;
  result.serial = identity.serial;
  result.firmware = identity.firmware;
  result.hardware = identity.hardware;
  return result;
}

}  // namespace headunit
