// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/route_guidance.hpp>
#include <memory>
#include <span>
#include <thread>

namespace headunit {
inline constexpr std::size_t maximum_navigation_datagram = 1536;
inline constexpr auto navigation_owner_lease = std::chrono::seconds(2);
// The source date is navigation.updated_at. sent_at is only a live-owner lease;
// it does not renew any source observation. Epoch distinguishes process restarts.
struct NavigationPublication {
  std::uint64_t owner_epoch = 0, session_generation = 0;
  Clock::time_point sent_at{};
  NavigationSnapshot navigation;
};
std::vector<std::uint8_t> encode_navigation_datagram(const NavigationPublication&);
std::optional<NavigationPublication> parse_navigation_datagram(std::span<const std::uint8_t>,
                                                               Clock::time_point now);

// Passive, optional local publication. No phone subscription or blocking send.
// The sole publisher holds a private lock and sends no more than four times/s.
class NavigationPublisher {
 public:
  explicit NavigationPublisher(std::string endpoint);
  ~NavigationPublisher();
  NavigationPublisher(const NavigationPublisher&) = delete;
  NavigationPublisher& operator=(const NavigationPublisher&) = delete;
  bool publish(const NavigationSnapshot&, std::uint64_t session_generation, Clock::time_point now);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Owns one private endpoint. A killed consumer's dead socket may be recovered;
// a live endpoint/lock is never replaced. First sample selects the owner thread.
class SocketNavigation {
 public:
  explicit SocketNavigation(std::string endpoint);
  ~SocketNavigation();
  SocketNavigation(const SocketNavigation&) = delete;
  SocketNavigation& operator=(const SocketNavigation&) = delete;
  NavigationSnapshot sample(Clock::time_point now);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
