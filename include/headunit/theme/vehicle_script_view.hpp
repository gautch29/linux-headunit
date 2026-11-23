// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/vehicle_service.hpp>
#include <array>
#include <functional>
#include <memory>

namespace headunit::theme::vehicle {
// Receiver integration supplies the semantic accessory/service/characteristic
// path for a real IID. Never derive an IID from an Apple type identifier or
// invent a signal just because a signed asset mentions its name.
struct ScriptBinding {
  std::int64_t iid;
  std::array<std::string, 3> path;
  std::optional<std::int32_t> format;  // Optional script ABI enum, not a wire string.
  // A receiver-owned grouping key for repeated services, e.g. two buttons or
  // display zones. Present keys produce a service array; absent keeps the
  // existing single-service dictionary. This name is never a phone type ID.
  std::optional<std::string> instance = {};
};
struct ScriptRead {
  std::map<std::int64_t, opack::Value> values;
  std::map<std::int64_t, std::int32_t> errors;
};
class ScriptView {
 public:
  using Dismissals = std::map<std::int64_t, std::function<void(std::uint64_t)>>;
  ScriptView(std::shared_ptr<ReadOnlyService>, std::vector<ScriptBinding> = {}, Dismissals = {});
  opack::Value configuration(std::uint64_t now_ms) const;
  void subscribe(std::span<const std::int64_t>);  // Replace this display's set.
  ScriptRead read(std::span<const std::int64_t>, std::uint64_t now_ms) const;
  // Changed repeated instances, uiControl.uiState and driveState.transmissionStatus
  // include their complete subscribed state. Other services retain incremental
  // IID updates; neither path refreshes source age.
  ScriptRead updates(std::uint64_t now_ms);
  ScriptRead reject_writes(std::span<const std::int64_t>) const;
  // Only an explicitly bound RequestContent On=false is accepted. This is
  // dismissal of host intent, never a vehicle/actuator write.
  ScriptRead dismiss(const std::map<std::int64_t, opack::Value>&, std::uint64_t now_ms) const;

 private:
  struct Entry {
    ScriptBinding binding;
    opack::Value metadata;
  };
  std::shared_ptr<ReadOnlyService> source_;
  std::map<std::int64_t, Entry> entries_;
  std::vector<std::int64_t> subscribed_;
  std::map<std::int64_t, ReadOnlyService::Reading> delivered_;
  Dismissals dismissals_;
  void validate(std::span<const std::int64_t>) const;
  static void append(ScriptRead&, std::int64_t, const ReadOnlyService::Reading&);
};
}  // namespace headunit::theme::vehicle
