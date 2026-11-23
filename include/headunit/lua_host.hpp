// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include "headunit/theme/scene.hpp"
#include "headunit/theme/video_stream.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace headunit {
// A single-threaded, bounded host for verified theme source. The resource
// callback must read an AssetBundle or explicitly synthetic test resources.
// This is a CP subset; it does not advertise bridge 1.4 or graphics readiness.
class LuaHost {
 public:
  using Bytes = std::vector<std::uint8_t>;
  using ResourceReader = std::function<Bytes(const std::string&)>;
  struct OutputStream {
    std::string identifier;
    std::uint32_t width = 0, height = 0;
    double frame_duration = 0;
  };
  explicit LuaHost(ResourceReader reader, std::size_t memory_limit = 64 * 1024 * 1024,
                   std::vector<OutputStream> outputs = {});
  ~LuaHost();
  LuaHost(const LuaHost&) = delete;
  LuaHost& operator=(const LuaHost&) = delete;
  void execute(std::string_view source, std::string_view name = "host");
  void tick(double monotonic_seconds);
  void connected(bool value);
  bool receive(std::span<const std::uint8_t> message);
  std::vector<Bytes> take_outbound();
  std::size_t active_tasks() const;
  std::size_t memory_used() const;
  std::size_t reset_count() const;
  std::vector<std::string> take_logs();
  std::shared_ptr<theme::graphics::Layer> scene_root() const;
  std::shared_ptr<theme::video::Stream> add_video_input(theme::video::Input input);
  void bind_custom_image_provider(
      std::function<std::optional<Bytes>(std::string_view, int)> provider);
  theme::graphics::Image render_reference();
  // Feed confirmed presentation receipts; drawing/ticking does not advance PTS.
  void presented(double pts);
  bool faulted() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit
