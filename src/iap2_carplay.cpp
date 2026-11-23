// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/iap2_carplay.hpp>
#include <headunit/iap2_parameter.hpp>
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
namespace headunit {
namespace {
std::vector<std::uint8_t> text(const std::string& s) {
  if (s.empty() || s.size() > 127 || s.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid CarPlay identity string");
  std::vector<std::uint8_t> b(s.begin(), s.end());
  b.push_back(0);
  return b;
}
std::vector<std::uint8_t> word(std::uint32_t n) {
  return {std::uint8_t(n >> 24), std::uint8_t(n >> 16), std::uint8_t(n >> 8), std::uint8_t(n)};
}
bool hex(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}
}  // namespace
bool wireless_carplay_available(const ControlMessage& message) {
  if (message.identifier != 0x4300) throw std::invalid_argument("expected CarPlay availability");
  bool available = false;
  std::set<std::uint16_t> groups;
  for (const auto& group : decode_iap2_parameters(message.parameters)) {
    if (!groups.insert(group.identifier).second)
      throw std::invalid_argument("duplicate CarPlay availability group");
    if (group.identifier > 2) continue;
    bool found = false;
    std::set<std::uint16_t> fields;
    for (const auto& field : decode_iap2_parameters(group.value)) {
      if (!fields.insert(field.identifier).second)
        throw std::invalid_argument("duplicate CarPlay availability field");
      if (field.identifier != 0) continue;
      if (field.value.size() != 1 || field.value[0] > 1)
        throw std::invalid_argument("invalid CarPlay availability boolean");
      found = true;
      if (group.identifier == 1) available = field.value[0] == 1;
    }
    if (!found) throw std::invalid_argument("missing CarPlay availability boolean");
  }
  return available;
}
ControlMessage make_carplay_start(const Iap2CarplayStart& config) {
  (void)Iap2WifiHandoff(config.wifi);  // Same SSID/security bounds as authenticated handoff.
  if (config.port == 0 || config.port > 65535 || config.address != "10.10.0.1" ||
      config.identity.public_key_hex.size() != 64 || !hex(config.identity.public_key_hex))
    throw std::invalid_argument("invalid CarPlay lab endpoint");
  const std::array<Iap2Parameter, 5> wireless{{{0, text(config.wifi.ssid)},
                                               {1, text(config.wifi.passphrase)},
                                               {2, {config.wifi.channel}},
                                               {3, text(config.address)},
                                               {4, {config.wifi.security}}}};
  std::vector<Iap2Parameter> fields{{1, encode_iap2_parameters(wireless)},
                                    {2, word(config.port)},
                                    {3, text(config.identity.device_id)},
                                    {4, text(config.identity.public_key_hex)},
                                    {5, text(config.identity.source_version)}};
  if (config.installed_asset) {
    const auto& asset = *config.installed_asset;
    if (asset.identifier.empty() || asset.identifier.size() > 128 ||
        !std::all_of(asset.identifier.begin(), asset.identifier.end(),
                     [](char c) {
                       return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                     }) ||
        asset.version == 0 || asset.version > 0x7fffffff || asset.sdk_version != "19.0")
      throw std::invalid_argument("unsupported installed CarPlay asset metadata");
    fields.push_back({6, text(asset.sdk_version)});
    const std::array<Iap2Parameter, 2> info{
        {{0, text(asset.identifier)}, {1, word(asset.version)}}};
    fields.push_back({7, encode_iap2_parameters(info)});
  }
  return {0x4301, encode_iap2_parameters(fields)};
}
}  // namespace headunit
