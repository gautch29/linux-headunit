// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/vehicle.hpp>
#include <headunit/vehicle_observation_receiver.hpp>

#include <string>
#include <cstdint>
#include <thread>

namespace headunit {

// Local Unix input from a future vehicle adapter. This opens no car
// hardware and never sends commands to it. Uses an exact absolute path in an
// owned private directory; an owned dead endpoint can be recovered after a
// killed consumer, while an active lock/live socket is never reclaimed.
// Construction may precede handoff to a renderer. The first sample() selects
// the sole sampling thread; subsequent calls must remain on that thread.
class SocketVehicle final : public VehicleSource {
 public:
  explicit SocketVehicle(std::string path);
  ~SocketVehicle() override;
  SocketVehicle(const SocketVehicle&) = delete;
  SocketVehicle& operator=(const SocketVehicle&) = delete;
  VehicleSample sample(Clock::time_point now) override;

 private:
  std::string path_;
  int socket_ = -1;
  int lock_ = -1;
  std::uint64_t bound_device_ = 0, bound_inode_ = 0;
  bool bound_ = false;
  VehicleSample latest_{};
  std::optional<VehicleObservationReceiver> observations_;
  std::optional<std::thread::id> sample_owner_;
  std::optional<bool> observed_format_;
  std::optional<Clock::time_point> last_now_;
  void cleanup() noexcept;
};

}  // namespace headunit
