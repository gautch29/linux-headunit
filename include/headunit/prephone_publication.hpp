// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/prephone_vehicle_summary.hpp>
#include <array>
#include <memory>

namespace headunit {
namespace ambient {
class SoftwareModel;
}
// Fixed local IPC only. No file/JSON work, acquisition, worker or child.
inline constexpr std::size_t prephone_packet_size = 344;
using PrephoneNonce = std::array<std::byte, 32>;
using PrephonePacket = std::array<std::byte, prephone_packet_size>;
PrephonePacket prephone_packet(const PrephoneVehicleSummary&, const PrephoneNonce&,
                               std::uint64_t sequence, Clock::time_point copied_at);
class PrephonePublisher {
 public:
  explicit PrephonePublisher(std::string path, bool ambient_enabled = false,
                             bool indicators_enabled = false);
  ~PrephonePublisher();
  PrephonePublisher(const PrephonePublisher&) = delete;
  PrephonePublisher& operator=(const PrephonePublisher&) = delete;
  void publish(const VehicleSample&, Clock::time_point) noexcept;
  void retire() noexcept;

 private:
  std::string path_;
  int socket_ = -1;
  bool admitted_ = false;
  PrephoneNonce nonce_{};
  std::uint64_t sequence_ = 0;
  Clock::time_point retry_at_{}, next_copy_{};
  bool ambient_enabled_ = false;
  bool indicators_enabled_ = false;
  std::unique_ptr<ambient::SoftwareModel> ambient_;
  std::array<int, 3> peer_{};
};
// This adapter forwards the SAME acquisition, including local signed sampling.
// It never calls the underlying source twice or changes its returned snapshot.
class PublishingVehicle final : public VehicleSource {
 public:
  PublishingVehicle(std::unique_ptr<VehicleSource>, std::string path, bool ambient_enabled = false,
                    bool indicators_enabled = false);
  VehicleSample sample(Clock::time_point) override;

 private:
  std::unique_ptr<VehicleSource> source_;
  PrephonePublisher publisher_;
};
}  // namespace headunit
