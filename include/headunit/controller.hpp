// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/vehicle.hpp>

#include <chrono>
#include <optional>

namespace headunit {

enum class MainSurface { standalone, carplay };
enum class ClusterSurface { backup, carplay };
enum class PhoneMode { disconnected, standard, ultra };

struct PhonePresentation {
  PhoneMode mode = PhoneMode::disconnected;
  bool authenticated = false;
  bool theme_verified = false;
  std::optional<Clock::time_point> main_presented_at;
  std::optional<Clock::time_point> cluster_presented_at;
};

struct DisplayDecision {
  MainSurface main = MainSurface::standalone;
  ClusterSurface cluster = ClusterSurface::backup;
  bool operator==(const DisplayDecision&) const = default;
};

// The backup cluster exists before a phone is seen. The controller switches
// a display only after its CarPlay view has actually been presented. Standard
// CarPlay, or Ultra with a stale cluster, can keep the main picture while the
// backup cluster stays in charge of instruments.
class Controller {
 public:
  explicit Controller(Clock::duration maximum_frame_age = std::chrono::seconds(1));
  DisplayDecision update(const PhonePresentation& phone, Clock::time_point now);
  DisplayDecision current() const { return current_; }

 private:
  Clock::duration maximum_frame_age_;
  DisplayDecision current_{};
};

}  // namespace headunit
