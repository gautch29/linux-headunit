// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/vehicle_service.hpp>
#include <memory>

namespace headunit::theme::vehicle {
// Research profiles, not production capability claims. Numeric types were
// observed in Apple's iOS framework. The minimal combined profile passed a
// live phone test; broader compatibility and physical providers remain open.
enum class BenchProfile { empty, climate, settings, combined };
BenchProfile bench_profile(std::string_view);
class Bench {
 public:
  explicit Bench(BenchProfile);
  Bench(const Bench&) = delete;
  Bench& operator=(const Bench&) = delete;
  Service& service() { return *service_; }
  void refresh(std::uint64_t now_ms);
  void disconnect();  // Retain simulated settings, clear connection readings/subscriptions.
  double target_c() const { return target_c_; }
  unsigned fan_level() const { return fan_level_; }
  bool comfort_lighting() const { return comfort_lighting_; }

 private:
  double target_c_ = 21.0;
  unsigned fan_level_ = 40;
  bool temperature_on_ = true, fan_on_ = true, fan_auto_ = false;
  bool comfort_lighting_ = false;
  std::unique_ptr<Service> service_;
  std::map<std::int64_t, std::function<opack::Value()>> readers_;
  std::optional<std::uint64_t> refreshed_;
};
}  // namespace headunit::theme::vehicle
