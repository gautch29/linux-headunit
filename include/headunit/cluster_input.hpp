// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace headunit {
inline constexpr const char* cluster_hid_uuid = "7e73a101";
inline constexpr const char* media_hid_uuid = "7e73a102";
// Which advertised consumer-control device a request presses.
enum class HidKeyTarget { cluster, media };
// Consumer-page transport controls, in descriptor bit order.
enum class MediaButton : std::uint8_t { play_pause = 1, next_track = 2, previous_track = 4 };
// Consumer-page Menu controls, in descriptor bit order. Zero releases all keys.
enum class ClusterButton : std::uint8_t {
  select = 1,
  up = 2,
  down = 4,
  previous = 8,
  next = 16,
  back = 32
};
class ClusterInputControl {
 public:
  using Clock = std::chrono::steady_clock;
  using Sender = std::function<bool(HidKeyTarget, std::uint8_t)>;
  // Opens a phone app (requestUI URL); true when queued.
  using Opener = std::function<bool(const std::string& url)>;
  ClusterInputControl(std::string path, std::string nonce, std::uint64_t generation);
  // Sends one bounded press/release pair. A stale or busy command is never queued
  // for replay. Retirement of the connection retires its advertised HID device.
  void poll(bool ready, const Sender&, Clock::time_point, const Opener& open = {});

 private:
  std::string path_, nonce_;
  std::uint64_t generation_, watermark_ = 0;
  bool pressed_ = false;
  HidKeyTarget pressed_target_ = HidKeyTarget::cluster;
  Clock::time_point release_at_{}, next_read_{};
};
}  // namespace headunit
