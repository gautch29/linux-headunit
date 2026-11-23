// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/local_vehicle_profile.hpp>
#include <headunit/theme/script_runtime.hpp>
#include <plist/plist.h>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace headunit::theme;
namespace v = vehicle;
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("bad local profile accepted");
}
struct Document {
  plist_t root = plist_new_dict(), signals = plist_new_array();
  Document() {
    plist_dict_set_item(root, "schemaVersion", plist_new_uint(1));
    plist_dict_set_item(root, "mode", plist_new_string("local-ui-bindings"));
    plist_dict_set_item(root, "signals", signals);
  }
  ~Document() { plist_free(root); }
  plist_t add(std::uint64_t id, const char* name, const char* kind) {
    auto signal = plist_new_dict(), path = plist_new_array();
    for (const auto* part : {"testVehicle", "gauge", name})
      plist_array_append_item(path, plist_new_string(part));
    plist_dict_set_item(signal, "iid", plist_new_uint(id));
    plist_dict_set_item(signal, "path", path);
    plist_dict_set_item(signal, "kind", plist_new_string(kind));
    plist_dict_set_item(signal, "maxAgeMs", plist_new_uint(100));
    plist_array_append_item(signals, signal);
    return signal;
  }
  v::LocalProfile decode(bool binary = true) {
    char* raw = nullptr;
    std::uint32_t size = 0;
    if (binary)
      plist_to_bin(root, &raw, &size);
    else
      plist_to_xml(root, &raw, &size);
    std::unique_ptr<char, decltype(&std::free)> data(raw, std::free);
    return v::decode_local_profile({reinterpret_cast<const std::uint8_t*>(raw), size});
  }
};
plist_t transmission_signal(Document& d, std::uint64_t id, const char* name, const char* field,
                            const char* kind = "integer") {
  auto signal = d.add(id, name, kind);
  plist_dict_set_item(signal, "vehicleField", plist_new_string(field));
  return signal;
}
plist_t integer_states(plist_t signal,
                       std::initializer_list<std::pair<const char*, std::int64_t>> values) {
  auto states = plist_new_dict();
  for (const auto& [key, value] : values)
    plist_dict_set_item(states, key, plist_new_int(value));
  plist_dict_set_item(signal, "vehicleStates", states);
  return states;
}
void drive_mode_path(plist_t signal, const char* name) {
  auto path = plist_new_array();
  for (const auto* key : {"driveState", "driveMode", name})
    plist_array_append_item(path, plist_new_string(key));
  plist_dict_set_item(signal, "path", path);
}
plist_t authored_modes(Document& d) {
  const auto signal = d.add(3, "identifiers", "array"), values = plist_new_array();
  drive_mode_path(signal, "identifiers");
  // Authored order deliberately differs from the source's semantic catalogue.
  for (const auto* name : {"sport", "GT"})
    plist_array_append_item(values, plist_new_string(name));
  plist_dict_set_item(signal, "fixedValue", values);
  return signal;
}
plist_t observed_mode(Document& d) {
  const auto signal = transmission_signal(d, 4, "currentIndex", "drive_mode");
  drive_mode_path(signal, "currentIndex");
  integer_states(signal, {{"comfort", 1}, {"sport", 0}});
  return signal;
}
int main() {
  try {
    for (bool binary : {false, true}) {
      Document d;
      auto identity = plist_new_dict();
      plist_dict_set_item(identity, "make", plist_new_string("Land Rover"));
      plist_dict_set_item(identity, "model", plist_new_string("Defender 110"));
      plist_dict_set_item(identity, "year", plist_new_uint(1998));
      plist_dict_set_item(d.root, "vehicleIdentity", identity);
      const auto profile = d.decode(binary);
      check(profile.identity && profile.identity->make == "Land Rover" &&
                profile.identity->model == "Defender 110" && profile.identity->year == 1998 &&
                profile.bindings.empty() && profile.vehicle_inputs.empty(),
            "vehicle identity fabricated fitted inputs");
      plist_dict_set_item(identity, "year", plist_new_uint(1885));
      rejects([&] { d.decode(binary); });
      plist_dict_set_item(identity, "year", plist_new_uint(1998));
      plist_dict_set_item(identity, "model", plist_new_string("Defender\n110"));
      rejects([&] { d.decode(binary); });
      plist_dict_set_item(identity, "model", plist_new_string("Defender 110"));
      plist_dict_set_item(identity, "vin", plist_new_string("undeclared"));
      rejects([&] { d.decode(binary); });
    }
    for (bool binary : {false, true}) {
      Document d;
      d.add(3, "speed", "real");
      const auto maximum = d.add(4, "maximum", "real");
      plist_dict_set_item(maximum, "fixedValue", plist_new_real(220));
      plist_dict_set_item(maximum, "minimumValue", plist_new_real(1));
      plist_dict_set_item(maximum, "maximumValue", plist_new_real(300));
      plist_dict_set_item(maximum, "units", plist_new_uint(1032));
      auto profile = d.decode(binary);
      v::ScriptView view(profile.source, profile.bindings);
      const std::array<std::int64_t, 2> ids{3, 4};
      auto read = view.read(ids, 10000);
      check(read.errors.empty() && read.values.at(3) == opack::Value{} &&
                read.values.at(4) == opack::Value{220.0},
            "missing sample differs from fixed config");
      profile.source->publish(3, opack::Value{25.0}, 10000, 10000);
      check(view.read(ids, 10099).values.at(3) == opack::Value{25.0}, "fresh sample missing");
      check(view.read(ids, 10100).values.at(3) == opack::Value{}, "expired sample retained");
      profile.source->disconnect();
      check(view.read(ids, 0).values.at(4) == opack::Value{220.0},
            "fixed config lost on disconnect");
      rejects([&] { profile.source->publish(4, opack::Value{240.0}, 1, 1); });
    }
    for (const auto* key : {"initialValue", "unknown", "type"}) {
      Document d;
      auto s = d.add(3, "speed", "real");
      plist_dict_set_item(s, key, plist_new_uint(0));
      rejects([&] { d.decode(); });
    }
    for (std::uint64_t id : {0ULL, 2ULL, 9007199254740992ULL, ~0ULL}) {
      Document d;
      d.add(id, "speed", "real");
      rejects([&] { d.decode(); });
    }
    for (const auto* kind : {"real", "integer", "boolean", "bytes"}) {
      Document d;
      auto s = d.add(3, "speed", kind);
      plist_dict_set_item(s, "fixedValue", plist_new_string("wrong"));
      rejects([&] { d.decode(); });
    }
    for (const auto* field : {"maxAgeMs", "units", "scriptFormat"}) {
      Document d;
      auto s = d.add(3, "speed", "real");
      plist_dict_set_item(s, field, plist_new_bool(1));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto s = d.add(3, "speed", "real");
      plist_dict_set_item(s, "maxAgeMs", plist_new_uint(0));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      d.add(3, "speed", "real");
      d.add(3, "rpm", "real");
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      d.add(3, "speed", "real");
      d.add(4, "speed", "real");
      rejects([&] { d.decode(); });
    }
    for (bool binary : {false, true}) {
      Document d;
      auto left = d.add(3, "value", "real"), right = d.add(4, "value", "real");
      plist_dict_set_item(left, "instance", plist_new_string("left"));
      plist_dict_set_item(right, "instance", plist_new_string("right"));
      const auto profile = d.decode(binary);
      check(profile.bindings.size() == 2 && profile.bindings[0].instance == "left" &&
                profile.bindings[1].instance == "right",
            "repeated service instance lost");
      plist_dict_set_item(right, "instance", plist_new_string("left"));
      rejects([&] { d.decode(binary); });
    }
    for (const auto* instance : {"", "bad.name", "bad/name"}) {
      Document d;
      auto s = d.add(3, "value", "real");
      plist_dict_set_item(s, "instance", plist_new_string(instance));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto s = d.add(3, "value", "real");
      plist_dict_set_item(s, "instance", plist_new_uint(1));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto s = d.add(3, "value", "real");
      plist_dict_set_item(s, "instance", plist_new_string("one"));
      d.add(4, "other", "real");
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      d.add(3, "bad.path", "real");
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto s = d.add(3, "speed", "real");
      plist_dict_set_item(s, "minimumValue", plist_new_real(1));
      plist_dict_set_item(s, "fixedValue", plist_new_real(0));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      plist_dict_set_item(d.root, "mode", plist_new_string("phone-vdp"));
      rejects([&] { d.decode(); });
    }
    for (bool binary : {false, true}) {
      for (auto value : {9007199254740992ULL, 18446744073709551615ULL}) {
        Document d;
        auto s = d.add(3, "maximum", "integer");
        plist_dict_set_item(s, "fixedValue", plist_new_uint(value));
        rejects([&] { d.decode(binary); });
      }
      Document d;
      auto s = d.add(3, "offset", "integer");
      plist_dict_set_item(s, "fixedValue", plist_new_int(-4));
      s = d.add(4, "label", "string");
      plist_dict_set_item(s, "fixedValue", plist_new_string(""));
      s = d.add(5, "opaque", "bytes");
      plist_dict_set_item(s, "fixedValue", plist_new_data("", 0));
      auto profile = d.decode(binary);
      v::ScriptView view(profile.source, profile.bindings);
      const std::array<std::int64_t, 3> ids{3, 4, 5};
      const auto read = view.read(ids, 0);
      check(read.values.at(3) == opack::Value{std::int64_t(-4)} &&
                read.values.at(4) == opack::Value{std::string{}} &&
                read.values.at(5) == opack::Value{Bytes{}},
            "signed and empty fixed values");
    }
    for (const auto* invalid : {"unknown_input", ""}) {
      Document d;
      auto item = d.add(3, "fuel", "real");
      plist_dict_set_item(item, "vehicleField", plist_new_string(invalid));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto item = d.add(3, "lamp", "integer");
      plist_dict_set_item(item, "vehicleField", plist_new_string("check_engine"));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto item = d.add(3, "fuel", "real");
      plist_dict_set_item(item, "vehicleField", plist_new_string("fuel_percent"));
      plist_dict_set_item(item, "fixedValue", plist_new_real(50));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto item = d.add(3, "fuel", "real");
      plist_dict_set_item(item, "vehicleScale", plist_new_real(0.01));
      rejects([&] { d.decode(); });
    }
    for (bool binary : {false, true}) {
      Document d;
      const auto selector = transmission_signal(d, 3, "selector", "transmission.selector");
      integer_states(selector, {{"park", 10}, {"drive", 20}, {"reverse", -7}});
      plist_dict_set_item(selector, "minimumValue", plist_new_int(-10));
      plist_dict_set_item(selector, "maximumValue", plist_new_int(20));
      const auto gear = transmission_signal(d, 4, "gear", "transmission.gear", "string");
      auto states = plist_new_dict();
      plist_dict_set_item(states, "neutral", plist_new_string("N"));
      plist_dict_set_item(states, "reverse", plist_new_string("R"));
      plist_dict_set_item(states, "32", plist_new_string("G32"));
      plist_dict_set_item(gear, "vehicleStates", states);
      const auto type = transmission_signal(d, 5, "type", "transmission.gearbox_type", "string");
      auto types = plist_new_dict();
      plist_dict_set_item(types, "manual", plist_new_string("Manual gearbox"));
      plist_dict_set_item(types, "automated_manual", plist_new_string("AMT"));
      plist_dict_set_item(type, "vehicleStates", types);
      const auto lamp = d.add(6, "lamp", "boolean");
      plist_dict_set_item(lamp, "vehicleField", plist_new_string("high_beam"));
      auto lamps = plist_new_array();
      for (bool value : {false, true, true})
        plist_array_append_item(lamps, plist_new_bool(value));
      plist_dict_set_item(lamp, "vehicleStates", lamps);
      const auto profile = d.decode(binary);
      check(profile.vehicle_inputs[0].semantic_states.at("reverse") ==
                    opack::Value{std::int64_t(-7)} &&
                profile.vehicle_inputs[1].semantic_states.at("32") ==
                    opack::Value{std::string("G32")} &&
                profile.vehicle_inputs[2].semantic_states.at("manual") ==
                    opack::Value{std::string("Manual gearbox")},
            "explicit semantic transmission mapping lost");
      check(profile.vehicle_inputs[0].states.empty() &&
                profile.vehicle_inputs[3].states.size() == 3 &&
                profile.vehicle_inputs[3].semantic_states.empty(),
            "transmission dictionary changed lamp array mapping");
    }
    for (const auto* field :
         {"transmission.selector", "transmission.gear", "transmission.gearbox_type"}) {
      Document d;
      const auto signal = transmission_signal(d, 3, "state", field);
      rejects([&] { d.decode(); });
      plist_dict_set_item(signal, "vehicleStates", plist_new_dict());
      rejects([&] { d.decode(); });
      plist_dict_set_item(signal, "vehicleStates", plist_new_array());
      rejects([&] { d.decode(); });
    }
    for (const auto* key : {"automatic", "unknown", "D", "", "park ", "neutral/drive"}) {
      Document d;
      integer_states(transmission_signal(d, 3, "selector", "transmission.selector"), {{key, 1}});
      rejects([&] { d.decode(); });
    }
    for (const auto* key : {"0", "33", "01", "+1", "-1", "forward", "park"}) {
      Document d;
      integer_states(transmission_signal(d, 3, "gear", "transmission.gear"), {{key, 1}});
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      integer_states(transmission_signal(d, 3, "type", "transmission.gearbox_type"),
                     {{"sport", 1}});
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto signal = transmission_signal(d, 3, "allGears", "transmission.gear"),
           states = integer_states(signal, {{"neutral", 0}, {"reverse", -1}});
      for (unsigned gear = 1; gear <= 32; ++gear)
        plist_dict_set_item(states, std::to_string(gear).c_str(), plist_new_uint(gear));
      check(d.decode().vehicle_inputs[0].semantic_states.size() == 34,
            "complete bounded gear map rejected");
    }
    for (const auto* option : {"vehicleScale", "vehicleOffset"}) {
      Document d;
      const auto signal = transmission_signal(d, 3, "selector", "transmission.selector");
      integer_states(signal, {{"drive", 1}});
      plist_dict_set_item(signal, option, plist_new_real(1));
      rejects([&] { d.decode(); });
    }
    for (const auto* kind : {"boolean", "real", "bytes"}) {
      Document d;
      integer_states(transmission_signal(d, 3, "selector", "transmission.selector", kind),
                     {{"drive", 1}});
      rejects([&] { d.decode(); });
    }
    for (unsigned shape = 0; shape < 4; ++shape) {
      Document d;
      const auto signal = transmission_signal(d, 3, "selector", "transmission.selector");
      auto states = plist_new_dict();
      plist_dict_set_item(states, "drive",
                          shape == 0   ? plist_new_bool(1)
                          : shape == 1 ? plist_new_real(1)
                          : shape == 2 ? plist_new_string("D")
                                       : plist_new_uint(9007199254740992ULL));
      plist_dict_set_item(signal, "vehicleStates", states);
      rejects([&] { d.decode(); });
    }
    for (unsigned length : {0U, 128U, 129U}) {
      Document d;
      const auto signal = transmission_signal(d, 3, "selector", "transmission.selector", "string"),
                 states = plist_new_dict();
      plist_dict_set_item(states, "drive", plist_new_string(std::string(length, 'x').c_str()));
      plist_dict_set_item(signal, "vehicleStates", states);
      if (length == 128)
        (void)d.decode();
      else
        rejects([&] { d.decode(); });
    }
    {
      Document d;
      const auto signal = transmission_signal(d, 3, "selector", "transmission.selector", "string");
      integer_states(signal, {{"drive", 1}});
      rejects([&] { d.decode(); });
    }
    for (std::int64_t value : {-1, 11}) {
      Document d;
      const auto signal = transmission_signal(d, 3, "selector", "transmission.selector");
      integer_states(signal, {{"drive", value}});
      plist_dict_set_item(signal, "minimumValue", plist_new_uint(0));
      plist_dict_set_item(signal, "maximumValue", plist_new_uint(10));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      const auto signal = transmission_signal(d, 3, "type", "transmission.gearbox_type");
      integer_states(signal, {{"manual", 77}});
      auto path = plist_new_array();
      for (const auto* key : {"driveState", "transmissionStatus", "transmissionMode"})
        plist_array_append_item(path, plist_new_string(key));
      plist_dict_set_item(signal, "path", path);
      rejects([&] { d.decode(); });
    }
    for (bool binary : {false, true}) {
      Document d;
      authored_modes(d);
      observed_mode(d);
      auto profile = d.decode(binary);
      const std::array<std::int64_t, 2> ids{3, 4};
      v::ScriptView view(profile.source, profile.bindings);
      check(profile.vehicle_inputs[0].semantic_states.at("comfort") ==
                    opack::Value{std::int64_t(1)} &&
                profile.vehicle_inputs[0].semantic_states.at("sport") ==
                    opack::Value{std::int64_t(0)},
            "authored mode order changed explicit source mapping");
      auto read = view.read(ids, 1000);
      check(read.values.at(4) == opack::Value{} && !profile.source->timed(3) &&
                profile.source->timed(4),
            "immutable mode catalogue inferred an active mode");
      profile.source->publish(4, opack::Value{std::int64_t(0)}, 1000, 1000);
      v::validate_local_vehicle_inputs(profile);
      check(view.read(ids, 1001).values.at(4) == opack::Value{std::int64_t(0)},
            "configuration validation advanced the source clock");
      auto direct = profile;
      direct.vehicle_inputs[0].semantic_states["comfort"] = opack::Value{std::int64_t(0)};
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.vehicle_inputs[0].semantic_states["comfort"] = opack::Value{std::int64_t(2)};
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.vehicle_inputs[0].semantic_states["comfort"] = opack::Value{1.};
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.vehicle_inputs[0].semantic_states.clear();
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.vehicle_inputs[0].scale = 2;
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.bindings.erase(direct.bindings.begin());
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.bindings[0].instance = "other";
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
    }
    {
      Document d;
      const auto signal = transmission_signal(d, 3, "modeLabel", "drive_mode", "string"),
                 states = plist_new_dict();
      plist_dict_set_item(states, "comfort", plist_new_string("Comfort"));
      plist_dict_set_item(signal, "vehicleStates", states);
      check(d.decode().vehicle_inputs[0].semantic_states.at("comfort") ==
                opack::Value{std::string("Comfort")},
            "generic semantic mode labels rejected");
    }
    {
      Document d;
      const auto signal = transmission_signal(d, 3, "mode", "drive_mode");
      rejects([&] { d.decode(); });
      plist_dict_set_item(signal, "vehicleStates", plist_new_dict());
      rejects([&] { d.decode(); });
      plist_dict_set_item(signal, "vehicleStates", plist_new_array());
      rejects([&] { d.decode(); });
    }
    for (const auto& key :
         std::array<std::string, 8>{"", "1sport", "sport+", "sport mode", "sport.mode", "é",
                                    "sport/comfort", std::string(65, 's')}) {
      Document d;
      integer_states(transmission_signal(d, 3, "mode", "drive_mode"), {{key.c_str(), 1}});
      rejects([&] { d.decode(); });
    }
    for (unsigned count : {32U, 33U}) {
      Document d;
      const auto signal = transmission_signal(d, 3, "mode", "drive_mode"),
                 states = plist_new_dict();
      for (unsigned i = 0; i < count; ++i)
        plist_dict_set_item(states, ("m" + std::to_string(i)).c_str(), plist_new_uint(i));
      plist_dict_set_item(signal, "vehicleStates", states);
      if (count == 32)
        check(d.decode().vehicle_inputs[0].semantic_states.size() == 32,
              "bounded mode mapping rejected");
      else
        rejects([&] { d.decode(); });
    }
    for (const auto* kind : {"boolean", "real", "bytes"}) {
      Document d;
      integer_states(transmission_signal(d, 3, "mode", "drive_mode", kind), {{"sport", 1}});
      rejects([&] { d.decode(); });
    }
    for (const auto* option : {"vehicleScale", "vehicleOffset", "fixedValue"}) {
      Document d;
      const auto signal = transmission_signal(d, 3, "mode", "drive_mode");
      integer_states(signal, {{"sport", 1}});
      plist_dict_set_item(signal, option,
                          plist_new_uint(option == std::string_view("vehicleOffset") ? 0 : 1));
      rejects([&] { d.decode(); });
    }
    for (unsigned shape = 0; shape < 4; ++shape) {
      Document d;
      const auto signal = transmission_signal(d, 3, "mode", "drive_mode"),
                 states = plist_new_dict();
      plist_dict_set_item(states, "sport",
                          shape == 0   ? plist_new_bool(1)
                          : shape == 1 ? plist_new_real(1)
                          : shape == 2 ? plist_new_string("Sport")
                                       : plist_new_uint(9007199254740992ULL));
      plist_dict_set_item(signal, "vehicleStates", states);
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      const auto signal = transmission_signal(d, 3, "mode", "drive_mode");
      integer_states(signal, {{"sport", 2}});
      plist_dict_set_item(signal, "maximumValue", plist_new_uint(1));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      observed_mode(d);
      rejects([&] { d.decode(); });
    }
    for (unsigned shape = 0; shape < 6; ++shape) {
      Document d;
      const auto catalog = authored_modes(d);
      observed_mode(d);
      auto values = plist_new_array();
      if (shape == 0) plist_array_append_item(values, plist_new_bool(0));
      if (shape == 1) {
        plist_array_append_item(values, plist_new_string("sport"));
        plist_array_append_item(values, plist_new_string("sport"));
      }
      if (shape == 2) {
        plist_array_append_item(values, plist_new_string(""));
        plist_array_append_item(values, plist_new_string("GT"));
      }
      if (shape == 3) {
        plist_array_append_item(values, plist_new_string("sport\n"));
        plist_array_append_item(values, plist_new_string("GT"));
      }
      if (shape == 4) {
        plist_array_append_item(values, plist_new_string("sport"));
        plist_dict_set_item(catalog, "instance", plist_new_string("other"));
      }
      plist_dict_set_item(catalog, "fixedValue", values);
      rejects([&] { d.decode(); });
    }
    for (std::int64_t index : {-1, 0, 2}) {
      Document d;
      authored_modes(d);
      const auto active = observed_mode(d);
      integer_states(active, {{"comfort", index}, {"sport", 0}});
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      authored_modes(d);
      const auto active = observed_mode(d);
      plist_dict_set_item(active, "kind", plist_new_string("string"));
      const auto states = plist_new_dict();
      plist_dict_set_item(states, "sport", plist_new_string("0"));
      plist_dict_set_item(active, "vehicleStates", states);
      rejects([&] { d.decode(); });
    }
    for (bool binary : {false, true}) {
      Document d;
      const auto fuel =
          transmission_signal(d, 3, "fuelEfficiency", "trip.fuel_efficiency_l_per_100km", "real");
      const auto label = d.add(4, "userVisibleLabel", "string");
      for (const auto signal : {fuel, label}) {
        auto path = plist_new_array();
        for (const auto* part :
             {"tripComputer", "trip", signal == fuel ? "fuelEfficiency" : "userVisibleLabel"})
          plist_array_append_item(path, plist_new_string(part));
        plist_dict_set_item(signal, "path", path);
        plist_dict_set_item(signal, "instance", plist_new_string("currentTrip"));
      }
      plist_dict_set_item(fuel, "vehicleScale", plist_new_real(1000));
      plist_dict_set_item(fuel, "units", plist_new_uint(674));
      plist_dict_set_item(label, "fixedValue", plist_new_string("Current Trip"));
      auto profile = d.decode(binary);
      const std::array<std::int64_t, 2> ids{3, 4};
      v::ScriptView view(profile.source, profile.bindings);
      auto read = view.read(ids, 1000);
      check(read.values.at(3) == opack::Value{} &&
                read.values.at(4) == opack::Value{std::string("Current Trip")} &&
                profile.vehicle_inputs[0].scale == 1000 &&
                profile.vehicle_inputs[0].field == "trip.fuel_efficiency_l_per_100km",
            "trip label/unit configuration fabricated or replaced consumption");
      auto direct = profile;
      direct.vehicle_inputs[0].scale = 0;
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.vehicle_inputs[0].offset = std::numeric_limits<double>::infinity();
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.vehicle_inputs[0].states = {opack::Value{std::int64_t(0)}};
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
      direct = profile;
      direct.vehicle_inputs[0].semantic_states = {{"default", opack::Value{0.}}};
      rejects([&] { v::validate_local_vehicle_inputs(direct); });
    }
    for (const auto* field : {"trip.distance_km", "trip.elapsed_seconds", "trip.average_speed_kmh",
                              "heading.degrees"}) {
      Document d;
      transmission_signal(d, 3, "unknownPath", field, "real");
      rejects([&] { d.decode(); });
    }
    for (const auto* kind : {"boolean", "string", "bytes"}) {
      Document d;
      transmission_signal(d, 3, "consumption", "trip.fuel_efficiency_l_per_100km", kind);
      rejects([&] { d.decode(); });
    }
    for (double scale : {0., 1000001., std::numeric_limits<double>::quiet_NaN()}) {
      Document d;
      const auto signal =
          transmission_signal(d, 3, "consumption", "trip.fuel_efficiency_l_per_100km", "real");
      plist_dict_set_item(signal, "vehicleScale", plist_new_real(scale));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      const auto signal =
          transmission_signal(d, 3, "consumption", "trip.fuel_efficiency_l_per_100km", "real");
      plist_dict_set_item(signal, "fixedValue", plist_new_real(0));
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      const auto signal =
          transmission_signal(d, 3, "consumption", "trip.fuel_efficiency_l_per_100km", "real");
      plist_dict_set_item(signal, "vehicleStates", plist_new_dict());
      rejects([&] { d.decode(); });
    }
    {
      Document d;
      const auto signal =
          transmission_signal(d, 3, "consumption", "trip.fuel_efficiency_l_per_100km", "real");
      plist_dict_set_item(signal, "vehicleScale", plist_new_bool(1));
      rejects([&] { d.decode(); });
    }
    for (bool binary : {false, true}) {
      Document d;
      auto item = d.add(3, "identifiers", "array"), values = plist_new_array();
      for (const auto* name : {"GT", "sport"})
        plist_array_append_item(values, plist_new_string(name));
      plist_dict_set_item(item, "fixedValue", values);
      const auto index = d.add(4, "currentIndex", "integer");
      plist_dict_set_item(index, "fixedValue", plist_new_uint(1));
      auto profile = d.decode(binary);
      v::ScriptView view(profile.source, profile.bindings);
      const std::array<std::int64_t, 1> ids{3};
      const opack::Value expected{
          opack::Array{opack::Value{std::string("GT")}, opack::Value{std::string("sport")}}};
      check(view.read(ids, 1000).values.at(3) == expected,
            "fixed array not supplied to the script view");
      script::Runtime runtime(script::ResourceReader{[](std::string_view) -> std::string {
                                throw std::runtime_error("unexpected resource read");
                              }},
                              {"Cluster_Display"});
      runtime.bind_vehicle(profile.source, profile.bindings, [] { return 1000; });
      runtime.evaluate(R"(
      local config=CP.getVehicleDataConfig().testVehicle.gauge
      local identifiers=config.identifiers.initialValue
      assert(#identifiers==2 and identifiers[1]=='GT' and identifiers[2]=='sport')
      assert(identifiers[config.currentIndex.initialValue+1]=='sport')
      local values=CP.getVehicleData({3})
      assert(#values[3]==2 and values[3][2]=='sport')
    )",
                       "fixed-array-indexing");
      profile.source->disconnect();
      check(view.read(ids, 0).values.at(3) == expected, "fixed array cleared on disconnect");
      rejects([&] { profile.source->publish(3, opack::Value{opack::Array{}}, 1, 1); });
      rejects([&] { profile.source->invalidate(3, 1, 1); });
    }
    for (bool binary : {false, true}) {
      Document d;
      auto item = d.add(3, "primitives", "array"), values = plist_new_array();
      plist_array_append_item(values, plist_new_bool(1));
      plist_array_append_item(values, plist_new_int(-2));
      plist_array_append_item(values, plist_new_real(0.5));
      plist_array_append_item(values, plist_new_string(""));
      plist_dict_set_item(item, "fixedValue", values);
      const auto profile = d.decode(binary);
      v::ScriptView view(profile.source, profile.bindings);
      const std::array<std::int64_t, 1> ids{3};
      check(view.read(ids, 0).values.at(3) ==
                opack::Value{opack::Array{opack::Value{true}, opack::Value{std::int64_t(-2)},
                                          opack::Value{0.5}, opack::Value{std::string{}}}},
            "primitive array types changed");
    }
    {
      Document d;
      d.add(3, "timedArray", "array");
      rejects([&] { d.decode(); });
    }
    for (const auto* field : {"minimumValue", "maximumValue", "vehicleField"}) {
      Document d;
      auto item = d.add(3, "identifiers", "array");
      plist_dict_set_item(item, "fixedValue", plist_new_array());
      plist_dict_set_item(item, field,
                          field == std::string_view("vehicleField")
                              ? plist_new_string("fuel_percent")
                              : plist_new_real(0));
      rejects([&] { d.decode(); });
    }
    for (unsigned count : {32U, 33U}) {
      Document d;
      auto item = d.add(3, "flags", "array"), values = plist_new_array();
      for (unsigned i = 0; i < count; ++i)
        plist_array_append_item(values, plist_new_bool(0));
      plist_dict_set_item(item, "fixedValue", values);
      if (count == 32)
        (void)d.decode();
      else
        rejects([&] { d.decode(); });
    }
    for (unsigned length : {128U, 129U}) {
      Document d;
      auto item = d.add(3, "labels", "array"), values = plist_new_array();
      plist_array_append_item(values, plist_new_string(std::string(length, 'x').c_str()));
      plist_dict_set_item(item, "fixedValue", values);
      if (length == 128)
        (void)d.decode();
      else
        rejects([&] { d.decode(); });
    }
    {
      Document d;
      auto item = d.add(3, "labels", "array"), values = plist_new_array();
      for (unsigned i = 0; i < 16; ++i)
        plist_array_append_item(
            values, plist_new_string((std::to_string(i) + std::string(126, 'x')).c_str()));
      plist_dict_set_item(item, "fixedValue", values);
      rejects([&] { d.decode(); });
    }
    for (unsigned shape = 0; shape < 3; ++shape) {
      Document d;
      auto item = d.add(3, "nested", "array"), values = plist_new_array();
      plist_array_append_item(values, shape == 0   ? plist_new_array()
                                      : shape == 1 ? plist_new_dict()
                                                   : plist_new_data("", 0));
      plist_dict_set_item(item, "fixedValue", values);
      rejects([&] { d.decode(); });
    }
    for (double value :
         {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
      Document d;
      auto item = d.add(3, "invalid", "array"), values = plist_new_array();
      plist_array_append_item(values, plist_new_real(value));
      plist_dict_set_item(item, "fixedValue", values);
      rejects([&] { d.decode(); });
    }
    rejects([] { v::decode_local_profile({}); });
    rejects([] { v::decode_local_profile(Bytes(256 * 1024 + 1)); });
    rejects([] { v::decode_local_profile(Bytes{'b', 'a', 'd'}); });
    std::cout << "Local vehicle profile: fixed config, unavailable/expired samples, strict schema "
                 "and bindings passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
