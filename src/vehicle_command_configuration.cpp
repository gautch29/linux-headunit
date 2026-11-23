// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_command_configuration.hpp>
#include "theme/vehicle_json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace headunit::vehicle_control {
namespace {

using JsonValue = theme::opack::Value;
using JsonObject = theme::opack::Dictionary;
constexpr std::size_t maximum_file_bytes = 65536;
constexpr std::int64_t maximum_exact_integer = 9007199254740991;

void require(bool valid, const char* message) {
  if (!valid) throw std::invalid_argument(message);
}

const JsonObject& object(const JsonValue& value,
                         std::initializer_list<std::string_view> permitted) {
  const auto* fields = std::get_if<JsonObject>(&value.data);
  require(fields != nullptr, "vehicle controls field must be an object");
  for (const auto& [key, child] : *fields) {
    (void)child;
    const auto* name = std::get_if<std::string>(&key.data);
    require(name && std::find(permitted.begin(), permitted.end(), *name) != permitted.end(),
            "unknown vehicle controls configuration field");
  }
  return *fields;
}

const JsonValue* optional_field(const JsonObject& fields, std::string_view key) {
  for (const auto& [name, value] : fields)
    if (std::get<std::string>(name.data) == key) return &value;
  return nullptr;
}

const JsonValue& field(const JsonObject& fields, std::string_view key) {
  const auto* value = optional_field(fields, key);
  require(value != nullptr, "missing required vehicle controls configuration field");
  return *value;
}

std::int64_t integer(const JsonValue& value) {
  const auto* number = std::get_if<std::int64_t>(&value.data);
  require(number != nullptr, "vehicle controls field must be a JSON integer");
  return *number;
}

std::string_view string(const JsonValue& value) {
  const auto* text = std::get_if<std::string>(&value.data);
  require(text != nullptr, "vehicle controls field must be a JSON string");
  return *text;
}

bool flag(const JsonValue& value) {
  const auto* boolean = std::get_if<bool>(&value.data);
  require(boolean != nullptr, "vehicle controls writable must be a JSON boolean");
  return *boolean;
}

double bound(const JsonValue& value, ValueKind kind) {
  if (kind == ValueKind::integer) {
    const auto number = integer(value);
    require(number >= -maximum_exact_integer && number <= maximum_exact_integer,
            "vehicle controls integer bound exceeds exact numeric range");
    return static_cast<double>(number);
  }
  if (const auto* number = std::get_if<std::int64_t>(&value.data))
    return static_cast<double>(*number);
  const auto* number = std::get_if<double>(&value.data);
  require(number && std::isfinite(*number), "vehicle controls bound must be a finite JSON number");
  return *number;
}

TargetPolicy target(const JsonValue& value, std::int64_t version) {
  const auto& fields =
      object(value, {"key", "kind", "writable", "max_age_ms", "minimum", "maximum", "step"});
  const auto key = parse_key(string(field(fields, "key")));
  require(key.has_value(), "invalid semantic vehicle controls target key");
  TargetPolicy policy;
  policy.key = *key;
  const auto kind = string(field(fields, "kind"));
  if (kind == "boolean")
    policy.kind = ValueKind::boolean;
  else if (kind == "integer")
    policy.kind = ValueKind::integer;
  else if (kind == "real")
    policy.kind = ValueKind::real;
  else if (kind == "ambient_light" && version == 2)
    policy.kind = ValueKind::ambient_light;
  else
    throw std::invalid_argument("unknown vehicle controls target kind");
  if (const auto* writable = optional_field(fields, "writable")) policy.writable = flag(*writable);
  const auto age = integer(field(fields, "max_age_ms"));
  require(age >= 1 && age <= 86400000, "vehicle controls maximum age must be 1..86400000 ms");
  policy.maximum_age = std::chrono::milliseconds(age);
  if (policy.kind == ValueKind::boolean || policy.kind == ValueKind::ambient_light) {
    require(!optional_field(fields, "minimum") && !optional_field(fields, "maximum") &&
                !optional_field(fields, "step"),
            "boolean/ambient vehicle controls target cannot have numeric bounds");
  } else {
    policy.minimum = bound(field(fields, "minimum"), policy.kind);
    policy.maximum = bound(field(fields, "maximum"), policy.kind);
    policy.step = bound(field(fields, "step"), policy.kind);
  }
  require(valid_policy(policy), "invalid vehicle controls target policy");
  return policy;
}

}  // namespace

std::vector<TargetPolicy> parse_configuration(std::string_view json) {
  require(!json.empty() && json.size() <= maximum_file_bytes && json.find('\0') == json.npos,
          "vehicle controls configuration must be nonempty and at most 64 KiB");
  const auto parsed = theme::vehicle::detail::Json(json).parse();
  const auto& root = object(parsed, {"schemaVersion", "targets"});
  const auto version = integer(field(root, "schemaVersion"));
  require(version == 1 || version == 2, "unsupported vehicle controls schema version");
  const auto* targets = std::get_if<theme::opack::Array>(&field(root, "targets").data);
  require(targets && !targets->empty() && targets->size() <= maximum_targets,
          "vehicle controls configuration requires 1..32 targets");
  std::vector<TargetPolicy> policies;
  policies.reserve(targets->size());
  for (const auto& entry : *targets) {
    auto policy = target(entry, version);
    require(std::none_of(policies.begin(), policies.end(),
                         [&](const auto& existing) { return existing.key == policy.key; }),
            "duplicate semantic vehicle controls target key");
    policies.push_back(std::move(policy));
  }
  return policies;
}

std::vector<TargetPolicy> load_configuration(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error)
    throw std::runtime_error("vehicle controls configuration must be a readable regular file");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open vehicle controls configuration");
  std::string json(maximum_file_bytes + 1, '\0');
  input.read(json.data(), static_cast<std::streamsize>(json.size()));
  json.resize(static_cast<std::size_t>(input.gcount()));
  if (input.bad() || (input.fail() && !input.eof()))
    throw std::runtime_error("cannot read vehicle controls configuration");
  return parse_configuration(json);
}

}  // namespace headunit::vehicle_control
