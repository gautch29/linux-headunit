// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace headunit::theme {
// User-selected trade-off between touch response and video smoothness, written
// by the native settings page. Each mode is a fixed, bounded presentation
// policy; the release's installed-UI values apply until a valid choice exists.
enum class DisplayResponse { smooth, balanced, fast };

struct DisplayResponsePolicy {
  std::uint64_t presentation_advance_ns;
  unsigned main_queue_depth;
};

DisplayResponsePolicy display_response_policy(DisplayResponse);
std::string_view display_response_name(DisplayResponse);
// Accepts exactly one mode name with an optional trailing newline.
std::optional<DisplayResponse> parse_display_response(std::string_view content);

// Polls one preference file on the display owner thread. A missing, unsafe or
// malformed file leaves the previous choice in place; it never resets policy.
class DisplayResponseFile {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto poll_interval = std::chrono::milliseconds(500);
  explicit DisplayResponseFile(std::string path);
  // Returns a mode only when a valid choice differs from the last one returned.
  std::optional<DisplayResponse> poll(Clock::time_point now);

 private:
  struct Stamp {
    std::uint64_t device = 0, inode = 0, size = 0, seconds = 0, nanoseconds = 0;
    bool operator==(const Stamp&) const = default;
  };
  std::string path_;
  Clock::time_point next_poll_{};
  std::optional<Stamp> stamp_;
  std::optional<DisplayResponse> current_;
};
}  // namespace headunit::theme
