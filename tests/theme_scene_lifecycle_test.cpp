// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/scene_lifecycle.hpp>
#include <plist/plist.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace headunit::theme;
using script::Runtime;
using script::SceneLifecycle;
void check(bool ok) {
  if (!ok) throw std::runtime_error("scene lifecycle check failed");
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error("invalid scene input accepted");
}
struct Profile {
  plist_t root = plist_new_dict(), signals = plist_new_array();
  Profile() {
    plist_dict_set_item(root, "schemaVersion", plist_new_uint(1));
    plist_dict_set_item(root, "mode", plist_new_string("local-ui-bindings"));
    plist_dict_set_item(root, "signals", signals);
    for (unsigned index = 0; index < 2; ++index) {
      auto node = plist_new_dict(), path = plist_new_array();
      for (const auto* name : {"uiControl", "uiState", index ? "uiSceneOptions" : "uiSceneState"})
        plist_array_append_item(path, plist_new_string(name));
      plist_dict_set_item(node, "path", path);
      plist_dict_set_item(node, "iid", plist_new_uint(index + 3));
      plist_dict_set_item(node, "kind", plist_new_string(index ? "array" : "integer"));
      plist_dict_set_item(node, "maxAgeMs", plist_new_uint(250));
      plist_dict_set_item(node, "sessionField",
                          plist_new_string(index ? "scene_options" : "scene_state"));
      plist_array_append_item(signals, node);
    }
  }
  ~Profile() { plist_free(root); }
  vehicle::LocalProfile decode() {
    char* raw = nullptr;
    std::uint32_t size = 0;
    plist_to_bin(root, &raw, &size);
    std::unique_ptr<char, decltype(&std::free)> bytes(raw, std::free);
    return vehicle::decode_local_profile({reinterpret_cast<const std::uint8_t*>(raw), size});
  }
};
auto readings(const vehicle::LocalProfile& p, std::uint64_t at) {
  return p.source->snapshot(std::array<std::int64_t, 2>{3, 4}, at);
}
bool state(const vehicle::LocalProfile& p, std::uint64_t at, std::int64_t expected) {
  return readings(p, at).at(3).value == opack::Value{expected};
}
void startup() {
  Profile document;
  auto p = document.decode();
  check(p.scene_inputs.has_value() && p.vehicle_inputs.empty());
  SceneLifecycle scene(p, SceneLifecycle::Launch::startup);
  scene.pump(100, Runtime::ScenePhase::unavailable, false);
  check(state(p, 100, 1));
  scene.pump(110, Runtime::ScenePhase::startup, true);  // No witnessed animation.
  check(!scene.driving());
  scene.pump(120, Runtime::ScenePhase::logos, true);
  scene.pump(6100, Runtime::ScenePhase::startup, false);  // Clock/visibility lost.
  check(!scene.driving() && state(p, 6100, 1));
  scene.pump(6110, Runtime::ScenePhase::startup, true);
  check(scene.driving() && state(p, 6110, 2));
  scene.pump(6120, Runtime::ScenePhase::logos, true);  // Never replay.
  check(state(p, 6120, 2));
  rejects([&] { scene.pump(6119, Runtime::ScenePhase::drive, true); });
  scene.retire(6120);
  check(std::holds_alternative<std::monostate>(readings(p, 6120).at(3).value->data));
  check(std::holds_alternative<std::monostate>(readings(p, 6120).at(4).value->data));
  rejects([&] { scene.pump(6130, Runtime::ScenePhase::drive, true); });
}
void first_pairing_and_resume() {
  Profile document;
  auto p = document.decode();
  vehicle::ScriptView view(p.source, p.bindings);
  view.subscribe(std::array<std::int64_t, 2>{3, 4});
  SceneLifecycle scene(p, SceneLifecycle::Launch::first_pairing);
  scene.pump(100, Runtime::ScenePhase::unavailable, false);
  check(state(p, 100, 2));
  const opack::Value allowed{opack::Array{opack::Value{std::string("AllowPairingAnimation")}}};
  check(readings(p, 100).at(4).value == allowed);
  check(view.updates(100).values.size() == 2);
  scene.pump(110, Runtime::ScenePhase::logos, true);
  scene.pump(6100, Runtime::ScenePhase::drive, true);
  check(scene.driving() && readings(p, 6100).at(4).value == opack::Value{opack::Array{}});
  const auto update = view.updates(6100);
  check(update.values.size() == 2 && update.values.at(3) == opack::Value{std::int64_t(2)});
  check(view.updates(6100).values.empty());
  scene.retire(6110);
  check(std::holds_alternative<std::monostate>(readings(p, 6110).at(3).value->data));
  check(std::holds_alternative<std::monostate>(readings(p, 6110).at(4).value->data));
  auto next = document.decode();
  SceneLifecycle resume(next, SceneLifecycle::Launch::resume);
  resume.pump(7000, Runtime::ScenePhase::unavailable, false);
  check(state(next, 7000, 2) && readings(next, 7000).at(4).value == opack::Value{opack::Array{}});
  check(std::holds_alternative<std::monostate>(readings(next, 7250).at(3).value->data));
}
void failure_and_admission() {
  Profile document;
  auto p = document.decode();
  rejects(
      [&] { SceneLifecycle missing(vehicle::LocalProfile{}, SceneLifecycle::Launch::startup); });
  SceneLifecycle scene(p, SceneLifecycle::Launch::startup);
  scene.pump(100, Runtime::ScenePhase::logos, true);
  rejects([&] { scene.pump(15100, Runtime::ScenePhase::logos, true); });
  check(!scene.driving());
  auto node = plist_array_get_item(document.signals, 0);
  plist_dict_set_item(node, "fixedValue", plist_new_uint(1));
  rejects([&] { document.decode(); });
  plist_dict_remove_item(node, "fixedValue");
  plist_dict_set_item(node, "sessionField", plist_new_string("pairing"));
  rejects([&] { document.decode(); });
  plist_dict_set_item(node, "sessionField", plist_new_string("scene_state"));
  plist_array_remove_item(document.signals, 1);
  rejects([&] { document.decode(); });
}
int main() {
  try {
    startup();
    first_pairing_and_resume();
    failure_and_admission();
    std::cout << "session scene lifecycle passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
