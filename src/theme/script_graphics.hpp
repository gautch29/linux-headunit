// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/scene.hpp>
#include <vector>
#include <headunit/theme/video_stream.hpp>
#include <functional>
#include <memory>
#include <string_view>
struct lua_State;
namespace headunit::theme::script {
using Bytes = std::vector<std::uint8_t>;
class Graphics {
 public:
  Graphics(std::string display, std::function<std::string(std::string_view)> asset_reader);
  ~Graphics();
  void install(lua_State*, int cp_table);
  std::shared_ptr<graphics::Layer> root() const;
  std::shared_ptr<video::Stream> add_video_input(video::Input);
  int enumerate_inputs(lua_State*);
  void bind_custom_image_provider(std::function<std::optional<Bytes>(std::string_view, int)>);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::script
