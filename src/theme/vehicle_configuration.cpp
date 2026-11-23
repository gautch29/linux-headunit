// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_configuration.hpp>
#include "vehicle_json.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <regex>
#include <tuple>

namespace headunit::theme::vehicle {
namespace {
using opack::Array;
using opack::Dictionary;
using opack::Value;
Value text(std::string_view s) {
  return Value{std::string(s)};
}
Value integer(std::int64_t n) {
  return Value{n};
}
Value object(Dictionary pairs) {
  return Value{std::move(pairs)};
}
[[noreturn]] void bad(std::string_view message) {
  throw std::invalid_argument(std::string(message));
}
const Dictionary& dictionary(const Value& v) {
  const auto* d = std::get_if<Dictionary>(&v.data);
  if (!d) bad("expected a vehicle configuration object");
  return *d;
}
const Array& array(const Value& v) {
  const auto* a = std::get_if<Array>(&v.data);
  if (!a) bad("expected an array");
  return *a;
}
const Value* field(const Value& v, std::string_view key) {
  for (const auto& [k, value] : dictionary(v))
    if (k == text(key)) return &value;
  return nullptr;
}
const Value& required(const Value& v, std::string_view key) {
  const auto* r = field(v, key);
  if (!r) bad("missing configuration field: " + std::string(key));
  return *r;
}
void keys(const Value& v, std::initializer_list<std::string_view> allowed) {
  for (const auto& [k, value] : dictionary(v)) {
    (void)value;
    const auto& name = std::get<std::string>(k.data);
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
      bad("unknown configuration field: " + name);
  }
}
std::string string(const Value& v) {
  const auto* s = std::get_if<std::string>(&v.data);
  if (!s || s->empty() || s->size() > 128 ||
      std::any_of(s->begin(), s->end(), [](unsigned char c) { return c < 32 || c == 127; }))
    bad("expected a nonempty display string of at most 128 UTF-8 bytes without control characters");
  return *s;
}
std::string name(const Value& v) {
  auto s = string(v);
  if (!std::regex_match(s, std::regex("[A-Za-z][A-Za-z0-9_.-]{0,95}")))
    bad("invalid stable identifier or binding");
  return s;
}
bool boolean(const Value& v) {
  const auto* b = std::get_if<bool>(&v.data);
  if (!b) bad("expected a boolean");
  return *b;
}
double number(const Value& v) {
  if (const auto* n = std::get_if<double>(&v.data)) return *n;
  if (const auto* n = std::get_if<std::int64_t>(&v.data)) return static_cast<double>(*n);
  bad("expected a finite number");
}
std::int64_t int_number(const Value& v) {
  const auto* n = std::get_if<std::int64_t>(&v.data);
  if (!n) bad("expected an integer");
  return *n;
}
bool flag(const Value& v, std::string_view key, bool fallback) {
  const auto* p = field(v, key);
  return p ? boolean(*p) : fallback;
}
std::string label(const Value& v, std::string_view key, std::string fallback) {
  const auto* p = field(v, key);
  return p ? string(*p) : std::move(fallback);
}
double numeric(const Value& v, std::string_view key, double fallback) {
  const auto* p = field(v, key);
  return p ? number(*p) : fallback;
}
void validate(const ConfiguredSignal& s, const Value& value) {
  std::optional<double> n;
  switch (s.policy.kind) {
    case ValueKind::boolean:
      (void)boolean(value);
      break;
    case ValueKind::integer:
      n = static_cast<double>(int_number(value));
      break;
    case ValueKind::real:
      if (!std::holds_alternative<double>(value.data))
        bad("provider must return a floating-point value");
      n = number(value);
      break;
    case ValueKind::string:
      (void)string(value);
      break;
    case ValueKind::array: {
      std::set<std::int64_t> seen;
      for (const auto& item : array(value)) {
        const auto index = int_number(item);
        if (index < 0 || static_cast<std::size_t>(index) >= s.disabled_entries.size() ||
            s.disabled_entries[index] || !seen.insert(index).second)
          bad("invalid or disabled selection index");
      }
      break;
    }
    default:
      bad("unsupported configured signal kind");
  }
  if (n) {
    if (!std::isfinite(*n) || (s.policy.minimum && *n < *s.policy.minimum) ||
        (s.policy.maximum && *n > *s.policy.maximum))
      bad("value outside configured range");
    const auto steps = (*n - s.policy.minimum.value_or(0)) / s.step;
    if (std::abs(steps - std::round(steps)) > 1e-6) bad("value does not match configured step");
    if (!s.disabled_entries.empty() && s.disabled_entries[static_cast<std::size_t>(*n)])
      bad("disabled selection entry");
  }
  if (opack::encode(value).size() > 1024) bad("configured value exceeds 1 KiB");
}
class Builder {
  Configuration result_;
  Array accessories_;
  std::int64_t next_ = 1000;
  std::set<std::string> write_keys_;
  bool experimental_ = false;
  std::int64_t id() { return next_++; }
  void experimental() {
    if (!experimental_)
      bad("this capability requires experimental: true; see VEHICLE_CAPABILITIES.md");
    result_.experimental = true;
  }
  Value service(std::int64_t type, Array chars) {
    return object({{text("iid"), integer(id())},
                   {text("type"), integer(type)},
                   {text("characteristics"), Value{std::move(chars)}},
                   {text("controls"), Value{Array{}}}});
  }
  void accessory(std::int64_t type, Array services) {
    if (services.empty()) return;
    accessories_.push_back(object({{text("iid"), integer(id())},
                                   {text("type"), integer(type)},
                                   {text("services"), Value{std::move(services)}}}));
  }
  Value metadata(std::int64_t iid, std::int64_t type, unsigned format, bool dynamic, bool writable,
                 std::optional<double> low = {}, std::optional<double> high = {}, double step = 1) {
    Dictionary d{{text("iid"), integer(iid)},         {text("type"), integer(type)},
                 {text("format"), integer(format)},   {text("mutable"), Value{dynamic}},
                 {text("writable"), Value{writable}}, {text("supportsInvalid"), Value{false}}};
    if (format >= 10) d.emplace_back(text("maximumLength"), integer(format == 12 ? 32 : 128));
    if (low) {
      d.emplace_back(text("minimumValue"), Value{*low});
      d.emplace_back(text("maximumValue"), Value{*high});
      d.emplace_back(text("stepValue"), Value{step});
    }
    return object(std::move(d));
  }
  Value constant(std::int64_t type, Value value, unsigned format = 10) {
    const auto iid = id();
    const auto kind = std::holds_alternative<std::string>(value.data) ? ValueKind::string
                      : std::holds_alternative<bool>(value.data)      ? ValueKind::boolean
                      : std::holds_alternative<Array>(value.data)     ? ValueKind::array
                                                                      : ValueKind::integer;
    if (kind == ValueKind::boolean) format = 0;
    result_.fixed_policies.push_back({iid, kind, 1000, {}, {}});
    result_.fixed.emplace(iid, std::move(value));
    return metadata(iid, type, format, false, false,
                    format > 0 && format < 10 ? std::optional<double>{0} : std::nullopt,
                    format > 0 && format < 10
                        ? std::optional<double>{format == 1 ? 255.0 : 4294967295.0}
                        : std::nullopt);
  }
  Value signal(std::int64_t type, unsigned format, ValueKind kind, const Value& config,
               std::string key, Value initial, std::optional<double> low = {},
               std::optional<double> high = {}, double step = 1, bool writable = true,
               std::vector<bool> disabled = {}) {
    if (const auto* p = field(config, "binding")) key = name(*p);
    if (const auto* p = field(config, "initial")) {
      initial = *p;
      if (kind == ValueKind::real) initial = Value{number(*p)};
    }
    if (kind == ValueKind::integer || kind == ValueKind::real) {
      low = numeric(config, "minimum", *low);
      high = numeric(config, "maximum", *high);
      step = numeric(config, "step", step);
      if (!std::isfinite(step) || step <= 0 || *low > *high ||
          (format == 1 && (*low < 0 || *high > 255)) ||
          (format == 3 && (*low < 0 || *high > 4294967295.0)) ||
          (format == 7 && (*low < -2147483648.0 || *high > 2147483647.0)))
        bad("invalid characteristic range");
      if (format == 9 && (std::abs(*low) > std::numeric_limits<float>::max() ||
                          std::abs(*high) > std::numeric_limits<float>::max()))
        bad("float range cannot be represented by the phone");
      if (kind == ValueKind::integer &&
          (std::floor(*low) != *low || std::floor(*high) != *high || std::floor(step) != step))
        bad("integer range and step must be integers");
    }
    writable = writable && flag(config, "writable", true);
    const auto age = field(config, "max_age_ms") ? int_number(*field(config, "max_age_ms")) : 1000;
    if (age < 100 || age > 60000) bad("max_age_ms must be 100..60000");
    const auto iid = id();
    ConfiguredSignal s{{iid, kind, static_cast<std::uint64_t>(age), low, high},
                       std::move(key),
                       std::move(initial),
                       writable,
                       step,
                       std::move(disabled)};
    validate(s, s.initial);
    if (writable && !write_keys_.insert(s.key).second)
      bad("multiple controls write the same provider binding");
    result_.signals.push_back(std::move(s));
    return metadata(iid, type, format, true, writable, low, high, step);
  }
  Value bool_signal(std::int64_t type, const Value& config, std::string key, bool initial = false) {
    keys(config, {"binding", "initial", "writable", "max_age_ms"});
    return signal(type, 0, ValueKind::boolean, config, std::move(key), Value{initial});
  }
  void optional_bool(Array& chars, const Value& config, std::string_view key, std::int64_t type,
                     const std::string& binding, bool initial = false) {
    if (const auto* p = field(config, key))
      chars.push_back(bool_signal(type, *p, binding + "." + std::string(key), initial));
  }
  void layout(Array& chars, std::string_view key) {
    chars.push_back(constant(0x36000065, text(key)));
  }
  void ranged(const Value& c) {
    keys(c, {"binding", "initial", "writable", "max_age_ms", "minimum", "maximum", "step", "on",
             "auto"});
  }
  void level_on(const ConfiguredSignal& level, const ConfiguredSignal& on) {
    const auto low = *level.policy.minimum, high = *level.policy.maximum;
    const auto initial = number(level.initial);
    const auto nonzero = initial != 0         ? initial
                         : high >= level.step ? level.step
                         : low <= -level.step ? -level.step
                                              : low;
    if (nonzero != 0)
      result_.level_switches.push_back(
          {level.key, on.key, integer(static_cast<std::int64_t>(nonzero)), low <= 0 && high >= 0});
  }
  void fan(Array& services, const Value& c, const std::string& prefix, const std::string& key,
           bool seat) {
    ranged(c);
    Array chars{signal(0x31000012, 1, ValueKind::integer, c, prefix + ".level",
                       integer(seat ? 0 : 40), 0, 100)};
    const auto level = result_.signals.back();
    if (field(c, "on")) {
      optional_bool(chars, c, "on", 0x30000002, prefix, !seat);
      level_on(level, result_.signals.back());
    }
    optional_bool(chars, c, "auto", 0x3000005f, prefix);
    layout(chars, key);
    services.push_back(service(seat ? 0x22000002 : 0x11000007, std::move(chars)));
  }
  void level(Array& services, const Value& c, const std::string& prefix, const std::string& key,
             std::int64_t service_type, bool on) {
    experimental();
    ranged(c);
    if (!on && field(c, "on")) bad("steering wheel service has no On characteristic");
    Array chars{signal(0x31000021, 7, ValueKind::integer, c, prefix + ".level", integer(0), 0, 3)};
    const auto level = result_.signals.back();
    if (on && field(c, "on")) {
      optional_bool(chars, c, "on", 0x30000002, prefix);
      level_on(level, result_.signals.back());
    }
    optional_bool(chars, c, "auto", 0x3000005f, prefix);
    if (on) layout(chars, key);
    services.push_back(service(service_type, std::move(chars)));
  }
  void boolean_service(Array& services, const Value& c, const std::string& prefix,
                       const std::string& key, std::int64_t type, bool auto_mode = false) {
    keys(c, {"on", "auto"});
    Array chars{bool_signal(0x30000002, required(c, "on"), prefix + ".on")};
    if (auto_mode)
      optional_bool(chars, c, "auto", 0x3000005f, prefix);
    else if (field(c, "auto"))
      bad("this service has no AutoMode characteristic");
    layout(chars, key);
    services.push_back(service(type, std::move(chars)));
  }
  void climate(const Value& root) {
    keys(root, {"version", "experimental", "cabin", "zones", "steering_wheel"});
    if (int_number(required(root, "version")) != 1)
      bad("unsupported climate configuration version");
    experimental_ = flag(root, "experimental", false);
    Array services;
    if (const auto* cabin = field(root, "cabin")) {
      keys(*cabin, {"power", "ac", "max_ac", "max_defrost"});
      Array chars;
      for (const auto& [key, type] :
           std::initializer_list<std::pair<std::string_view, std::int64_t>>{
               {"power", 0x31000002},
               {"ac", 0x31000003},
               {"max_ac", 0x31000022},
               {"max_defrost", 0x31000029}})
        if (const auto* p = field(*cabin, key))
          chars.push_back(bool_signal(type, *p, "climate.cabin." + std::string(key)));
      if (chars.empty()) bad("empty cabin capability");
      services.push_back(service(0x11000001, std::move(chars)));
    }
    std::set<std::string> zone_ids, layouts;
    if (const auto* zones = field(root, "zones")) {
      if (array(*zones).size() > 8) bad("at most eight climate zones");
      for (const auto& zone : array(*zones)) {
        keys(zone,
             {"id", "layout", "temperature", "fan", "recirculation", "power", "sync", "defrost",
              "vents", "automatic", "temperature_level", "seat_heating_cooling", "seat_fan"});
        const auto zone_id = name(required(zone, "id")), key = name(required(zone, "layout"));
        if (!zone_ids.insert(zone_id).second || !layouts.insert(key).second)
          bad("duplicate zone ID or layout key");
        const auto prefix = "climate." + zone_id;
        const auto before = services.size();
        Array seat_services;
        if (const auto* p = field(zone, "temperature")) {
          keys(*p, {"binding", "initial", "writable", "max_age_ms", "minimum", "maximum", "step",
                    "units", "on", "current"});
          const auto units = label(*p, "units", "celsius");
          const auto unit = units == "celsius"      ? 1082
                            : units == "fahrenheit" ? 1081
                            : units == "kelvin"     ? 1083
                                                    : 0;
          if (!unit) bad("temperature units must be celsius, fahrenheit or kelvin");
          const double low = units == "celsius" ? 16 : units == "fahrenheit" ? 60 : 289.15;
          const double high = units == "celsius" ? 30 : units == "fahrenheit" ? 86 : 303.15;
          auto target = signal(0x31000017, 9, ValueKind::real, *p, prefix + ".temperature.target",
                               Value{units == "celsius"      ? 21.0
                                     : units == "fahrenheit" ? 70.0
                                                             : 294.15},
                               low, high, units == "fahrenheit" ? 1 : 0.5);
          const auto target_signal = result_.signals.back();
          const auto current = field(*p, "current");
          const Value empty = object({});
          if (current)
            keys(*current, {"binding", "initial", "max_age_ms", "minimum", "maximum", "step"});
          auto measured =
              signal(0x31000019, 9, ValueKind::real, current ? *current : empty, target_signal.key,
                     target_signal.initial, target_signal.policy.minimum,
                     target_signal.policy.maximum, target_signal.step, false);
          std::get<Dictionary>(target.data).emplace_back(text("units"), integer(unit));
          std::get<Dictionary>(measured.data).emplace_back(text("units"), integer(unit));
          Array chars{std::move(measured), std::move(target)};
          optional_bool(chars, *p, "on", 0x30000002, prefix + ".temperature", true);
          layout(chars, key);
          services.push_back(service(0x11000002, std::move(chars)));
        }
        if (const auto* p = field(zone, "fan")) fan(services, *p, prefix + ".fan", key, false);
        if (const auto* p = field(zone, "seat_fan"))
          fan(seat_services, *p, prefix + ".seat_fan", key, true);
        if (const auto* p = field(zone, "seat_heating_cooling"))
          level(seat_services, *p, prefix + ".seat_heating_cooling", key, 0x22000001, true);
        if (const auto* p = field(zone, "temperature_level"))
          level(services, *p, prefix + ".temperature_level", key, 0x11000015, true);
        for (const auto& [k, type] :
             std::initializer_list<std::pair<std::string_view, std::int64_t>>{
                 {"recirculation", 0x11000011}, {"power", 0x11000014}, {"sync", 0x11000012}})
          if (const auto* p = field(zone, k))
            boolean_service(services, *p, prefix + "." + std::string(k), key, type,
                            k == "recirculation");
        if (const auto* p = field(zone, "automatic")) {
          keys(*p, {"binding", "initial", "minimum", "maximum", "step", "writable", "max_age_ms"});
          Array chars{signal(0x31000024, 1, ValueKind::integer, *p, prefix + ".automatic.level",
                             integer(0), 0, 3)};
          layout(chars, key);
          services.push_back(service(0x11000008, std::move(chars)));
        }
        if (const auto* p = field(zone, "defrost")) {
          keys(*p, {"types", "binding", "initial", "minimum", "maximum", "step", "writable",
                    "max_age_ms"});
          std::int64_t mask = 0;
          for (const auto& t : array(required(*p, "types"))) {
            const auto s = string(t);
            const auto bit = s == "air" ? 1 : s == "filament" ? 2 : 0;
            if (!bit || (mask & bit)) bad("invalid or duplicate defrost type");
            mask |= bit;
          }
          if (!mask) bad("defrost needs at least one type");
          experimental();
          Array chars{constant(0x31000014, integer(mask), 4),
                      signal(0x30000004, 1, ValueKind::integer, *p, prefix + ".defrost.level",
                             integer(0), 0, 1)};
          layout(chars, key);
          services.push_back(service(0x11000005, std::move(chars)));
        }
        if (const auto* p = field(zone, "vents")) {
          experimental();
          keys(*p, {"combinations", "binding", "initial", "writable", "max_age_ms", "on", "auto"});
          Array combinations;
          std::set<std::int64_t> masks;
          for (const auto& combination : array(required(*p, "combinations"))) {
            std::int64_t mask = 0;
            for (const auto& t : array(combination)) {
              const auto s = string(t);
              const auto bit = s == "window"   ? 1
                               : s == "upper"  ? 2
                               : s == "lower"  ? 4
                               : s == "neck"   ? 8
                               : s == "middle" ? 16
                                               : 0;
              if (!bit || (mask & bit)) bad("invalid or duplicate vent direction");
              mask |= bit;
            }
            if (!mask || !masks.insert(mask).second) bad("empty or duplicate vent combination");
            combinations.push_back(integer(mask));
          }
          if (combinations.empty() || combinations.size() > 16)
            bad("vents require 1..16 combinations");
          Array chars{constant(0x31000025, Value{combinations}, 12),
                      signal(0x30000038, 3, ValueKind::integer, *p, prefix + ".vents.index",
                             integer(0), 0, combinations.size() - 1)};
          optional_bool(chars, *p, "on", 0x30000002, prefix + ".vents", true);
          optional_bool(chars, *p, "auto", 0x3000005f, prefix + ".vents");
          layout(chars, key);
          services.push_back(service(0x11000006, std::move(chars)));
        }
        if (before == services.size() && seat_services.empty()) bad("zone declares no controls");
        accessory(0x01500001, std::move(seat_services));
      }
    }
    if (const auto* wheel = field(root, "steering_wheel"))
      level(services, *wheel, "climate.steering_wheel", "steering_wheel", 0x11000003, false);
    accessory(0x01000001, std::move(services));
  }
  void settings(const Value& root) {
    keys(root, {"version", "experimental", "sections", "settings", "cluster_content",
                "vehicle_resources", "media"});
    if (int_number(required(root, "version")) != 1)
      bad("unsupported settings configuration version");
    experimental_ = flag(root, "experimental", false);
    Array services;
    std::set<std::string> section_ids, setting_ids;
    const auto& sections = array(required(root, "sections"));
    if (sections.size() > 16) bad("at most sixteen settings sections");
    for (const auto& s : sections) {
      keys(s, {"id", "label", "footer", "order"});
      const auto identifier = name(required(s, "id"));
      if (!section_ids.insert(identifier).second) bad("duplicate settings section");
      const auto order = field(s, "order") ? int_number(*field(s, "order"))
                                           : static_cast<std::int64_t>(section_ids.size() - 1);
      if (order < 0 || order > 255) bad("section order must be 0..255");
      Array chars{constant(0x30000019, text(identifier)),
                  constant(0x36000004, text(string(required(s, "label")))),
                  constant(0x30000003, integer(order), 1)};
      if (const auto* p = field(s, "footer"))
        chars.push_back(constant(0x30000016, text(string(*p))));
      services.push_back(service(0x16000020, std::move(chars)));
    }
    const auto& settings = array(required(root, "settings"));
    if (settings.size() > 32) bad("at most 32 car settings");
    for (const auto& s : settings) {
      keys(s,
           {"id",         "section", "label",    "kind",           "order",          "description",
            "symbol",     "hidden",  "disabled", "minimum_symbol", "maximum_symbol", "hide_stepper",
            "binding",    "initial", "minimum",  "maximum",        "step",           "writable",
            "max_age_ms", "choices"});
      const auto identifier = name(required(s, "id")), section = name(required(s, "section")),
                 kind = string(required(s, "kind"));
      if (!setting_ids.insert(identifier).second || section_ids.contains(identifier))
        bad("duplicate setting identifier");
      if (!section_ids.contains(section)) bad("setting refers to an absent section");
      const auto order = field(s, "order") ? int_number(*field(s, "order"))
                                           : static_cast<std::int64_t>(setting_ids.size() - 1);
      if (order < 0 || order > 255) bad("setting order must be 0..255");
      Array chars{constant(0x30000019, text(identifier)),
                  constant(0x30000001, text(string(required(s, "label")))),
                  constant(0x36000028, text(section)), constant(0x30000003, integer(order), 1)};
      for (const auto& [key, type] :
           std::initializer_list<std::pair<std::string_view, std::int64_t>>{
               {"description", 0x30000005},
               {"symbol", 0x3000005e},
               {"minimum_symbol", 0x36000025},
               {"maximum_symbol", 0x36000026}})
        if (const auto* p = field(s, key)) chars.push_back(constant(type, text(string(*p))));
      for (const auto& [key, type] :
           std::initializer_list<std::pair<std::string_view, std::int64_t>>{
               {"hidden", 0x36000023}, {"disabled", 0x32000011}, {"hide_stepper", 0x36000027}})
        if (const auto* p = field(s, key)) chars.push_back(constant(type, Value{boolean(*p)}, 0));
      if (kind != "integer" && field(s, "hide_stepper"))
        bad("hide_stepper applies only to integer settings");
      if (kind != "integer" && kind != "float" &&
          (field(s, "minimum_symbol") || field(s, "maximum_symbol")))
        bad("range symbols apply only to numeric settings");
      const auto key = "settings." + identifier;
      const bool writable = !flag(s, "disabled", false);
      std::int64_t type = 0;
      if (kind == "boolean") {
        if (field(s, "minimum") || field(s, "maximum") || field(s, "step"))
          bad("boolean settings have no numeric range");
        type = 0x16000001;
        chars.push_back(
            signal(0x30000002, 0, ValueKind::boolean, s, key, Value{false}, {}, {}, 1, writable));
      } else if (kind == "float" || kind == "integer") {
        experimental();
        (void)required(s, "minimum");
        (void)required(s, "maximum");
        type = kind == "float" ? 0x16000003 : 0x16000007;
        const auto low = number(required(s, "minimum"));
        if (kind == "integer") {
          (void)int_number(required(s, "minimum"));
          (void)int_number(required(s, "maximum"));
          if (low < -2147483648.0 || low > 2147483647.0)
            bad("integer setting minimum must fit int32");
        }
        chars.push_back(
            signal(kind == "float" ? 0x30000006 : 0x30000010, kind == "float" ? 9 : 7,
                   kind == "float" ? ValueKind::real : ValueKind::integer, s, key,
                   kind == "float" ? Value{low} : integer(static_cast<std::int64_t>(low)), low,
                   number(required(s, "maximum"))));
      } else if (kind == "select" || kind == "multi_select") {
        if (field(s, "minimum") || field(s, "maximum") || field(s, "step"))
          bad("selection ranges come from choices");
        experimental();
        const auto& choices = array(required(s, "choices"));
        if (choices.empty() || choices.size() > 16) bad("settings require 1..16 choices");
        Array entries;
        std::vector<bool> disabled;
        for (const auto& choice : choices) {
          keys(choice, {"label", "description", "symbol", "disabled"});
          const auto off = flag(choice, "disabled", false);
          disabled.push_back(off);
          Dictionary entry{{text("disabled"), Value{off}},
                           {text("userVisibleLabel"), text(string(required(choice, "label")))}};
          if (const auto* p = field(choice, "description"))
            entry.emplace_back(text("userVisibleDescription"), text(string(*p)));
          if (const auto* p = field(choice, "symbol"))
            entry.emplace_back(text("symbolName"), text(string(*p)));
          entries.push_back(object(std::move(entry)));
        }
        chars.push_back(constant(0x36000024, Value{std::move(entries)}, 12));
        type = kind == "select" ? 0x16000004 : 0x16000005;
        chars.push_back(
            signal(kind == "select" ? 0x30000061 : 0x36000062, kind == "select" ? 1 : 12,
                   kind == "select" ? ValueKind::integer : ValueKind::array, s, key,
                   kind == "select" ? integer(0) : Value{Array{}},
                   kind == "select" ? std::optional<double>{0} : std::nullopt,
                   kind == "select" ? std::optional<double>{static_cast<double>(choices.size() - 1)}
                                    : std::nullopt,
                   1, writable, std::move(disabled)));
      } else if (kind == "text") {
        experimental();
        type = 0x16000015;
        if (flag(s, "writable", false)) bad("text settings are read-only");
        chars.push_back(signal(0x30000017, 10, ValueKind::string, s, key, text("Unavailable"), {},
                               {}, 1, false));
      } else
        bad("unsupported car-setting kind: " + kind);
      if (kind != "select" && kind != "multi_select" && field(s, "choices"))
        bad("choices apply only to select settings");
      if (kind != "integer" && kind != "float" && kind != "boolean" && kind != "select" &&
          (field(s, "minimum") || field(s, "maximum") || field(s, "step")))
        bad("this setting has no numeric range");
      services.push_back(service(type, std::move(chars)));
    }
    accessory(0x05000001, std::move(services));
    if (const auto* content = field(root, "cluster_content")) cluster_content(*content);
    if (const auto* resources = field(root, "vehicle_resources")) vehicle_resources(*resources);
    if (const auto* media = field(root, "media")) media_presence(*media);
  }
  void media_presence(const Value& root) {
    experimental();
    keys(root, {"source_id", "source_label"});
    // The phone's Media app hosts the Ultra cluster Now Playing page only when
    // the car declares both Media 0x03000001 and NowPlayingInformation
    // 0x0F000001. Declare one fixed source representing the connected phone and
    // name it as current. Phone titles/artwork are never mirrored into the car's
    // NowPlaying; with mainAudio on the phone, iOS renders its own metadata.
    const auto identifier = name(required(root, "source_id"));
    const auto label = string(required(root, "source_label"));
    if (label.empty() || label.size() > 64) bad("media source label must be 1..64 bytes");
    Array source{constant(0x30000019, text(identifier)), constant(0x30000001, text(label))};
    accessory(0x03000001, Array{service(0x14000006, std::move(source))});
    Array now_playing{constant(0x32000023, text(identifier))};
    accessory(0x0F000001, Array{service(0x14000001, std::move(now_playing))});
  }
  void vehicle_resources(const Value& root) {
    experimental();
    keys(root, {"exterior_style", "interior_style", "vehicle_layout"});
    // These select resources already present in the phone-side asset package.
    // They are immutable metadata, not sensor readings or an image archive.
    // Explicitly require all three: the phone's variant agent reads both style
    // traits before resolving the separately selected vehicle layout.
    Array chars{constant(0x48000003, text(name(required(root, "exterior_style")))),
                constant(0x48000004, text(name(required(root, "interior_style")))),
                constant(0x48000005, text(name(required(root, "vehicle_layout"))))};
    accessory(0x01200002, Array{service(0x20000001, std::move(chars))});
  }
  // Metadata is immutable for the connection. Dynamic readings are read-only
  // and support explicit invalidation; omission/null never becomes a zero.
  Value content_reading(std::int64_t type, unsigned format, ValueKind kind, const Value& config,
                        std::string key, double low = 0, double high = 255, double step = 1) {
    keys(config, {"binding", "initial", "max_age_ms", "units", "minimum", "maximum", "step"});
    if (const auto* binding = field(config, "binding")) key = name(*binding);
    const auto age =
        field(config, "max_age_ms") ? int_number(required(config, "max_age_ms")) : 1000;
    if (age < 100 || age > 60000) bad("content reading freshness must be 100..60000 ms");
    const bool is_numeric = kind == ValueKind::integer || kind == ValueKind::real;
    if (!is_numeric && (field(config, "minimum") || field(config, "maximum") ||
                        field(config, "step") || field(config, "units")))
      bad("non-numeric content readings cannot declare units or ranges");
    if (is_numeric) {
      const auto configured_low = numeric(config, "minimum", low);
      const auto configured_high = numeric(config, "maximum", high);
      step = numeric(config, "step", step);
      if (configured_low < low || configured_high > high || configured_low > configured_high ||
          !std::isfinite(step) || step <= 0 ||
          (kind == ValueKind::integer &&
           (std::floor(configured_low) != configured_low ||
            std::floor(configured_high) != configured_high || std::floor(step) != step)))
        bad("content range exceeds the characteristic domain");
      low = configured_low;
      high = configured_high;
    }
    const auto iid = id();
    Value initial;
    if (const auto* v = field(config, "initial")) {
      initial = *v;
      if (kind == ValueKind::real && !std::holds_alternative<std::monostate>(v->data))
        initial = Value{number(*v)};
    }
    ConfiguredSignal signal{{iid, kind, static_cast<std::uint64_t>(age),
                             is_numeric ? std::optional<double>{low} : std::nullopt,
                             is_numeric ? std::optional<double>{high} : std::nullopt},
                            std::move(key),
                            std::move(initial),
                            false,
                            step,
                            {}};
    if (!std::holds_alternative<std::monostate>(signal.initial.data))
      validate(signal, signal.initial);
    result_.signals.push_back(std::move(signal));
    auto meta = metadata(iid, type, format, true, false,
                         is_numeric ? std::optional<double>{low} : std::nullopt,
                         is_numeric ? std::optional<double>{high} : std::nullopt, step);
    auto& entries = std::get<Dictionary>(meta.data);
    for (auto& [k, v] : entries)
      if (k == text("supportsInvalid")) v = Value{true};
    if (const auto* units = field(config, "units")) {
      const auto n = int_number(*units);
      if (kind != ValueKind::real || n <= 0 || n > 65535)
        bad("measurement requires an explicit positive unit identifier");
      entries.emplace_back(text("units"), integer(n));
    } else if (kind == ValueKind::real)
      bad("content measurement requires explicit units");
    return meta;
  }
  void content_fields(
      Array& chars, const Value& entry, const std::string& prefix,
      std::initializer_list<std::tuple<std::string_view, std::int64_t, double>> fields) {
    for (const auto& [key, type, maximum] : fields)
      if (const auto* value = field(entry, key))
        chars.push_back(content_reading(type, 1, ValueKind::integer, *value,
                                        prefix + "." + std::string(key), 0, maximum));
  }
  void content_measurements(
      Array& chars, const Value& entry, const std::string& prefix,
      std::initializer_list<std::pair<std::string_view, std::int64_t>> fields) {
    for (const auto& [key, type] : fields)
      if (const auto* value = field(entry, key))
        chars.push_back(content_reading(type, 9, ValueKind::real, *value,
                                        prefix + "." + std::string(key), 0, 4294967295.0, .001));
  }
  void cluster_content(const Value& root) {
    experimental();
    keys(root, {"pages", "request_file", "trips", "tyres", "seats", "closures", "charging"});
    std::set<std::string> identifiers;
    const auto repeated = [&](std::string_view key, unsigned maximum, auto visit) {
      if (const auto* values = field(root, key)) {
        const auto& entries = array(*values);
        if (entries.empty() || entries.size() > maximum)
          bad("content instance count exceeds its bound");
        std::set<std::string> locations;
        for (const auto& entry : entries) {
          const auto identifier = name(required(entry, "id"));
          if (!identifiers.insert(identifier).second) bad("duplicate content instance identifier");
          if (const auto* place = field(entry, "layout"))
            if (!locations.insert(name(*place)).second) bad("duplicate fitted content location");
          visit(entry, identifier);
        }
      }
    };
    Array pages;
    repeated("pages", 16, [&](const Value& page, const std::string& identifier) {
      keys(page, {"id", "widget", "panel", "zone", "dismissible"});
      const auto widget = name(required(page, "widget"));
      const std::string url = widget == "body"    ? "widget://?identifier=com.apple.carBodyStatus"
                              : widget == "tyres" ? "widget://?identifier=com.apple.tires"
                              : widget == "charging" ? "widget://?identifier=com.apple.charging"
                                                     : std::string{};
      if (url.empty()) bad("content widget has no established URL contract");
      if (field(root, "request_file") && !flag(page, "dismissible", true))
        bad("requested vehicle content must remain user-dismissible");
      const auto panel = name(required(page, "panel")), zone = name(required(page, "zone"));
      if (panel != "Cluster_Display" || (zone != "zone1" && zone != "zone2"))
        bad("content page must use an explicit reference-pack cluster zone");
      if (std::any_of(
              result_.content_pages.begin(), result_.content_pages.end(),
              [&](const auto& p) { return p.url == url && p.panel == panel && p.zone == zone; }))
        bad("duplicate content page in a display zone");
      pages.push_back(service(
          0x1700000B, Array{constant(0x37000013, text(url)), constant(0x37000007, text(panel)),
                            constant(0x3700000B, text(zone)),
                            constant(0x36000063, Value{flag(page, "dismissible", true)})}));
      result_.content_pages.push_back({identifier, url, panel, zone});
    });
    if (const auto* path = field(root, "request_file")) {
      const auto file = string(*path);
      const std::filesystem::path endpoint(file);
      if (!endpoint.is_absolute() || endpoint.lexically_normal() != endpoint ||
          endpoint.filename().empty() || result_.content_pages.empty())
        bad("content request needs an absolute endpoint and declared pages");
      // One bounded request surface, one visible page. The phone may dismiss,
      // but cannot turn a page on by writing into this receiver-owned intent.
      const auto& first = result_.content_pages.front();
      for (const auto& page : result_.content_pages)
        if (page.panel != first.panel || page.zone != first.zone)
          bad("content request pages must share one explicit display zone");
      const std::string on_key = "content.request.on", url_key = "content.request.url";
      const auto on_config =
          object({{text("initial"), Value{false}}, {text("max_age_ms"), integer(3000)}});
      Array chars{constant(0x37000007, text(first.panel)), constant(0x3700000B, text(first.zone)),
                  constant(0x36000063, Value{true}),
                  signal(0x30000002, 0, ValueKind::boolean, on_config, on_key, Value{false})};
      result_.signals.back().dismiss_only = true;
      const auto url_config = object({{text("max_age_ms"), integer(3000)}});
      chars.push_back(content_reading(0x30000007, 10, ValueKind::string, url_config, url_key));
      pages.push_back(service(0x17000004, std::move(chars)));
      result_.content_request =
          ContentRequestCapability{file, on_key, url_key, result_.content_pages};
    }
    accessory(0x01200001, std::move(pages));
    Array trips;
    repeated("trips", 8, [&](const Value& entry, const std::string& identifier) {
      keys(entry, {"id", "label", "order", "show_odometer", "average_speed", "distance", "duration",
                   "energy", "energy_efficiency", "fuel_efficiency"});
      const auto order = field(entry, "order") ? int_number(required(entry, "order")) : 0;
      if (order < 0 || order > 255) bad("trip order must fit UInt8");
      Array chars{constant(0x30000001, text(string(required(entry, "label")))),
                  constant(0x30000003, integer(order), 1),
                  constant(0x35000016, Value{flag(entry, "show_odometer", false)})};
      content_measurements(chars, entry, "content.trip." + identifier,
                           {{"average_speed", 0x30000045},
                            {"distance", 0x30000022},
                            {"duration", 0x30000023},
                            {"energy", 0x30000025},
                            {"energy_efficiency", 0x35000012},
                            {"fuel_efficiency", 0x35000013}});
      if (chars.size() == 3) bad("trip declares no readings");
      trips.push_back(service(0x15000002, std::move(chars)));
    });
    accessory(0x04000001, std::move(trips));
    Array tyres;
    repeated("tyres", 8, [&](const Value& entry, const std::string& identifier) {
      keys(entry, {"id", "layout", "pressure", "pressure_state", "sensor_state"});
      Array chars;
      layout(chars, name(required(entry, "layout")));
      content_measurements(chars, entry, "content.tyre." + identifier, {{"pressure", 0x3000001B}});
      content_fields(chars, entry, "content.tyre." + identifier,
                     {{"pressure_state", 0x3000001C, 5}, {"sensor_state", 0x3000001A, 4}});
      if (chars.size() == 1) bad("tyre declares no readings");
      tyres.push_back(service(0x1B000001, std::move(chars)));
    });
    accessory(0x0A000001, std::move(tyres));
    Array seats;
    repeated("seats", 16, [&](const Value& entry, const std::string& identifier) {
      keys(entry, {"id", "layout", "occupancy", "buckle", "indicator"});
      Array chars;
      layout(chars, name(required(entry, "layout")));
      content_fields(
          chars, entry, "content.seat." + identifier,
          {{"occupancy", 0x60000006, 3}, {"buckle", 0x60000002, 2}, {"indicator", 0x60000007, 2}});
      if (chars.size() == 1) bad("seat declares no readings");
      seats.push_back(service(0x22000004, std::move(chars)));
    });
    accessory(0x01500001, std::move(seats));
    Array closures;
    repeated("closures", 16, [&](const Value& entry, const std::string& identifier) {
      keys(entry, {"id", "layout", "latch"});
      Array chars{content_reading(0x4200000B, 0, ValueKind::boolean, required(entry, "latch"),
                                  "content.closure." + identifier + ".latch")};
      layout(chars, name(required(entry, "layout")));
      closures.push_back(service(0x1D000001, std::move(chars)));
    });
    accessory(0x0D000001, std::move(closures));
    Array charging;
    repeated("charging", 4, [&](const Value& entry, const std::string& identifier) {
      keys(entry, {"id", "state", "cable", "mode_identifier"});
      Array chars;
      content_fields(chars, entry, "content.charging." + identifier,
                     {{"state", 0x40000005, 11}, {"cable", 0x40000007, 4}});
      if (const auto* mode = field(entry, "mode_identifier"))
        chars.push_back(content_reading(0x4000000C, 10, ValueKind::string, *mode,
                                        "content.charging." + identifier + ".mode_identifier"));
      if (chars.empty()) bad("charging declares no readings");
      charging.push_back(service(0x19000002, std::move(chars)));
    });
    accessory(0x09000008, std::move(charging));
    if (identifiers.empty()) bad("cluster content is empty");
  }
  void camera(const Value& root) {
    keys(root, {"version", "experimental", "stream_identifier", "general"});
    if (int_number(required(root, "version")) != 1) bad("unsupported camera configuration version");
    if (!boolean(required(root, "experimental")))
      bad("camera capability requires experimental: true");
    experimental_ = true;
    experimental();
    const auto uid = name(required(root, "stream_identifier"));
    if (uid.size() > 64) bad("camera stream identifier exceeds 64 bytes");
    const auto& general = required(root, "general");
    keys(general, {"on"});
    const auto& on = required(general, "on");
    keys(on, {"binding", "max_age_ms", "writable", "initial"});
    const auto binding = name(required(on, "binding"));
    const auto age = int_number(required(on, "max_age_ms"));
    if (age < 100 || age > 1000) bad("camera max_age_ms must be 100..1000");
    if (boolean(required(on, "writable")) || boolean(required(on, "initial")))
      bad("camera On must start false and remain read-only until activation is confirmed");
    if (std::any_of(result_.signals.begin(), result_.signals.end(),
                    [&](const auto& s) { return s.key == binding; }))
      bad("camera binding aliases an unrelated vehicle control");
    Array input_chars{constant(0x44000000, text(uid))};
    input_chars.push_back(
        signal(0x30000002, 0, ValueKind::boolean, on, binding, Value{false}, {}, {}, 1, false));
    const auto input_on_iid = result_.signals.back().policy.iid;
    accessory(0x0C000001, Array{service(0x1C000001, std::move(input_chars))});
    Array general_chars{
        signal(0x30000002, 0, ValueKind::boolean, on, binding, Value{false}, {}, {}, 1, false)};
    const auto general_on_iid = result_.signals.back().policy.iid;
    // CAFAccessoryTypes.mm declares 0x0000000001300001. Do not drop the
    // leading zero nibble: 0x13000001 is a different accessory type.
    accessory(0x01300001, Array{service(0x1F000001, std::move(general_chars))});
    result_.camera = CameraCapability{uid, binding, static_cast<std::uint64_t>(age), input_on_iid,
                                      general_on_iid};
  }

 public:
  Configuration build(std::string_view climate_json, std::string_view settings_json,
                      std::string_view camera_json) {
    if (!climate_json.empty()) climate(detail::Json(climate_json).parse());
    if (!settings_json.empty()) settings(detail::Json(settings_json).parse());
    if (!camera_json.empty()) camera(detail::Json(camera_json).parse());
    result_.profile = object({{text("accessories"), Value{std::move(accessories_)}}});
    // Alias reads are allowed (current/target); conflicting initial state isn't.
    std::map<std::string, ConfiguredSignal> initial;
    for (const auto& s : result_.signals) {
      const auto [it, inserted] = initial.emplace(s.key, s);
      if (!inserted &&
          (it->second.initial != s.initial || it->second.policy.kind != s.policy.kind ||
           it->second.policy.minimum != s.policy.minimum ||
           it->second.policy.maximum != s.policy.maximum || it->second.step != s.step ||
           it->second.disabled_entries != s.disabled_entries ||
           it->second.dismiss_only != s.dismiss_only))
        bad("conflicting values or policies for provider binding: " + s.key);
    }
    return std::move(result_);
  }
};
std::string read_file(const std::optional<std::filesystem::path>& path) {
  if (!path) return {};
  std::ifstream in(*path, std::ios::binary);
  if (!in) bad("cannot open vehicle capability file: " + path->string());
  std::string contents;
  char block[4096];
  while (in) {
    in.read(block, sizeof(block));
    contents.append(block, static_cast<std::size_t>(in.gcount()));
    if (contents.size() > 65536) bad("vehicle capability file exceeds 64 KiB");
  }
  if (!in.eof() || contents.empty()) bad("unreadable or empty vehicle capability file");
  return contents;
}
class Simulation final : public Provider {
  std::map<std::string, Value> state_;
  std::vector<LevelSwitch> switches_;

 public:
  explicit Simulation(const Configuration& c) : switches_(c.level_switches) {
    for (const auto& s : c.signals)
      state_.emplace(s.key, s.initial);
  }
  std::optional<Sample> read(std::string_view key, std::uint64_t now) override {
    const auto it = state_.find(std::string(key));
    return it == state_.end() || std::holds_alternative<std::monostate>(it->second.data)
               ? std::nullopt
               : std::optional<Sample>{{it->second, now}};
  }
  Value apply(std::string_view key, const Value& requested, std::uint64_t) override {
    const auto it = state_.find(std::string(key));
    if (it == state_.end()) bad("simulation binding absent");
    it->second = requested;
    for (auto& pair : switches_) {
      if (key == pair.level) {
        const auto active = number(requested) != 0;
        state_.at(pair.on) = Value{active};
        if (active) pair.nonzero = requested;
      } else if (key == pair.on) {
        if (boolean(requested)) {
          if (number(state_.at(pair.level)) == 0) state_.at(pair.level) = pair.nonzero;
        } else if (pair.has_zero) {
          if (number(state_.at(pair.level)) != 0) pair.nonzero = state_.at(pair.level);
          state_.at(pair.level) = integer(0);
        }
      }
    }
    return requested;
  }
};
}  // namespace
Configuration parse_configuration(std::string_view climate, std::string_view settings,
                                  std::string_view camera) {
  return Builder{}.build(climate, settings, camera);
}
Configuration load_configuration(const std::optional<std::filesystem::path>& climate,
                                 const std::optional<std::filesystem::path>& settings,
                                 const std::optional<std::filesystem::path>& camera) {
  return parse_configuration(read_file(climate), read_file(settings), read_file(camera));
}
std::shared_ptr<Provider> simulated_provider(const Configuration& c) {
  return std::make_shared<Simulation>(c);
}
ConfiguredModel::ConfiguredModel(Configuration c, std::shared_ptr<Provider> provider)
    : configuration_(std::move(c)), provider_(std::move(provider)) {
  if (!provider_) bad("configured vehicle model requires an explicit provider");
  auto policies = configuration_.fixed_policies;
  std::vector<WriteBinding> writers;
  std::vector<ConfirmationBinding> confirmations;
  for (const auto& s : configuration_.signals) {
    policies.push_back(s.policy);
    if (s.writable && provider_->supports_confirmation(s.key)) {
      confirmations.push_back(
          {s.policy.iid, [this, s](const Value& value, std::uint64_t now, std::uint64_t deadline) {
             if (s.dismiss_only && value != Value{false})
               bad("phone may only dismiss receiver-requested content");
             validate(s, value);
             auto pending = provider_->start_confirmation(s.key, value, now, deadline);
             if (pending && pending->poll) {
               auto poll = std::move(pending->poll);
               pending->poll = [s, poll = std::move(poll)](std::uint64_t tick) {
                 auto result = poll(tick);
                 if (result.status == WriteCompletion::Status::applied && result.value)
                   validate(s, *result.value);
                 return result;
               };
             }
             return pending;
           }});
    } else if (s.writable)
      writers.push_back({s.policy.iid, [this, s](const Value& value) {
                           if (s.dismiss_only && value != Value{false})
                             bad("phone may only dismiss receiver-requested content");
                           validate(s, value);
                           try {
                             auto applied = provider_->apply(s.key, value, now_);
                             validate(s, applied);
                             observed_[s.policy.iid] = Sample{applied, now_};
                             return applied;
                           } catch (const std::exception&) {
                             bad("vehicle provider rejected or could not confirm the write");
                           }
                         }});
  }
  service_ =
      std::make_unique<Service>(configuration_.profile, std::move(policies), configuration_.fixed,
                                std::move(writers), std::move(confirmations));
}
void ConfiguredModel::refresh(std::uint64_t now) {
  if (refreshed_ && now < *refreshed_) bad("vehicle model clock moved backwards");
  now_ = now;
  const bool regular_due = !refreshed_ || now - *refreshed_ >= 100;
  // A binding names one provider reading. Read it once per refresh, including
  // absence/failure, so aliases cannot observe different values or timestamps
  // just because the phone model exposes several characteristics for it.
  std::map<std::string, std::optional<Sample>> readings;
  for (const auto& s : configuration_.signals) {
    const bool camera = configuration_.camera && s.key == configuration_.camera->binding;
    const bool content =
        configuration_.content_request && (s.key == configuration_.content_request->on_binding ||
                                           s.key == configuration_.content_request->url_binding);
    const bool content_on = content && s.key == configuration_.content_request->on_binding;
    const bool visibility = camera || content_on;
    // Visibility and dismissal decisions must not wait for the ordinary sensor
    // poll. The provider itself bounds file reads; these copied state reads do
    // not refresh a leased intent or execute VM work.
    if (!regular_due && !camera && !content) continue;
    const auto unavailable = [&] {
      if (visibility) {
        service_->deactivate_boolean(s.policy.iid, now);
      } else
        service_->withdraw(s.policy.iid, now);
    };
    const auto previous = observed_.find(s.policy.iid);
    try {
      auto [reading, first] = readings.try_emplace(s.key);
      if (first) {
        try {
          reading->second = provider_->read(s.key, now);
        } catch (const std::exception&) {
          reading->second.reset();
        }
      }
      const auto& sample = reading->second;
      if (!sample || sample->sampled_ms > now || now - sample->sampled_ms >= s.policy.max_age_ms ||
          ((!visibility || sample->value == Value{true}) && previous != observed_.end() &&
           sample->sampled_ms < previous->second.sampled_ms)) {
        unavailable();
        continue;
      }
      validate(s, sample->value);
      if (visibility && sample->value == Value{false}) {
        service_->deactivate_boolean(s.policy.iid, sample->sampled_ms, now);
        continue;
      }
      if (previous != observed_.end() && sample->sampled_ms == previous->second.sampled_ms) {
        if (sample->value != previous->second.value) unavailable();
        continue;
      }
      if (camera)
        service_->publish_visibility_capture(s.policy.iid, sample->sampled_ms, now);
      else if (content_on)
        service_->publish_request_intent(s.policy.iid, sample->sampled_ms, now);
      else
        service_->publish(s.policy.iid, sample->value, sample->sampled_ms, now);
      observed_[s.policy.iid] = *sample;
    } catch (const std::exception&) {
      unavailable();
    }
  }
  if (regular_due) refreshed_ = now;
}
void ConfiguredModel::disconnect() {
  service_->disconnect();
  refreshed_.reset();
  observed_.clear();
}
}  // namespace headunit::theme::vehicle
