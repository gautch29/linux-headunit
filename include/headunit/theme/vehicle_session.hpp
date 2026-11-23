// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/airplay_session_setup.hpp>
#include <array>
#include <headunit/theme/vehicle_bench.hpp>
#include <headunit/theme/vehicle_configuration.hpp>
#include <headunit/theme/vehicle_transport.hpp>

namespace headunit::theme::vehicle {
// One authenticated AirPlay session, two priority sockets and one shared model.
// Constructed only when a capability model or legacy fixture is enabled.
class Session {
 public:
  explicit Session(BenchProfile profile) : bench_(std::make_unique<Bench>(profile)) {}
  Session(Configuration, std::shared_ptr<Provider>, Transport::WriteObserver observer = {});
  void attach(AirplaySessionSetup&);  // Once, before initial SETUP.
  void pump(AirplaySessionSetup&, std::uint64_t now_ms);
  const std::string& failure_reason() const { return failure_; }
  struct Status {
    unsigned channels = 0;
    std::size_t received = 0, writes = 0;
    Transport::RequestCounts requests;
    std::optional<std::array<ResourceTrace::Counts, 2>> camera_resources;
    double simulated_target_c = 21;
    unsigned simulated_fan_level = 40;
    bool simulated_comfort_lighting = false;
    bool operator==(const Status&) const = default;
  };
  Status status() const;
  const Coverage* coverage() const noexcept { return coverage_.get(); }

 private:
  struct Port {
    std::uint64_t id = 0;
    std::unique_ptr<Transport> transport;
    std::optional<std::uint64_t> blocked_since;
  };
  std::unique_ptr<Bench> bench_;
  std::unique_ptr<ConfiguredModel> model_;
  Transport::WriteObserver observer_;
  std::shared_ptr<ResourceTrace> camera_trace_;
  std::shared_ptr<Coverage> coverage_;
  Service& service() { return model_ ? model_->service() : bench_->service(); }
  std::array<Port, 2> ports_;
  AirplaySessionSetup* owner_ = nullptr;
  std::optional<Bytes> notification_;
  std::string failure_;
  bool had_channels_ = false;
};
}  // namespace headunit::theme::vehicle
