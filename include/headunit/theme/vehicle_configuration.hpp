// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/vehicle_service.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace headunit::theme::vehicle {
// Local, semantic capability files. These are not Apple's wire dictionaries.
struct ConfiguredSignal {
  SignalPolicy policy;
  std::string key;  // Stable provider key; IIDs are local to a connection.
  opack::Value initial;
  bool writable = true;
  double step = 1;
  std::vector<bool> disabled_entries;
  bool dismiss_only = false;
};
// Simulation relationships. A hardware provider supplies these related states
// itself; the receiver never issues extra physical writes from these hints.
struct LevelSwitch {
  std::string level, on;
  opack::Value nonzero;
  bool has_zero = true;
};
struct CameraCapability {
  std::string stream_identifier;
  std::string binding;
  std::uint64_t max_age_ms = 0;
  std::int64_t input_stream_on_iid = 0;
  std::int64_t camera_general_on_iid = 0;
};
struct ContentPage {
  std::string identifier, url, panel, zone;
};
struct ContentRequestCapability {
  std::string control_file, on_binding, url_binding;
  std::vector<ContentPage> pages;
};
struct Configuration {
  opack::Value profile = empty_configuration();
  std::vector<ConfiguredSignal> signals;
  std::vector<SignalPolicy> fixed_policies;
  std::map<std::int64_t, opack::Value> fixed;
  std::vector<LevelSwitch> level_switches;
  // Fixed for the session. A provider may use this descriptor to mirror one
  // confirmed camera state into the two independent phone characteristics.
  std::optional<CameraCapability> camera;
  // Session-fixed Remote UI pages. These are declarations, not presentation
  // confirmations, local video inputs, or evidence of fitted physical sensors.
  std::vector<ContentPage> content_pages;
  std::optional<ContentRequestCapability> content_request;
  bool experimental = false;
};
// Each nonempty argument is a complete JSON object. Empty means no capability.
Configuration parse_configuration(std::string_view climate_json, std::string_view settings_json,
                                  std::string_view camera_json = {});
Configuration load_configuration(const std::optional<std::filesystem::path>& climate,
                                 const std::optional<std::filesystem::path>& settings,
                                 const std::optional<std::filesystem::path>& camera = std::nullopt);

struct Sample {
  opack::Value value;
  std::uint64_t sampled_ms;
};
class Provider {
 public:
  virtual ~Provider() = default;
  // Called on the receiver thread: implementations must be bounded/nonblocking.
  virtual std::optional<Sample> read(std::string_view key, std::uint64_t now_ms) = 0;
  // Return confirmed applied state, or throw if unavailable/rejected. Never
  // acknowledge an asynchronous hardware request as if already applied.
  // Defaults preserve every synchronous/read-only provider. Only an attached
  // actual completion owner may opt in; starting is bounded and nonblocking.
  virtual bool supports_confirmation(std::string_view) const { return false; }
  virtual std::optional<PendingWrite> start_confirmation(std::string_view, const opack::Value&,
                                                         std::uint64_t, std::uint64_t) {
    return {};
  }
  virtual opack::Value apply(std::string_view key, const opack::Value& requested,
                             std::uint64_t now_ms) = 0;
};
std::shared_ptr<Provider> simulated_provider(const Configuration&);
class ConfiguredModel {
 public:
  ConfiguredModel(Configuration, std::shared_ptr<Provider>);
  ConfiguredModel(const ConfiguredModel&) = delete;
  ConfiguredModel& operator=(const ConfiguredModel&) = delete;
  Service& service() { return *service_; }
  void refresh(std::uint64_t now_ms);
  void disconnect();

 private:
  Configuration configuration_;
  std::shared_ptr<Provider> provider_;
  std::unique_ptr<Service> service_;
  // Last accepted acquisition/confirmation survives withdrawal. An equal
  // timestamp is an immutable observation, never a new state or fresh sample.
  std::map<std::int64_t, Sample> observed_;
  std::optional<std::uint64_t> refreshed_;
  std::uint64_t now_ = 0;
};
}  // namespace headunit::theme::vehicle
