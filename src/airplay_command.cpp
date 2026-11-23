// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_command.hpp"
#include "headunit/control_request.hpp"

#include <limits>
#include <stdexcept>
#include <string_view>

namespace headunit {
namespace {
using Dictionary = PlistValue::Dictionary;
void check(bool valid) {
  if (!valid) throw std::invalid_argument("invalid AirPlay command");
}
const Dictionary& dictionary(const PlistValue& value) {
  const auto* result = std::get_if<Dictionary>(&value.value);
  check(result != nullptr);
  return *result;
}
std::uint32_t number(const Dictionary& object, std::string_view key) {
  const auto item = object.find(key);
  check(item != object.end());
  const auto* value = std::get_if<std::int64_t>(&item->second.value);
  check(value && *value >= 0 && *value <= std::numeric_limits<std::uint32_t>::max());
  return static_cast<std::uint32_t>(*value);
}
std::optional<std::uint32_t> optional_number(const Dictionary& object, std::string_view key) {
  if (!object.contains(key)) return {};
  return number(object, key);
}
std::optional<std::int64_t> speech_mode(const Dictionary& object) {
  const auto item = object.find("speechMode");
  if (item == object.end()) return {};
  const auto* value = std::get_if<std::int64_t>(&item->second.value);
  check(value && *value >= -1 && *value <= std::numeric_limits<std::uint32_t>::max());
  return *value;
}
void keys(const Dictionary& object, std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, value] : object) {
    (void)value;
    bool found = false;
    for (const auto name : allowed)
      found |= name == key;
    check(found);
  }
}
template <typename Mode, std::size_t N, typename Parse>
void modes(const Dictionary& params, const char* key, const char* id_key,
           std::array<std::optional<Mode>, N>& result, Parse parse) {
  const auto item = params.find(key);
  if (item == params.end()) return;
  const auto* list = std::get_if<PlistValue::Array>(&item->second.value);
  check(list && list->size() <= N);
  for (const auto& value : *list) {
    const auto& object = dictionary(value);
    const auto id = number(object, id_key);
    check(id >= 1 && id <= N && !result[id - 1]);
    result[id - 1] = parse(object);
  }
}
}  // namespace

AirplayCommand decode_airplay_command(std::span<const std::uint8_t> bytes) {
  preflight_binary_plist(bytes, {65536, 512, 131072, 16});
  const auto value = decode_binary_plist(bytes);
  const auto& root = dictionary(value);
  const auto item = root.find("type");
  check(item != root.end());
  const auto* type = std::get_if<std::string>(&item->second.value);
  check(type && !type->empty() && type->size() <= 64);
  AirplayCommand result;
  if (*type == "duckAudio") result.kind = AirplayCommandKind::duck_audio;
  if (*type == "unduckAudio") result.kind = AirplayCommandKind::unduck_audio;
  if (*type == "suggestUI") result.kind = AirplayCommandKind::suggest_ui;
  if (*type == "disableBluetooth") result.kind = AirplayCommandKind::disable_bluetooth;
  if (*type != "modesChanged") {
    if (result.kind == AirplayCommandKind::unsupported)
      result.unsupported_fingerprint = protocol_fingerprint(*type);
    return result;
  }
  keys(root, {"type", "params"});
  const auto parameters = root.find("params");
  check(parameters != root.end());
  const auto& params = dictionary(parameters->second);
  keys(params, {"appStates", "resources"});
  check(params.contains("appStates") || params.contains("resources"));
  AirplayModesUpdate update;
  modes(params, "resources", "resourceID", update.resources, [](const Dictionary& object) {
    keys(object, {"resourceID", "entity", "permanentEntity"});
    return AirplayResourceMode{number(object, "entity"),
                               optional_number(object, "permanentEntity")};
  });
  modes(params, "appStates", "appStateID", update.apps, [](const Dictionary& object) {
    keys(object, {"appStateID", "entity", "speechMode"});
    return AirplayAppMode{number(object, "entity"), speech_mode(object)};
  });
  result.kind = AirplayCommandKind::modes_changed;
  result.modes = std::move(update);
  return result;
}

AirplayResourceOwner airplay_resource_owner(const std::optional<AirplayResourceMode>& mode) {
  if (!mode) return AirplayResourceOwner::unobserved;
  if (mode->entity == 1) return AirplayResourceOwner::controller;
  if (mode->entity == 2) return AirplayResourceOwner::accessory;
  return AirplayResourceOwner::unknown;
}

void AirplayModesState::apply(const AirplayModesUpdate& update,
                              std::chrono::steady_clock::time_point now) {
  if ((observed_at && now < *observed_at) || revision == std::numeric_limits<std::uint64_t>::max())
    throw std::invalid_argument("invalid AirPlay mode observation time or revision");
  for (std::size_t i = 0; i < resources.size(); ++i)
    if (update.resources[i]) {
      resources[i] = update.resources[i];
      resource_observed_at[i] = now;
    }
  for (std::size_t i = 0; i < apps.size(); ++i)
    if (update.apps[i]) {
      apps[i] = update.apps[i];
      app_observed_at[i] = now;
    }
  observed_at = now;
  ++revision;
}

std::string airplay_modes_summary(const AirplayModesUpdate& modes) {
  const auto entity = [](const std::optional<std::uint32_t>& value) {
    return value ? std::to_string(*value) : std::string("-");
  };
  constexpr std::array<const char*, 2> resource_names{"mainScreen", "mainAudio"};
  constexpr std::array<const char*, 3> app_names{"speech", "phoneCall", "turnByTurn"};
  std::string summary;
  for (std::size_t i = 0; i < modes.resources.size(); ++i) {
    const auto& resource = modes.resources[i];
    if (!summary.empty()) summary += ' ';
    summary += resource_names[i];
    summary += '=';
    summary += entity(resource ? std::optional<std::uint32_t>(resource->entity) : std::nullopt);
    summary += '/';
    summary += entity(resource ? resource->permanent_entity : std::nullopt);
  }
  for (std::size_t i = 0; i < modes.apps.size(); ++i) {
    const auto& app = modes.apps[i];
    summary += ' ';
    summary += app_names[i];
    summary += '=';
    summary += entity(app ? std::optional<std::uint32_t>(app->entity) : std::nullopt);
  }
  return summary;
}

const char* airplay_command_name(AirplayCommandKind kind) {
  switch (kind) {
    case AirplayCommandKind::modes_changed:
      return "modesChanged";
    case AirplayCommandKind::duck_audio:
      return "duckAudio";
    case AirplayCommandKind::unduck_audio:
      return "unduckAudio";
    case AirplayCommandKind::suggest_ui:
      return "suggestUI";
    case AirplayCommandKind::disable_bluetooth:
      return "disableBluetooth";
    default:
      return "unsupported";
  }
}
}  // namespace headunit
