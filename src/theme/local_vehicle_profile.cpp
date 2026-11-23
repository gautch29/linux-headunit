// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <tuple>
#include <map>
#include <headunit/theme/local_vehicle_profile.hpp>
#include <plist/plist.h>
#include <headunit/vehicle.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <stdexcept>

namespace headunit::theme::vehicle {
namespace {
using opack::Array;
using opack::Dictionary;
using opack::Value;
void check(bool value, const char* reason) {
  if (!value) throw std::invalid_argument(reason);
}
Value text(const char* value) {
  return Value{std::string(value)};
}
plist_t field(plist_t parent, const char* name) {
  return plist_dict_get_item(parent, name);
}
void keys(plist_t node, const std::set<std::string>& allowed) {
  check(node && plist_get_node_type(node) == PLIST_DICT, "local profile dictionary required");
  check(plist_dict_get_size(node) <= allowed.size(), "unknown local profile field");
  plist_dict_iter raw = nullptr;
  plist_dict_new_iter(node, &raw);
  std::unique_ptr<void, decltype(&std::free)> iterator(raw, std::free);
  for (;;) {
    char* name = nullptr;
    plist_t value = nullptr;
    plist_dict_next_item(node, raw, &name, &value);
    std::unique_ptr<char, decltype(&std::free)> owned(name, std::free);
    if (!value) break;
    check(name && allowed.contains(name), "unknown local profile field");
  }
}
std::string string(plist_t node, std::size_t maximum = 128, bool allow_empty = false) {
  check(node && plist_get_node_type(node) == PLIST_STRING, "local profile string required");
  std::uint64_t n = 0;
  const char* value = plist_get_string_ptr(node, &n);
  check(value && (allow_empty || n > 0) && n <= maximum, "invalid local profile string");
  std::string result(value, n);
  check(result.find('\0') == result.npos, "embedded NUL in local profile string");
  return result;
}
std::uint64_t integer(plist_t node, std::uint64_t maximum) {
  check(node && plist_get_node_type(node) == PLIST_INT, "local profile integer required");
  std::uint64_t value = 0;
  plist_get_uint_val(node, &value);
  check(value <= maximum, "local profile integer out of range");
  return value;
}
std::int64_t exact_integer(plist_t node) {
  check(node && plist_get_node_type(node) == PLIST_INT, "local profile integer required");
  if (!plist_int_val_is_negative(node)) return std::int64_t(integer(node, 9007199254740991ULL));
  std::int64_t value = 0;
  plist_get_int_val(node, &value);
  check(value >= -9007199254740991LL, "inexact local profile integer");
  return value;
}
double number(plist_t node) {
  check(node, "missing local profile bound");
  double value = 0;
  if (plist_get_node_type(node) == PLIST_REAL)
    plist_get_real_val(node, &value);
  else {
    check(plist_get_node_type(node) == PLIST_INT, "numeric local profile bound required");
    value = double(exact_integer(node));
  }
  check(std::isfinite(value), "nonfinite local profile bound");
  return value;
}
Value scalar(plist_t node) {
  switch (plist_get_node_type(node)) {
    case PLIST_BOOLEAN: {
      std::uint8_t value = 0;
      plist_get_bool_val(node, &value);
      return Value{bool(value)};
    }
    case PLIST_INT:
      return Value{exact_integer(node)};
    case PLIST_REAL:
      return Value{number(node)};
    case PLIST_STRING:
      return Value{string(node, 1024, true)};
    case PLIST_DATA: {
      std::uint64_t n = 0;
      const auto* p = plist_get_data_ptr(node, &n);
      check((p || n == 0) && n <= 1024, "oversized fixed data");
      return Value{n ? Bytes(p, p + n) : Bytes{}};
    }
    default:
      throw std::invalid_argument("fixed configuration must be a scalar");
  }
}
Value fixed_scalar_array(plist_t node) {
  check(node && plist_get_node_type(node) == PLIST_ARRAY, "fixed scalar array required");
  const auto count = plist_array_get_size(node);
  check(count <= 32, "fixed array exceeds 32 items");
  Array values;
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto item = plist_array_get_item(node, i);
    const auto type = plist_get_node_type(item);
    check(type == PLIST_BOOLEAN || type == PLIST_INT || type == PLIST_REAL || type == PLIST_STRING,
          "fixed array items must be primitive scalar values");
    values.push_back(type == PLIST_STRING ? Value{string(item, 128, true)} : scalar(item));
  }
  Value value{std::move(values)};
  check(opack::encode(value).size() <= 1024, "fixed array exceeds 1 KiB");
  return value;
}
bool authored_identifier(std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto byte = static_cast<unsigned char>(value[i]);
    if (byte < 32 || byte == 127) return false;
    if (byte == 0xc2 && i + 1 < value.size()) {
      const auto next = static_cast<unsigned char>(value[i + 1]);
      if (next >= 0x80 && next <= 0x9f) return false;
    }
  }
  return true;
}
bool trip_field(std::string_view name) {
  return name == "trip.fuel_efficiency_l_per_100km";
}
bool transmission_field(std::string_view name) {
  return name == "transmission.selector" || name == "transmission.gear" ||
         name == "transmission.gearbox_type";
}
bool transmission_key(std::string_view name, std::string_view key) {
  if (name == "transmission.selector") return parse_transmission_selector_key(key).has_value();
  if (name == "transmission.gear") return parse_engaged_gear_key(key).has_value();
  if (name == "transmission.gearbox_type") return parse_gearbox_type_key(key).has_value();
  return false;
}
void semantic_mapping(VehicleInputBinding& mapping, plist_t states, const SignalPolicy& policy) {
  check(states && plist_get_node_type(states) == PLIST_DICT,
        "semantic input needs explicit state mappings");
  const auto count = plist_dict_get_size(states);
  const std::uint32_t maximum = mapping.field == "drive_mode"              ? 32
                                : mapping.field == "transmission.gear"     ? 34
                                : mapping.field == "transmission.selector" ? 7
                                                                           : 4;
  check(count > 0 && count <= maximum, "invalid semantic mapping size");
  plist_dict_iter raw = nullptr;
  plist_dict_new_iter(states, &raw);
  std::unique_ptr<void, decltype(&std::free)> iterator(raw, std::free);
  for (;;) {
    char* name = nullptr;
    plist_t item = nullptr;
    plist_dict_next_item(states, raw, &name, &item);
    std::unique_ptr<char, decltype(&std::free)> owned(name, std::free);
    if (!item) break;
    check(name && (mapping.field == "drive_mode" ? parse_drive_mode_identifier(name).has_value()
                                                 : transmission_key(mapping.field, name)),
          "unknown semantic input state");
    Value value;
    if (mapping.kind == ValueKind::integer) {
      value = Value{exact_integer(item)};
      const double n = double(std::get<std::int64_t>(value.data));
      check((!policy.minimum || n >= *policy.minimum) && (!policy.maximum || n <= *policy.maximum),
            "semantic state outside profile bounds");
    } else
      value = Value{string(item, 128)};
    check(mapping.semantic_states.emplace(name, std::move(value)).second,
          "duplicate semantic input state");
  }
}
const Value* value_field(const Value& value, std::string_view key) {
  const auto* dictionary = std::get_if<Dictionary>(&value.data);
  check(dictionary, "local source configuration dictionary required");
  for (const auto& [name, item] : *dictionary)
    if (const auto* text = std::get_if<std::string>(&name.data); text && *text == key) return &item;
  return nullptr;
}
const Array& value_array(const Value& value, const char* key) {
  const auto* item = value_field(value, key);
  const auto* array = item ? std::get_if<Array>(&item->data) : nullptr;
  check(array, "local source configuration array required");
  return *array;
}
const Value* characteristic(const Value& configuration, std::int64_t id) {
  for (const auto& accessory : value_array(configuration, "accessories"))
    for (const auto& service : value_array(accessory, "services"))
      for (const auto& item : value_array(service, "characteristics")) {
        const auto* number = value_field(item, "iid");
        if (number && number->data == Value{id}.data) return &item;
      }
  return nullptr;
}
}  // namespace
void validate_local_vehicle_inputs(const LocalProfile& profile) {
  check(bool(profile.source), "local vehicle inputs need a source");
  std::optional<Value> configuration;
  for (const auto& input : profile.vehicle_inputs) {
    if (trip_field(input.field)) {
      const auto policy = profile.source->policy(input.iid);
      check(profile.source->timed(input.iid) && policy && policy->kind == input.kind &&
                (input.kind == ValueKind::real || input.kind == ValueKind::integer),
            "trip consumption needs a timed numeric mapping");
      check(std::isfinite(input.scale) && std::isfinite(input.offset) && input.scale != 0 &&
                std::abs(input.scale) <= 1000000 && std::abs(input.offset) <= 1000000,
            "invalid trip unit conversion");
      check(input.states.empty() && input.semantic_states.empty(),
            "trip consumption cannot map semantic or lamp states");
      check((!input.minimum || std::isfinite(*input.minimum)) &&
                (!input.maximum || std::isfinite(*input.maximum)) &&
                (!input.minimum || !input.maximum || *input.minimum <= *input.maximum),
            "invalid trip mapping bounds");
      continue;
    }
    if (input.field != "drive_mode") continue;
    check(profile.source->timed(input.iid) &&
              (input.kind == ValueKind::integer || input.kind == ValueKind::string),
          "drive mode needs a timed integer or string mapping");
    check(input.scale == 1 && input.offset == 0 && input.states.empty(),
          "drive mode cannot use numeric conversion or lamp states");
    check(!input.semantic_states.empty() && input.semantic_states.size() <= 32,
          "invalid drive-mode mapping size");
    const auto policy = profile.source->policy(input.iid);
    check(policy && policy->kind == input.kind, "drive-mode source kind does not match mapping");
    for (const auto& [key, value] : input.semantic_states) {
      check(parse_drive_mode_identifier(key).has_value(), "invalid semantic drive-mode identifier");
      if (input.kind == ValueKind::integer) {
        const auto* n = std::get_if<std::int64_t>(&value.data);
        check(n && *n >= -9007199254740991LL && *n <= 9007199254740991LL &&
                  (!policy->minimum || double(*n) >= *policy->minimum) &&
                  (!policy->maximum || double(*n) <= *policy->maximum),
              "drive-mode state outside profile type or bounds");
      } else {
        const auto* text = std::get_if<std::string>(&value.data);
        check(text && !text->empty() && text->size() <= 128 && text->find('\0') == text->npos,
              "invalid drive-mode string state");
      }
    }
    const ScriptBinding* index_binding = nullptr;
    for (const auto& item : profile.bindings)
      if (item.iid == input.iid) {
        check(!index_binding, "ambiguous drive-mode source binding");
        index_binding = &item;
      }
    check(index_binding, "drive-mode source binding missing");
    if (index_binding->path !=
        std::array<std::string, 3>{"driveState", "driveMode", "currentIndex"})
      continue;
    check(input.kind == ValueKind::integer, "current drive-mode index needs an integer mapping");
    const ScriptBinding* identifiers_binding = nullptr;
    for (const auto& item : profile.bindings)
      if (item.path == std::array<std::string, 3>{"driveState", "driveMode", "identifiers"} &&
          item.instance == index_binding->instance) {
        check(!identifiers_binding, "ambiguous authored drive-mode identifiers");
        identifiers_binding = &item;
      }
    check(identifiers_binding && !profile.source->timed(identifiers_binding->iid),
          "current drive-mode index needs fixed authored identifiers");
    // Only read immutable configuration. This does not advance the source
    // clock or expose an active reading as fixed catalogue configuration.
    if (!configuration)
      configuration =
          profile.source->configuration_snapshot(std::numeric_limits<std::uint64_t>::max());
    const auto* declaration = characteristic(*configuration, identifiers_binding->iid);
    const auto* initial = declaration ? value_field(*declaration, "initialValue") : nullptr;
    const auto* identifiers = initial ? std::get_if<Array>(&initial->data) : nullptr;
    check(identifiers && !identifiers->empty() && identifiers->size() <= 32 &&
              opack::encode(*initial).size() <= 1024,
          "invalid authored drive-mode identifiers array");
    std::set<std::string> unique;
    for (const auto& value : *identifiers) {
      const auto* text = std::get_if<std::string>(&value.data);
      check(text && authored_identifier(*text) && unique.insert(*text).second,
            "authored drive-mode identifiers must be unique nonempty strings");
    }
    std::set<std::int64_t> indices;
    for (const auto& [key, value] : input.semantic_states) {
      (void)key;
      const auto index = std::get<std::int64_t>(value.data);
      check(
          index >= 0 && std::uint64_t(index) < identifiers->size() && indices.insert(index).second,
          "drive-mode index outside or ambiguous within authored identifiers");
    }
  }
}
LocalProfile decode_local_profile(std::span<const std::uint8_t> bytes) {
  check(!bytes.empty() && bytes.size() <= 256 * 1024, "invalid local profile size");
  plist_t raw = nullptr;
  plist_format_t format = PLIST_FORMAT_NONE;
  const auto status =
      plist_from_memory(reinterpret_cast<const char*>(bytes.data()), bytes.size(), &raw, &format);
  std::unique_ptr<void, decltype(&plist_free)> root(raw, plist_free);
  check(status == PLIST_ERR_SUCCESS && raw &&
            (format == PLIST_FORMAT_BINARY || format == PLIST_FORMAT_XML),
        "invalid local profile plist");
  keys(raw, {"schemaVersion", "mode", "signals", "vehicleIdentity"});
  check(integer(field(raw, "schemaVersion"), 1) == 1 &&
            string(field(raw, "mode")) == "local-ui-bindings",
        "unsupported local profile schema");
  const auto list = field(raw, "signals");
  check(list && plist_get_node_type(list) == PLIST_ARRAY, "local profile signals array required");
  const auto count = plist_array_get_size(list);
  check(count <= 512, "too many local profile signals");
  Array characteristics;
  std::vector<SignalPolicy> policies;
  std::vector<ScriptBinding> bindings;
  std::vector<VehicleInputBinding> inputs;
  std::map<std::int64_t, Value> fixed;
  std::optional<std::int64_t> scene_state, scene_options;
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto node = plist_array_get_item(list, index);
    keys(node, {"iid", "path", "kind", "maxAgeMs", "minimumValue", "maximumValue", "units",
                "scriptFormat", "fixedValue", "instance", "vehicleField", "vehicleScale",
                "vehicleOffset", "vehicleStates", "sessionField"});
    const auto id = std::int64_t(integer(field(node, "iid"), 9007199254740991ULL));
    check(id >= 3, "local profile IID is reserved");
    const auto path = field(node, "path");
    check(path && plist_get_node_type(path) == PLIST_ARRAY && plist_array_get_size(path) == 3,
          "local profile semantic path required");
    ScriptBinding binding{
        id,
        {string(plist_array_get_item(path, 0)), string(plist_array_get_item(path, 1)),
         string(plist_array_get_item(path, 2))},
        {}};
    if (const auto instance = field(node, "instance")) binding.instance = string(instance);
    const auto kind = string(field(node, "kind"));
    ValueKind type;
    if (kind == "boolean")
      type = ValueKind::boolean;
    else if (kind == "integer")
      type = ValueKind::integer;
    else if (kind == "real")
      type = ValueKind::real;
    else if (kind == "string")
      type = ValueKind::string;
    else if (kind == "bytes")
      type = ValueKind::bytes;
    else if (kind == "array") {
      check((field(node, "fixedValue") || field(node, "sessionField")) &&
                !field(node, "vehicleField"),
            "array signal requires fixed configuration or managed scene options");
      check(!field(node, "minimumValue") && !field(node, "maximumValue"),
            "array signals have no scalar numeric bounds");
      type = ValueKind::array;
    } else
      throw std::invalid_argument("unknown local signal kind");
    if (const auto owner = field(node, "sessionField")) {
      check(!field(node, "fixedValue") && !field(node, "vehicleField") && !binding.instance &&
                !field(node, "minimumValue") && !field(node, "maximumValue") &&
                !field(node, "units") && !field(node, "scriptFormat"),
            "session input must be an unmodified timed singleton");
      const auto name = string(owner);
      if (name == "scene_state") {
        check(
            !scene_state && type == ValueKind::integer &&
                binding.path == std::array<std::string, 3>{"uiControl", "uiState", "uiSceneState"},
            "invalid session scene-state binding");
        scene_state = id;
      } else {
        check(name == "scene_options" && !scene_options && type == ValueKind::array &&
                  binding.path ==
                      std::array<std::string, 3>{"uiControl", "uiState", "uiSceneOptions"},
              "invalid session scene-options binding");
        scene_options = id;
      }
    }
    const auto age = integer(field(node, "maxAgeMs"), 600000);
    check(age > 0, "local sample age must be positive");
    SignalPolicy policy{id, type, age, {}, {}};
    Dictionary metadata{{text("iid"), Value{id}}, {text("supportsInvalid"), Value{true}}};
    for (const auto* name : {"minimumValue", "maximumValue"})
      if (const auto value = field(node, name)) {
        const auto bound = number(value);
        if (std::string_view(name) == "minimumValue")
          policy.minimum = bound;
        else
          policy.maximum = bound;
        metadata.emplace_back(text(name), Value{bound});
      }
    if (const auto value = field(node, "units"))
      metadata.emplace_back(text("units"), Value{std::int64_t(integer(value, 0x7fffffff))});
    if (const auto value = field(node, "scriptFormat"))
      binding.format = std::int32_t(integer(value, 0x7fffffff));
    if (const auto value = field(node, "fixedValue"))
      check(fixed.emplace(id, type == ValueKind::array ? fixed_scalar_array(value) : scalar(value))
                .second,
            "duplicate fixed IID");
    if (const auto input = field(node, "vehicleField")) {
      check(!field(node, "fixedValue"), "vehicle input cannot be fixed configuration");
      VehicleInputBinding mapping{id, string(input),  type,           1,
                                  0,  policy.minimum, policy.maximum, {}};
      const auto reading = std::find_if(reading_definitions.begin(), reading_definitions.end(),
                                        [&](const auto& d) { return d.key == mapping.field; });
      const auto indicator =
          std::find_if(indicator_definitions.begin(), indicator_definitions.end(),
                       [&](const auto& d) { return d.key == mapping.field; });
      const bool transmission = transmission_field(mapping.field);
      const bool drive_mode = mapping.field == "drive_mode";
      const bool trip = trip_field(mapping.field);
      check(reading != reading_definitions.end() || indicator != indicator_definitions.end() ||
                transmission || drive_mode || trip,
            "unknown vehicle source field");
      if (reading != reading_definitions.end() || trip) {
        check(type == ValueKind::real || type == ValueKind::integer,
              "numeric input needs a numeric script signal");
        check(!field(node, "vehicleStates"), "numeric input cannot map lamp states");
        if (const auto n = field(node, "vehicleScale")) mapping.scale = number(n);
        if (const auto n = field(node, "vehicleOffset")) mapping.offset = number(n);
        check(mapping.scale != 0 && std::abs(mapping.scale) <= 1000000 &&
                  std::abs(mapping.offset) <= 1000000,
              "invalid vehicle unit conversion");
      } else if (transmission || drive_mode) {
        check(!field(node, "vehicleScale") && !field(node, "vehicleOffset"),
              "semantic input cannot use numeric conversion");
        check(type == ValueKind::integer || type == ValueKind::string,
              "semantic input needs an integer or string script signal");
        check(mapping.field != "transmission.gearbox_type" ||
                  binding.path != std::array<std::string, 3>{"driveState", "transmissionStatus",
                                                             "transmissionMode"},
              "gearbox type cannot select the transmission mode");
        semantic_mapping(mapping, field(node, "vehicleStates"), policy);
      } else {
        check(!field(node, "vehicleScale") && !field(node, "vehicleOffset"),
              "indicator input cannot use numeric conversion");
        check(type == ValueKind::integer || type == ValueKind::boolean,
              "indicator input needs an integer or boolean script signal");
        const auto states = field(node, "vehicleStates");
        check(states && plist_get_node_type(states) == PLIST_ARRAY &&
                  plist_array_get_size(states) == 3,
              "indicator needs explicit off/on/flashing values");
        for (unsigned i = 0; i < 3; ++i) {
          auto value = scalar(plist_array_get_item(states, i));
          if (type == ValueKind::boolean)
            check(std::holds_alternative<bool>(value.data), "indicator boolean state required");
          else {
            check(std::holds_alternative<std::int64_t>(value.data),
                  "indicator integer state required");
            const double n = double(std::get<std::int64_t>(value.data));
            check((!policy.minimum || n >= *policy.minimum) &&
                      (!policy.maximum || n <= *policy.maximum),
                  "indicator state outside profile bounds");
          }
          mapping.states.push_back(std::move(value));
        }
      }
      inputs.push_back(std::move(mapping));
    } else
      check(!field(node, "vehicleScale") && !field(node, "vehicleOffset") &&
                !field(node, "vehicleStates"),
            "vehicle conversion needs a source field");

    characteristics.push_back(Value{std::move(metadata)});
    policies.push_back(policy);
    bindings.push_back(std::move(binding));
  }
  Value service{Dictionary{{text("iid"), Value{std::int64_t(2)}},
                           {text("characteristics"), Value{std::move(characteristics)}}}};
  Value accessory{Dictionary{{text("iid"), Value{std::int64_t(1)}},
                             {text("services"), Value{Array{std::move(service)}}}}};
  auto source = std::make_shared<ReadOnlyService>(
      Value{Dictionary{{text("accessories"), Value{Array{std::move(accessory)}}}}},
      std::move(policies), std::move(fixed));
  ScriptView validate(
      source, bindings);  // Validate paths, IID mappings and uniqueness before any VM is started.
  // The signed updaters read sibling signals (same service and characteristic
  // group) from one batch; deliver each group together.
  std::map<std::tuple<std::string, std::string, std::string>, std::vector<std::int64_t>> groups;
  for (const auto& item : bindings) {
    groups[{item.path[0], item.path[1], item.instance.value_or("")}].push_back(item.iid);
  }
  std::vector<std::vector<std::int64_t>> update_groups;
  for (auto& [key, members] : groups) {
    (void)key;
    if (members.size() >= 2) update_groups.push_back(std::move(members));
  }
  source->set_update_groups(update_groups);
  LocalProfile profile{std::move(source), std::move(bindings), std::move(inputs)};
  check(scene_state.has_value() == scene_options.has_value(),
        "session scene inputs must be paired");
  if (scene_state) profile.scene_inputs = LocalProfile::SceneInputs{*scene_state, *scene_options};
  if (const auto identity = field(raw, "vehicleIdentity")) {
    keys(identity, {"make", "model", "year"});
    const auto make = string(field(identity, "make"));
    const auto model = string(field(identity, "model"));
    check(authored_identifier(make) && authored_identifier(model),
          "invalid local vehicle identity");
    const auto year = integer(field(identity, "year"), 9999);
    check(year >= 1886, "invalid local vehicle year");
    profile.identity = LocalProfile::Identity{make, model, static_cast<std::uint16_t>(year)};
  }
  validate_local_vehicle_inputs(profile);
  return profile;
}
}  // namespace headunit::theme::vehicle
