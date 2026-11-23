// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/gpu_compositor.hpp>
namespace headunit::theme::graphics {
// The EGL owner keeps each texture and decoder lease alive through the draw's
// completion fence and until Ganesh releases its borrowed image.
struct SkiaNativeTexture {
  std::shared_ptr<const video::NativeFrame> frame;
  unsigned texture = 0;
  bool released = false;
};
struct SkiaFrameImages;  // Immutable borrowed images retained by one GPU submission.
struct SkiaSceneConfiguration;
// Internal adapter. All methods/destruction require the owner's current EGL
// context. Rendering does not report presentation or advance video timestamps.
class SkiaScene {
 public:
  using Configuration = std::shared_ptr<const SkiaSceneConfiguration>;
  static Configuration capture_configuration();  // Capture before lazy context creation.
  SkiaScene(unsigned width, unsigned height, unsigned framebuffer, Configuration = {});
  ~SkiaScene();
  static void validate(const std::shared_ptr<const Layer>&, unsigned width, unsigned height);
  void draw(const std::shared_ptr<const Layer>&, std::span<SkiaNativeTexture*> native = {});
  void release_native();  //Call only after the owner's GPU completion fence.
  std::shared_ptr<SkiaFrameImages> retain_native();
  void release_native(std::shared_ptr<SkiaFrameImages>&);  // Its own fence must signal first.
  void abandon();
  GpuSceneCacheStats cache_stats() const;
  static Image raster_probe(const std::shared_ptr<const Layer>&, unsigned width, unsigned height);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::theme::graphics
