// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/airplay_clock.hpp>
#include <headunit/airplay_info.hpp>
#include <headunit/airplay_video_stream.hpp>
#include <headunit/route_guidance.hpp>
#include <headunit/touch_input.hpp>
#include <headunit/main_preview_options.hpp>
#include <headunit/presentation_status.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <vector>
#include <functional>

namespace headunit {

// Owns optional Standard CarPlay previews and bounded, private video captures.
// Ultra's native renderer can consume the same packets independently; this
// diagnostic output must never be required for the phone's transport to work.
class VideoOutput {
 public:
  struct Options {
    std::string capture_directory;
    unsigned connection_number = 0;
    std::string preview_sink;
    std::string preview_decoder;
    AirplayDisplaySize main_display{};
    AirplayDisplaySize cluster_display{};
    std::string standard_cluster_wayland;
    MainPreviewOptions main_preview{};
    bool defer_preview = false;  // Wait for the actual phone session mode.
  };

  explicit VideoOutput(Options options);
  ~VideoOutput();
  VideoOutput(const VideoOutput&) = delete;
  VideoOutput& operator=(const VideoOutput&) = delete;

  unsigned drain_touches(const std::function<bool(TouchInput)>& sender);
  void append_input_poll_descriptor(std::vector<pollfd>& descriptors) const;
  void set_navigation(std::shared_ptr<RouteGuidance> source);
  void select_mode(std::optional<CarplayMode>);
  void update_clock(const std::optional<AirplayClockSample>& timing);
  MainPresentationStatus main_presentation_status();
  void record(std::uint64_t stream_type, const AirplayVideoPacket& packet,
              const std::optional<AirplayClockSample>& timing);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace headunit
