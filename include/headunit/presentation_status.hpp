// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace headunit {
enum class CarplayMode { standard, ultra };
// Content-free facts from an actual main presentation owner. No image, socket,
// phone timestamp, VM or GPU object crosses the thread boundary.
struct MainPresentationStatus {
  bool qualified = false;
  bool healthy = false;
  bool home = false;
  // Times this generation's main window was recreated after a native Home
  // withdrawal. Its video was drained meanwhile, so each return needs a
  // fresh keyframe from the phone.
  std::uint64_t surface_restorations = 0;
};
std::uint64_t presentation_monotonic_ns();  // Genuine CLOCK_MONOTONIC, not PTS.

class MainPresentationSnapshot {
 public:
  // Each new UI worker resets all prior presentation qualification. A retired
  // generation cannot publish again; only begin() creates a new owner. An older
  // worker cannot update or clear its replacement's generation.
  std::uint64_t begin();
  void observe(std::uint64_t generation, bool scene_visible, bool home, bool healthy,
               std::uint64_t surface_restorations = 0);
  void retire(std::uint64_t generation) noexcept;
  MainPresentationStatus sample() const;

 private:
  mutable std::mutex mutex_;
  std::uint64_t generation_ = 0, observed_ns_ = 0;
  bool active_ = false, presented_ = false;
  MainPresentationStatus value_;
};

enum class PresentationState { disconnected, starting, ready, home, fault, stopped };
// One receiver control owner publishes an expiring private observation. This
// is eligibility to return to a retained session, not physical scanout proof.
// The manager's fresh random nonce binds it to exactly one owned child launch.
class PresentationStatusPublisher {
 public:
  PresentationStatusPublisher(std::string path, std::string nonce);
  ~PresentationStatusPublisher();
  PresentationStatusPublisher(const PresentationStatusPublisher&) = delete;
  PresentationStatusPublisher& operator=(const PresentationStatusPublisher&) = delete;
  // Changes publish immediately; otherwise a real-clock heartbeat is written
  // at most every 250ms. File operations run only on this control owner.
  // phone_home_requests counts the phone releasing mainScreen to the car (its
  // car icon) during this connection; the native page opens Home on change.
  void update(PresentationState, std::uint64_t connection_generation, bool return_ready,
              std::optional<CarplayMode> negotiated_mode = {},
              std::uint64_t phone_home_requests = 0);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
