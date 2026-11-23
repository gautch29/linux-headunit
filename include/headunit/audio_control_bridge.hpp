// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/audio_control_file.hpp>
#include <functional>
#include <memory>
#include <span>
#include <thread>

namespace headunit::audio_control {
// Control-thread file adapter; never runs I/O on the audio worker. Exactly one
// opened worker supplies authoritative gain state. A replacement gets a new
// route token, so an old page/file cannot control a replacement phone stream.
class Bridge {
 public:
  using RouteFactory = std::function<std::string()>;
  Bridge(std::string request_path, std::string state_path, unsigned owner_uid,
         RouteFactory route_factory = {});
  // Call with all actual audio worker mailboxes, and real local monotonic time.
  // Local file/provider errors withdraw controls or report a bounded error;
  // they do not stop phone audio/video. Wrong owner/time is a caller error.
  void poll(std::span<const std::shared_ptr<Mailbox>>, std::uint64_t monotonic_ns);
  const State& state() const;
  const std::string& publication_error() const;

 private:
  const std::thread::id owner_;
  std::string request_path_, state_path_;
  unsigned owner_uid_;
  RouteFactory route_factory_;
  std::shared_ptr<Mailbox> active_;
  std::string last_route_;
  State state_;
  std::string publication_error_;
  std::uint64_t last_poll_ns_ = 0;
  void check_owner() const;
};

}  // namespace headunit::audio_control
