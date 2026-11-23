// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_script_view.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <tuple>

namespace headunit::theme::vehicle {
namespace {
using opack::Array;
using opack::Dictionary;
using opack::Value;
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
Value text(const std::string& s) {
  return Value{s};
}
const Value* field(const Value& value, const char* name) {
  const auto* dict = std::get_if<Dictionary>(&value.data);
  check(dict, "script vehicle configuration dictionary required");
  for (const auto& [key, v] : *dict)
    if (const auto* s = std::get_if<std::string>(&key.data); s && *s == name) return &v;
  return nullptr;
}
const Array& children(const Value& value, const char* name) {
  const auto* f = field(value, name);
  check(f, "script vehicle configuration array missing");
  const auto* a = std::get_if<Array>(&f->data);
  check(a, "script vehicle configuration array required");
  return *a;
}
void put(Value& object, const std::string& key, Value value) {
  auto& dict = std::get<Dictionary>(object.data);
  for (auto& [k, v] : dict)
    if (k == text(key)) {
      v = std::move(value);
      return;
    }
  dict.emplace_back(text(key), std::move(value));
}
Value& child(Value& object, const std::string& key) {
  auto& dict = std::get<Dictionary>(object.data);
  for (auto& [k, v] : dict)
    if (k == text(key)) return v;
  dict.emplace_back(text(key), Value{Dictionary{}});
  return dict.back().second;
}
bool component(const std::string& s) {
  return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
  });
}
}  // namespace
ScriptView::ScriptView(std::shared_ptr<ReadOnlyService> source, std::vector<ScriptBinding> bindings,
                       Dismissals dismissals)
    : source_(std::move(source)), dismissals_(std::move(dismissals)) {
  check(bool(source_) && bindings.size() <= 512, "invalid script vehicle source");
  std::map<std::int64_t, Value> characteristics;
  for (const auto& a : children(source_->profile(), "accessories"))
    for (const auto& s : children(a, "services"))
      for (const auto& c : children(s, "characteristics")) {
        const auto* id = field(c, "iid");
        check(id && std::holds_alternative<std::int64_t>(id->data), "invalid characteristic IID");
        characteristics.emplace(std::get<std::int64_t>(id->data), c);
      }
  std::set<std::pair<std::array<std::string, 3>, std::optional<std::string>>> paths;
  std::map<std::pair<std::string, std::string>, bool> service_modes;
  for (auto& binding : bindings) {
    check(binding.iid >= 0 && binding.iid <= 9007199254740991LL && source_->contains(binding.iid) &&
              std::all_of(binding.path.begin(), binding.path.end(), component) &&
              (!binding.instance || component(*binding.instance)) &&
              paths.emplace(binding.path, binding.instance).second &&
              (!binding.format || *binding.format >= 0),
          "invalid or duplicate script vehicle binding");
    const auto [mode, inserted] = service_modes.emplace(
        std::make_pair(binding.path[0], binding.path[1]), binding.instance.has_value());
    check(inserted || mode->second == binding.instance.has_value(),
          "mixed single and repeated script vehicle service");
    Value metadata{Dictionary{{text("iid"), Value{binding.iid}}}};
    const auto& original = characteristics.at(binding.iid);
    for (const auto* key : {"minimumValue", "maximumValue", "stepValue", "units", "validValues",
                            "supportsInvalid", "supportsError"})
      if (const auto* value = field(original, key)) put(metadata, key, *value);
    if (binding.format) put(metadata, "format", Value{std::int64_t(*binding.format)});
    const auto id = binding.iid;
    check(entries_.emplace(id, Entry{std::move(binding), std::move(metadata)}).second,
          "duplicate script vehicle IID");
  }
  for (const auto& [id, callback] : dismissals_)
    check(callback && entries_.contains(id) &&
              entries_.at(id).binding.path ==
                  std::array<std::string, 3>{"automakerRequestContent", "requestContent", "on"} &&
              source_->policy(id) && source_->policy(id)->kind == ValueKind::boolean,
          "local dismissal must bind a declared boolean RequestContent On");
}
void ScriptView::validate(std::span<const std::int64_t> ids) const {
  check(ids.size() <= 512, "too many script vehicle IDs");
  for (const auto id : ids)
    check(entries_.contains(id), "unmapped script vehicle IID");
}
opack::Value ScriptView::configuration(std::uint64_t now) const {
  std::vector<std::int64_t> ids;
  for (const auto& [id, entry] : entries_) {
    (void)entry;
    ids.push_back(id);
  }
  const auto readings = source_->snapshot(ids, now);
  Value tree{Dictionary{}};
  using Instances = std::map<std::optional<std::string>, Value>;
  std::map<std::pair<std::string, std::string>, Instances> services;
  for (const auto& [id, entry] : entries_) {
    auto config = entry.metadata;
    const auto& reading = readings.at(id);
    if (reading.value) put(config, "initialValue", *reading.value);
    auto& instances = services[{entry.binding.path[0], entry.binding.path[1]}];
    auto [instance, inserted] = instances.try_emplace(entry.binding.instance, Value{Dictionary{}});
    (void)inserted;
    put(instance->second, entry.binding.path[2], std::move(config));
  }
  for (auto& [path, instances] : services) {
    if (!instances.begin()->first)
      put(child(tree, path.first), path.second, std::move(instances.begin()->second));
    else {
      Array list;
      list.reserve(instances.size());
      for (auto& [name, value] : instances) {
        (void)name;
        list.push_back(std::move(value));
      }
      put(child(tree, path.first), path.second, Value{std::move(list)});
    }
  }
  return tree;
}
void ScriptView::subscribe(std::span<const std::int64_t> ids) {
  validate(ids);
  std::set<std::int64_t> unique(ids.begin(), ids.end());
  subscribed_.assign(unique.begin(), unique.end());
  delivered_.clear();
}
void ScriptView::append(ScriptRead& out, std::int64_t id, const ReadOnlyService::Reading& reading) {
  // Every unavailable value explicitly clears a local consumer's previous
  // reading. Null maps to CP.INVALIDVALUE; wire errors are retained separately.
  out.values.emplace(id, reading.value.value_or(Value{}));
  if (reading.error) out.errors.emplace(id, *reading.error);
}
ScriptRead ScriptView::read(std::span<const std::int64_t> ids, std::uint64_t now) const {
  validate(ids);
  ScriptRead result;
  for (const auto& [id, reading] : source_->snapshot(ids, now))
    append(result, id, reading);
  return result;
}
ScriptRead ScriptView::updates(std::uint64_t now) {
  const auto readings = source_->snapshot(subscribed_, now);
  ScriptRead result;
  std::set<std::tuple<std::string, std::string, std::optional<std::string>>> changed_instances;
  const auto complete_state = [](const ScriptBinding& binding) {
    return binding.instance || (binding.path[0] == "uiControl" && binding.path[1] == "uiState") ||
           (binding.path[0] == "driveState" && binding.path[1] == "transmissionStatus");
  };
  for (const auto& [id, reading] : readings) {
    const auto old = delivered_.find(id);
    if (old != delivered_.end() && old->second == reading) continue;
    const auto& binding = entries_.at(id).binding;
    if (complete_state(binding))
      changed_instances.emplace(binding.path[0], binding.path[1], binding.instance);
    else
      append(result, id, reading);
  }
  // Repeated service, signed scene-state/options and transmission handlers
  // consume a complete state, including an unchanged selector beside a new gear.
  // Repeat only already-subscribed values in the changed group. snapshot()
  // applies each signal's own freshness policy;
  // copying a value into an update never renews its acquisition time.
  for (const auto& [id, reading] : readings) {
    const auto& binding = entries_.at(id).binding;
    if (complete_state(binding) &&
        changed_instances.contains({binding.path[0], binding.path[1], binding.instance}))
      append(result, id, reading);
  }
  delivered_ = readings;
  return result;
}
ScriptRead ScriptView::reject_writes(std::span<const std::int64_t> ids) const {
  check(ids.size() <= 512, "too many script vehicle writes");
  ScriptRead result;
  for (const auto id : ids)
    result.errors.emplace(id, entries_.contains(id) ? -6710 : -6727);
  return result;  // No accepted values, no sensor mutation, no actuator path.
}
ScriptRead ScriptView::dismiss(const std::map<std::int64_t, opack::Value>& values,
                               std::uint64_t now) const {
  check(values.size() <= 512, "too many script vehicle writes");
  ScriptRead result;
  for (const auto& [id, value] : values) {
    const auto callback = dismissals_.find(id);
    if (callback == dismissals_.end() || value != Value{false}) {
      result.errors.emplace(id, entries_.contains(id) ? -6710 : -6727);
      continue;
    }
    callback->second(now);
    result.values.emplace(id, Value{false});
  }
  return result;
}
}  // namespace headunit::theme::vehicle
