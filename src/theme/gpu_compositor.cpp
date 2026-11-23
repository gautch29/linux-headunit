// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_compositor.hpp>
#ifdef HEADUNIT_THEME_HAVE_SKIA
#include "skia_scene.hpp"
#include <headunit/theme/video_stream.hpp>
#endif
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>
#include <utility>
#ifdef HEADUNIT_THEME_GPU_WAYLAND
#include "wayland_window.hpp"
#endif

namespace headunit::theme::graphics {
namespace {
void check(bool ok, const char* reason) {
  if (!ok) throw std::runtime_error(reason);
}
std::uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
bool extension(const char* list, const char* wanted) {
  if (!list) return false;
  const std::string_view text(list), name(wanted);
  std::size_t begin = 0;
  while (begin < text.size()) {
    auto end = text.find(' ', begin);
    if (end == text.npos) end = text.size();
    if (text.substr(begin, end - begin) == name) return true;
    begin = end + 1;
  }
  return false;
}
GLuint program(const char* fragment, const char* vertex_source = nullptr,
               const char* second_attribute = "uv") {
  const auto compile = [](GLenum type, const char* source) {
    const auto object = glCreateShader(type);
    check(object, "GPU shader allocation failed");
    glShaderSource(object, 1, &source, nullptr);
    glCompileShader(object);
    GLint ok = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      glDeleteShader(object);
      throw std::runtime_error("GPU shader compilation failed");
    }
    return object;
  };
  const auto vertex = compile(
      GL_VERTEX_SHADER, vertex_source ? vertex_source
                                      : "attribute vec2 pos;attribute vec2 uv;varying vec2 "
                                        "tex;void main(){tex=uv;gl_Position=vec4(pos,0.,1.);}");
  GLuint frag = 0, result = 0;
  try {
    frag = compile(GL_FRAGMENT_SHADER, fragment);
    result = glCreateProgram();
    check(result, "GPU program allocation failed");
    glAttachShader(result, vertex);
    glAttachShader(result, frag);
    glBindAttribLocation(result, 0, "pos");
    glBindAttribLocation(result, 1, second_attribute);
    glLinkProgram(result);
    GLint ok = 0;
    glGetProgramiv(result, GL_LINK_STATUS, &ok);
    check(ok, "GPU program link failed");
  } catch (...) {
    if (result) glDeleteProgram(result);
    if (frag) glDeleteShader(frag);
    glDeleteShader(vertex);
    throw;
  }
  glDeleteShader(vertex);
  glDeleteShader(frag);
  return result;
}
std::vector<EGLint> dma_attributes(const video::NativeFrame& f) {
  check(f.lifetime && f.plane_count > 0 && f.plane_count <= 4 && f.storage_width &&
            f.storage_height && f.storage_width <= 8192 && f.storage_height <= 8192 &&
            f.metadata.width > 0 && f.metadata.height > 0 &&
            std::uint64_t(f.crop_x) + f.metadata.width <= f.storage_width &&
            std::uint64_t(f.crop_y) + f.metadata.height <= f.storage_height &&
            unsigned(f.matrix) <= unsigned(video::YuvMatrix::bt2020),
        "invalid GPU input frame");
  std::vector<EGLint> attrs{EGL_WIDTH,
                            EGLint(f.storage_width),
                            EGL_HEIGHT,
                            EGLint(f.storage_height),
                            EGL_LINUX_DRM_FOURCC_EXT,
                            EGLint(f.fourcc)};
  const std::array<EGLint, 4> fds{EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                  EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
  const std::array<EGLint, 4> offsets{EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                      EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
  const std::array<EGLint, 4> pitches{EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                      EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
  const std::array<EGLint, 4> lows{
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
      EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
  const std::array<EGLint, 4> highs{
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
      EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
  for (unsigned p = 0; p < f.plane_count; ++p) {
    check(f.planes[p].fd >= 0 && f.planes[p].stride > 0 && f.planes[p].stride <= 0x7fffffff &&
              f.planes[p].offset <= 0x7fffffff,
          "invalid GPU plane descriptor");
    attrs.insert(attrs.end(),
                 {fds[p], f.planes[p].fd, offsets[p], EGLint(f.planes[p].offset), pitches[p],
                  EGLint(f.planes[p].stride), lows[p], EGLint(f.modifier & 0xffffffff), highs[p],
                  EGLint(f.modifier >> 32)});
  }
  const auto matrix = f.matrix == video::YuvMatrix::bt601   ? EGL_ITU_REC601_EXT
                      : f.matrix == video::YuvMatrix::bt709 ? EGL_ITU_REC709_EXT
                                                            : EGL_ITU_REC2020_EXT;
  attrs.insert(
      attrs.end(),
      {EGL_YUV_COLOR_SPACE_HINT_EXT, matrix, EGL_SAMPLE_RANGE_HINT_EXT,
       f.full_range ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT,
       EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT,
       f.chroma_x_cosited ? EGL_YUV_CHROMA_SITING_0_EXT : EGL_YUV_CHROMA_SITING_0_5_EXT,
       EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT,
       f.chroma_y_cosited ? EGL_YUV_CHROMA_SITING_0_EXT : EGL_YUV_CHROMA_SITING_0_5_EXT, EGL_NONE});
  return attrs;
}
Image read_pixels(std::uint32_t width, std::uint32_t height) {
  std::vector<std::uint8_t> rgba(std::size_t(width) * height * 4);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  check(glGetError() == GL_NO_ERROR, "GPU verification readback failed");
  Image result{width, height, std::vector<std::uint32_t>(std::size_t(width) * height)};
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto at = (std::size_t(height - 1 - y) * width + x) * 4;
      result.pixels[std::size_t(y) * width + x] = (std::uint32_t(rgba[at + 3]) << 24) |
                                                  (std::uint32_t(rgba[at]) << 16) |
                                                  (std::uint32_t(rgba[at + 1]) << 8) | rgba[at + 2];
    }
  return result;
}
}  // namespace
struct GpuCompositor::Impl {
  int fd = -1;
  gbm_device* device = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface window_surface = EGL_NO_SURFACE;
  bool readback_presentation = false, defer_scene_fence = false, opaque_background = false;
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  std::unique_ptr<WaylandWindow> window;
#endif
  EGLImageKHR imported = EGL_NO_IMAGE_KHR;
  EGLSyncKHR fence = EGL_NO_SYNC_KHR;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture = nullptr;
  PFNEGLCREATESYNCKHRPROC create_sync = nullptr;
  PFNEGLDESTROYSYNCKHRPROC destroy_sync = nullptr;
  PFNEGLCLIENTWAITSYNCKHRPROC wait_sync = nullptr;
  GLuint framebuffer = 0, target = 0, external = 0, vertices = 0, video_program = 0,
         quad_program = 0, overlay_program = 0, copy_program = 0;
  std::array<GLuint, 128> conical_programs{};  //32 stop counts, coverage and mask modes.
  struct MaskEntry {
    std::weak_ptr<const AlphaMask> source;
    GLuint texture;
    std::size_t bytes;
    std::uint64_t used;
  };
  std::unordered_map<const AlphaMask*, MaskEntry> masks;
  std::uint64_t mask_serial = 0;
  GpuMaskStats mask_counts;
  std::vector<float> overlay_vertices;
  std::uint32_t width, height;
  std::string renderer;
  bool faulted = false, has_frame = false;
  std::shared_ptr<const video::NativeFrame> lease;
#ifdef HEADUNIT_THEME_HAVE_SKIA
  std::unique_ptr<SkiaScene> scene;
  const SkiaScene::Configuration scene_configuration = SkiaScene::capture_configuration();
  struct SceneImport {
    SkiaNativeTexture view;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
  };
  std::vector<std::unique_ptr<SceneImport>> scene_imports;
  struct DeferredScene {
    std::vector<std::unique_ptr<SceneImport>> imports;
    std::shared_ptr<SkiaFrameImages> images;
    EGLSyncKHR completion = EGL_NO_SYNC_KHR;
    std::uint64_t deadline_ns = 0;
  };
  std::deque<DeferredScene> deferred_scenes;
  unsigned max_pending_gpu_scenes = 1, max_deferred_seen = 0;
  void clear_scene_imports(std::vector<std::unique_ptr<SceneImport>>& imports, bool bound) {
    for (auto& entry : imports) {
      if (bound && entry->view.texture) glDeleteTextures(1, &entry->view.texture);
      if (entry->image != EGL_NO_IMAGE_KHR) destroy_image(display, entry->image);
    }
    imports.clear();
  }
  void clear_scene_imports(bool bound) { clear_scene_imports(scene_imports, bound); }
  void clear_deferred_scenes(bool bound) {
    for (auto& submission : deferred_scenes) {
      if (bound && scene)
        scene->release_native(submission.images);
      else
        submission.images.reset();  // Context was abandoned/destroyed, or already released.
      clear_scene_imports(submission.imports, bound);
      if (submission.completion != EGL_NO_SYNC_KHR) destroy_sync(display, submission.completion);
    }
    deferred_scenes.clear();
  }
  bool finish_oldest_scene(bool wait) {
    if (deferred_scenes.empty()) return true;
    try {
      current();
      auto& submission = deferred_scenes.front();
      const auto at = now();
      const auto remaining = wait && at < submission.deadline_ns ? submission.deadline_ns - at : 0;
      const auto result =
          wait_sync(display, submission.completion, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, remaining);
      if (result == EGL_TIMEOUT_EXPIRED_KHR && !wait) {
        check(at < submission.deadline_ns, "deferred GPU scene completion deadline exceeded");
        return false;
      }
      check(result == EGL_CONDITION_SATISFIED_KHR,
            "deferred GPU scene completion deadline exceeded");
      scene->release_native(submission.images);
      for (const auto& entry : submission.imports)
        check(entry->view.released, "Skia retained a completed native texture");
      clear_scene_imports(submission.imports, true);
      destroy_sync(display, submission.completion);
      deferred_scenes.pop_front();
      return true;
    } catch (...) {
      faulted = true;
      throw;
    }
  }
  bool finish_deferred_scene(bool wait) {
    while (!deferred_scenes.empty())
      if (!finish_oldest_scene(wait)) return false;
    return true;
  }
#endif
  Impl(std::uint32_t w, std::uint32_t h) : width(w), height(h) {}
  ~Impl() {
    if (context != EGL_NO_CONTEXT) {
      const bool bound = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
      // A timed-out operation keeps its producer lease. Driver teardown must
      // be supervised by the eventual process watchdog; never release/reuse a
      // buffer while pending GPU work can still read it.
      if (bound) {
        if (lease) glFinish();
#ifdef HEADUNIT_THEME_HAVE_SKIA
        if (!scene_imports.empty() || !deferred_scenes.empty()) glFinish();
#endif
        clear_import();
#ifdef HEADUNIT_THEME_HAVE_SKIA
        if (scene) {
          scene->release_native();
          for (auto& submission : deferred_scenes)
            scene->release_native(submission.images);
        }
        // A borrowed texture's release callback writes SceneImport::view.released.
        // Ganesh may still own that texture (the faulted paths above never saw
        // released=true), so destroy the context before freeing any import.
        scene.reset();  //Release Ganesh resources while the owning context is current.
        clear_deferred_scenes(true);
        clear_scene_imports(true);
#endif
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (target) glDeleteTextures(1, &target);
        if (vertices) glDeleteBuffers(1, &vertices);
        if (video_program) glDeleteProgram(video_program);
        if (quad_program) glDeleteProgram(quad_program);
        if (overlay_program) glDeleteProgram(overlay_program);
        if (copy_program) glDeleteProgram(copy_program);
        for (const auto p : conical_programs)
          if (p) glDeleteProgram(p);
        for (const auto& [key, entry] : masks) {
          (void)key;
          glDeleteTextures(1, &entry.texture);
        }
      }
#ifdef HEADUNIT_THEME_HAVE_SKIA
      if (!bound && scene) {
        scene->abandon();
        scene.reset();
      }
#endif
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(display, context);
      if (!bound) {
        if (fence != EGL_NO_SYNC_KHR) destroy_sync(display, fence);
        if (imported != EGL_NO_IMAGE_KHR) destroy_image(display, imported);
        lease.reset();
#ifdef HEADUNIT_THEME_HAVE_SKIA
        clear_scene_imports(false);  //Context destruction has ended all GPU reads.
        clear_deferred_scenes(false);
#endif
      }
    }
    if (window_surface != EGL_NO_SURFACE) eglDestroySurface(display, window_surface);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    if (device) gbm_device_destroy(device);
    if (fd >= 0) ::close(fd);
  }
  void clear_import() {
    if (fence != EGL_NO_SYNC_KHR) {
      destroy_sync(display, fence);
      fence = EGL_NO_SYNC_KHR;
    }
    if (external) {
      glDeleteTextures(1, &external);
      external = 0;
    }
    if (imported != EGL_NO_IMAGE_KHR) {
      destroy_image(display, imported);
      imported = EGL_NO_IMAGE_KHR;
    }
    lease.reset();
  }
  void current() {
    check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
          "GPU context binding failed");
  }
  void start(const std::string& node, std::optional<WaylandOptions> wayland = {}) {
    check(width > 0 && height > 0 && width <= 4096 && height <= 2160,
          "invalid GPU target dimensions");
    if (wayland) {
#ifdef HEADUNIT_THEME_GPU_WAYLAND
      check(wayland->max_pending_presentations >= 1 && wayland->max_pending_presentations <= 2,
            "invalid Wayland presentation queue bound");
      readback_presentation = !wayland->readback_render_node.empty();
      opaque_background = wayland->opaque_background;
      defer_scene_fence = wayland->defer_scene_fence && !readback_presentation;
#ifdef HEADUNIT_THEME_HAVE_SKIA
      max_pending_gpu_scenes = wayland->max_pending_presentations;
#endif
      window = std::make_unique<WaylandWindow>(
          width, height, wayland->title, readback_presentation ? 2 : 1, wayland->display_name,
          wayland->accept_touch, wayland->max_pending_presentations, wayland->app_id,
          wayland->output_name, wayland->touch_diagnostics, wayland->opaque_background,
          wayland->background_layer, wayland->local_diagnostics);
      if (!readback_presentation)
        display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, window->display(), nullptr);
#else
      throw std::runtime_error("Wayland GPU support is not built");
#endif
    }
    if (!wayland || readback_presentation) {
      const auto& render_node = readback_presentation ? wayland->readback_render_node : node;
      check(
          render_node.starts_with("/dev/dri/renderD") && render_node.find('\0') == render_node.npos,
          "invalid GPU render node");
      fd = ::open(render_node.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
      check(fd >= 0, "cannot open GPU render node");
      struct stat st{};
      check(::fstat(fd, &st) == 0 && S_ISCHR(st.st_mode), "GPU render node is not a device");
      device = gbm_create_device(fd);
      check(device, "GBM device creation failed");
      display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, device, nullptr);
    }
    check(display != EGL_NO_DISPLAY, "GPU EGL display unavailable");
    EGLint major = 0, minor = 0;
    check(eglInitialize(display, &major, &minor), "GPU EGL initialization failed");
    const auto* extensions = eglQueryString(display, EGL_EXTENSIONS);
    for (const auto* name : {"EGL_KHR_surfaceless_context", "EGL_EXT_image_dma_buf_import",
                             "EGL_EXT_image_dma_buf_import_modifiers", "EGL_KHR_fence_sync"})
      check(extension(extensions, name), "GPU lacks required import or synchronization extension");
    check(eglBindAPI(EGL_OPENGL_ES_API), "GPU GLES API unavailable");
    // GBM configurations may expose only window surfaces. We use a surfaceless
    // context and an explicit FBO, so requiring a pbuffer would reject them.
    const EGLint attrs[]{EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_ES2_BIT,
                         EGL_SURFACE_TYPE,
                         wayland && !readback_presentation ? EGL_WINDOW_BIT : 0,
                         EGL_RED_SIZE,
                         8,
                         EGL_GREEN_SIZE,
                         8,
                         EGL_BLUE_SIZE,
                         8,
                         EGL_ALPHA_SIZE,
                         8,
                         EGL_NONE};
    EGLConfig config{};
    EGLint count = 0;
    check(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1,
          "GPU EGL config unavailable");
    constexpr EGLint ctx[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx);
    check(context != EGL_NO_CONTEXT, "GPU context creation failed");
    current();
#ifdef HEADUNIT_THEME_GPU_WAYLAND
    if (window && !readback_presentation) {
      window_surface = eglCreatePlatformWindowSurface(display, config, window->window(), nullptr);
      check(window_surface != EGL_NO_SURFACE, "GPU Wayland surface unavailable");
      check(eglMakeCurrent(display, window_surface, window_surface, context),
            "GPU Wayland context binding failed");
      check(eglSwapInterval(display, 0), "GPU Wayland swap interval unavailable");
    }
#endif
    const auto* name = glGetString(GL_RENDERER);
    check(name, "GPU renderer identification unavailable");
    renderer = reinterpret_cast<const char*>(name);
    check(renderer.find("llvmpipe") == renderer.npos && renderer.find("softpipe") == renderer.npos,
          "software renderer rejected by GPU policy");
    check(extension(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)),
                    "GL_OES_EGL_image_external"),
          "GPU lacks external texture support");
    create_image =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroy_image =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    image_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    create_sync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    destroy_sync =
        reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    wait_sync =
        reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    check(
        create_image && destroy_image && image_texture && create_sync && destroy_sync && wait_sync,
        "GPU extension entry point unavailable");
    video_program = program(
        "#extension GL_OES_EGL_image_external : require\nprecision highp float;varying vec2 "
        "tex;uniform samplerExternalOES img;void main(){gl_FragColor=texture2D(img,tex);}");
    quad_program =
        program("precision highp float;uniform vec4 color;void main(){gl_FragColor=color;}");
    overlay_program =
        program("precision mediump float;varying vec4 tint;void main(){gl_FragColor=tint;}",
                "attribute vec2 pos;attribute vec4 color;varying vec4 tint;void "
                "main(){tint=color;gl_Position=vec4(pos,0.,1.);}",
                "color");
    copy_program = program(
        "precision highp float;varying vec2 tex;uniform sampler2D img;uniform float "
        "opaqueBackground;void main(){vec4 "
        "c=texture2D(img,tex);gl_FragColor=vec4(c.rgb,mix(c.a,1.,opaqueBackground));}");
    glGenBuffers(1, &vertices);
    glBindBuffer(GL_ARRAY_BUFFER, vertices);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
              glGetError() == GL_NO_ERROR,
          "GPU framebuffer initialization failed");
  }
  void raw_state() {
    // Ganesh owns arbitrary GL state. Restore every state used by our raw blits
    // and video primitives; its next draw resets its own state cache.
#ifdef HEADUNIT_THEME_HAVE_SKIA
    if (scene) {
      const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
      const bool es3 = version && std::string_view(version).starts_with("OpenGL ES 3");
      if (es3 || extension(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)),
                           "GL_OES_vertex_array_object")) {
        auto bind = reinterpret_cast<PFNGLBINDVERTEXARRAYOESPROC>(
            eglGetProcAddress("glBindVertexArrayOES"));
        if (!bind)
          bind =
              reinterpret_cast<PFNGLBINDVERTEXARRAYOESPROC>(eglGetProcAddress("glBindVertexArray"));
        check(bind, "GPU vertex array reset unavailable");
        bind(0);
      }
      if (es3) {
        using BindSampler = void (*)(GLuint, GLuint);
        const auto bind = reinterpret_cast<BindSampler>(eglGetProcAddress("glBindSampler"));
        check(bind, "GPU sampler reset unavailable");
        bind(0, 0);
        glBindBuffer(0x88EB, 0);
        glBindBuffer(0x88EC, 0);  //PIXEL_PACK/UNPACK_BUFFER
        glPixelStorei(0x0D02, 0);
        glPixelStorei(0x0D03, 0);
        glPixelStorei(0x0D04, 0);  //PACK row length/skips
        glDisable(0x8C89);         //RASTERIZER_DISCARD
      }
      if (es3 || extension(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)),
                           "GL_EXT_unpack_subimage")) {
        glPixelStorei(0x0CF2, 0);
        glPixelStorei(0x0CF3, 0);
        glPixelStorei(0x0CF4, 0);  //UNPACK row length/skips
      }
      glDisable(GL_CULL_FACE);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_STENCIL_TEST);
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_DITHER);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glBlendEquation(GL_FUNC_ADD);
      glActiveTexture(GL_TEXTURE0);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glBindBuffer(GL_ARRAY_BUFFER, vertices);
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            reinterpret_cast<void*>(2 * sizeof(float)));
    }
#endif
  }
  void quad(float left, float top, float right, float bottom, float u0 = 0, float v0 = 0,
            float u1 = 1, float v1 = 1) {
    const float data[]{left,  top, u0, v0, left,  bottom, u0, v1,
                       right, top, u1, v0, right, bottom, u1, v1};
    glBufferData(GL_ARRAY_BUFFER, sizeof(data), data, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  void prune_masks() {
    for (auto it = masks.begin(); it != masks.end();) {
      if (it->second.source.expired()) {
        glDeleteTextures(1, &it->second.texture);
        mask_counts.resident_bytes -= it->second.bytes;
        it = masks.erase(it);
      } else
        ++it;
    }
    mask_counts.entries = masks.size();
  }
  GLuint mask_texture(const std::shared_ptr<const AlphaMask>& mask) {
    if (const auto found = masks.find(mask.get()); found != masks.end()) {
      // prune_masks removes dead identities before each frame; a retained
      // immutable object cannot change either pixels or its output origin.
      found->second.used = ++mask_serial;
      return found->second.texture;
    }
    constexpr std::size_t byte_limit = 64 * 1024 * 1024, entry_limit = 32;
    const auto bytes = mask->pixels().size();
    check(bytes && bytes <= byte_limit, "invalid GPU mask allocation");
    while (masks.size() >= entry_limit || mask_counts.resident_bytes + bytes > byte_limit) {
      const auto oldest = std::min_element(
          masks.begin(), masks.end(),
          [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
      check(oldest != masks.end(), "GPU mask cache budget exhausted");
      glDeleteTextures(1, &oldest->second.texture);
      mask_counts.resident_bytes -= oldest->second.bytes;
      masks.erase(oldest);
    }
    GLuint texture = 0;
    try {
      glGenTextures(1, &texture);
      check(texture, "GPU mask texture allocation failed");
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, mask->width(), mask->height(), 0, GL_ALPHA,
                   GL_UNSIGNED_BYTE, mask->pixels().data());
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      check(glGetError() == GL_NO_ERROR, "GPU mask upload failed");
      masks.emplace(mask.get(), MaskEntry{mask, texture, bytes, ++mask_serial});
    } catch (...) {
      if (texture) glDeleteTextures(1, &texture);
      throw;
    }
    ++mask_counts.uploads;
    mask_counts.uploaded_bytes += bytes;
    mask_counts.resident_bytes += bytes;
    mask_counts.entries = masks.size();
    return texture;
  }
  void conical(const GpuConicalGradient& paint) {
    if (paint.opacity == 0 || paint.bounds.size.width == 0 || paint.bounds.size.height == 0 ||
        (paint.mask && paint.mask->empty()))
      return;
    const auto& b = paint.bounds;
    const auto t = *paint.to_output.inverse();
    const std::array<Point, 4> corners{
        {paint.to_output.apply(b.origin),
         paint.to_output.apply({b.origin.x + b.size.width, b.origin.y}),
         paint.to_output.apply({b.origin.x, b.origin.y + b.size.height}),
         paint.to_output.apply({b.origin.x + b.size.width, b.origin.y + b.size.height})}};
    // An axis-aligned, integer-edged rectangle has exact pixel coverage from
    // the scissor. Keep the expensive curved-edge shader out of those fragments.
    const bool rectangular =
        paint.corner_radius == 0 && paint.to_output.b == 0 && paint.to_output.c == 0 &&
        std::all_of(corners.begin(), corners.end(),
                    [](Point p) { return p.x == std::trunc(p.x) && p.y == std::trunc(p.y); });
    auto& paint_program = conical_programs[(paint.stops.size() - 1) * 4 + (rectangular ? 2 : 0) +
                                           (paint.mask ? 1 : 0)];
    if (!paint_program) {
      std::string fragment = std::string(rectangular ? "#define RECTANGULAR 1\n" : "") +
                             (paint.mask ? "#define MASKED 1\n" : "") +
                             R"GLSL(precision highp float;
uniform mat3 inverseTransform;
uniform float outputHeight;
uniform vec4 bounds;
uniform vec2 center;
uniform float zeroAngle;
uniform float radius;
uniform float opacity;
uniform vec4 colors[32];
uniform float locations[32];
#ifdef MASKED
uniform sampler2D maskTexture;
uniform vec4 maskBounds;
#endif
#ifndef RECTANGULAR
float edgeDistance(vec2 p){
  vec2 q=abs(p-bounds.zw*.5)-(bounds.zw*.5-vec2(radius));
  return length(max(q,0.))+min(max(q.x,q.y),0.)-radius;
}
#endif
void main(){
#ifdef MASKED
  vec2 maskUV=(vec2(gl_FragCoord.x,outputHeight-gl_FragCoord.y)-maskBounds.xy)/maskBounds.zw;
  float maskCoverage=texture2D(maskTexture,maskUV).a;
  if(maskCoverage==0.)discard;
#else
  float maskCoverage=1.;
#endif
  vec2 p=(inverseTransform*vec3(gl_FragCoord.x,outputHeight-gl_FragCoord.y,1.)).xy-bounds.xy;
#ifdef RECTANGULAR
  float coverage=1.;
#else
  vec2 dx=inverseTransform[0].xy,dy=inverseTransform[1].xy;
  float distance=edgeDistance(p),reach=.5*(length(dx)+length(dy));
  float coverage=distance<=-reach?1.:0.;
  if(abs(distance)<reach){
    vec2 a=p-.5*dx-.5*dy,b=p+.5*dx-.5*dy,c=p-.5*dx+.5*dy,d=p+.5*dx+.5*dy;
    vec2 lo=min(min(a,b),min(c,d)),hi=max(max(a,b),max(c,d));
    if(hi.x<=0. || hi.y<=0. || lo.x>=bounds.z || lo.y>=bounds.w)coverage=0.;
    else if(max(max(edgeDistance(a),edgeDistance(b)),max(edgeDistance(c),edgeDistance(d)))<=0.)coverage=1.;
    else for(int y=0;y<32;++y)for(int x=0;x<32;++x){
      vec2 samplePoint=p+dx*((float(x)+.5)/32.-.5)+dy*((float(y)+.5)/32.-.5);
      coverage+=edgeDistance(samplePoint)<=0.?1./1024.:0.;
    }
  }
  if(coverage==0.){gl_FragColor=vec4(0.);return;}
#endif
  vec2 delta=p-center;
  float angle=dot(delta,delta)==0.?0.:atan(delta.y,delta.x);
  float position=fract((zeroAngle-angle)/6.283185307179586);
  vec4 color=sampleGradient(position);
  gl_FragColor=color*(opacity*coverage*maskCoverage);
}
)GLSL";
      // GLES2 drivers can lower indirectly indexed uniform arrays into large
      // selection trees. Emit bounded constant indices, retaining exact hard
      // stops and premultiplied interpolation without a colour lookup texture.
      std::string sample = "vec4 sampleGradient(float p){if(p<locations[0])return colors[0];\n";
      for (unsigned i = 1; i < paint.stops.size(); ++i) {
        const auto at = std::to_string(i), previous = std::to_string(i - 1);
        sample += "if(p<locations[" + at + "])return mix(colors[" + previous + "],colors[" + at +
                  "],clamp((p-locations[" + previous + "])/(locations[" + at + "]-locations[" +
                  previous + "]),0.,1.));\n";
      }
      sample += "return colors[" + std::to_string(paint.stops.size() - 1) + "];}\n";
      fragment.insert(fragment.find("void main(){"), sample);
      paint_program = program(fragment.c_str());
    }
    auto lo = corners[0], hi = lo;
    for (const auto& p : corners) {
      lo.x = std::min(lo.x, p.x);
      lo.y = std::min(lo.y, p.y);
      hi.x = std::max(hi.x, p.x);
      hi.y = std::max(hi.y, p.y);
    }
    const auto fringe = rectangular ? 0 : 1;
    lo.x = std::floor(lo.x) - fringe;
    lo.y = std::floor(lo.y) - fringe;
    hi.x = std::ceil(hi.x) + fringe;
    hi.y = std::ceil(hi.y) + fringe;
    if (paint.mask) {
      lo.x = std::max(lo.x, double(paint.mask->x()));
      lo.y = std::max(lo.y, double(paint.mask->y()));
      hi.x = std::min(hi.x, double(paint.mask->x()) + paint.mask->width());
      hi.y = std::min(hi.y, double(paint.mask->y()) + paint.mask->height());
    }
    const auto x0 = int(std::clamp(lo.x, 0., double(width)));
    const auto y0 = int(std::clamp(lo.y, 0., double(height)));
    const auto x1 = int(std::clamp(hi.x, 0., double(width)));
    const auto y1 = int(std::clamp(hi.y, 0., double(height)));
    if (x0 >= x1 || y0 >= y1) return;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, height - y1, x1 - x0, y1 - y0);
    glUseProgram(paint_program);
    const auto location = [&](const char* name) {
      return glGetUniformLocation(paint_program, name);
    };
    const GLfloat inverse[]{GLfloat(t.a),  GLfloat(t.b),  0, GLfloat(t.c), GLfloat(t.d), 0,
                            GLfloat(t.tx), GLfloat(t.ty), 1};
    glUniformMatrix3fv(location("inverseTransform"), 1, GL_FALSE, inverse);
    glUniform1f(location("outputHeight"), height);
    glUniform4f(location("bounds"), b.origin.x, b.origin.y, b.size.width, b.size.height);
    glUniform2f(location("center"), paint.start.x * b.size.width, paint.start.y * b.size.height);
    glUniform1f(location("zeroAngle"), std::atan2((paint.end.y - paint.start.y) * b.size.height,
                                                  (paint.end.x - paint.start.x) * b.size.width));
    glUniform1f(location("radius"),
                std::clamp(paint.corner_radius, 0., std::min(b.size.width, b.size.height) / 2));
    glUniform1f(location("opacity"), paint.opacity);
    if (paint.mask) {
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, mask_texture(paint.mask));
      glUniform1i(location("maskTexture"), 0);
      glUniform4f(location("maskBounds"), paint.mask->x(), paint.mask->y(), paint.mask->width(),
                  paint.mask->height());
    }
    std::array<GLfloat, 128> colors{};
    std::array<GLfloat, 32> locations{};
    for (std::size_t i = 0; i < paint.stops.size(); ++i) {
      const auto& stop = paint.stops[i];
      const auto& c = stop.color;
      colors[4 * i] = c.r * c.a;
      colors[4 * i + 1] = c.g * c.a;
      colors[4 * i + 2] = c.b * c.a;
      colors[4 * i + 3] = c.a;
      locations[i] = stop.location;
    }
    glUniform4fv(location("colors"), paint.stops.size(), colors.data());
    glUniform1fv(location("locations"), paint.stops.size(), locations.data());
    quad(-1, 1, 1, -1);
    glDisable(GL_SCISSOR_TEST);
  }
};
GpuCompositor::GpuCompositor(std::uint32_t width, std::uint32_t height, const std::string& node)
    : impl_(std::make_unique<Impl>(width, height)) {
  impl_->start(node);
}
GpuCompositor::GpuCompositor(std::uint32_t width, std::uint32_t height, WaylandOptions options)
    : impl_(std::make_unique<Impl>(width, height)) {
  impl_->start({}, std::move(options));
}
GpuCompositor::~GpuCompositor() = default;
GpuTimings GpuCompositor::compose(std::shared_ptr<const video::NativeFrame> frame,
                                  std::span<const GpuQuad> overlays) {
  auto& h = *impl_;
  check(!h.faulted, "GPU compositor is faulted");
#ifdef HEADUNIT_THEME_HAVE_SKIA
  h.finish_deferred_scene(true);
#endif
  check(frame && frame->lifetime && frame->plane_count > 0 && frame->plane_count <= 4 &&
            frame->storage_width && frame->storage_height && frame->storage_width <= 8192 &&
            frame->storage_height <= 8192 && frame->metadata.width > 0 &&
            frame->metadata.height > 0 &&
            std::uint64_t(frame->crop_x) + frame->metadata.width <= frame->storage_width &&
            std::uint64_t(frame->crop_y) + frame->metadata.height <= frame->storage_height,
        "invalid GPU input frame");
  check(overlays.size() <= 256, "GPU overlay count exceeds limit");
  for (const auto& q : overlays) {
    for (const auto n : {q.rect.origin.x, q.rect.origin.y, q.rect.size.width, q.rect.size.height})
      check(std::isfinite(n) && std::abs(n) <= 16384, "invalid GPU overlay rectangle");
    for (const auto n : {q.color.r, q.color.g, q.color.b, q.color.a})
      check(std::isfinite(n) && n >= 0 && n <= 1, "invalid GPU overlay colour");
    check(q.rect.size.width >= 0 && q.rect.size.height >= 0, "negative GPU overlay size");
  }
  const auto attrs = dma_attributes(*frame);
  try {
    h.current();
    h.raw_state();
    h.lease = std::move(frame);
    const auto& f = *h.lease;
    const auto began = now();
    h.imported =
        h.create_image(h.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    check(h.imported != EGL_NO_IMAGE_KHR, "GPU DMA-buf import failed");
    glGenTextures(1, &h.external);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, h.external);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    h.image_texture(GL_TEXTURE_EXTERNAL_OES, h.imported);
    check(glGetError() == GL_NO_ERROR, "GPU native texture binding failed");
    const auto imported = now();
    glBindFramebuffer(GL_FRAMEBUFFER, h.framebuffer);
    glViewport(0, 0, h.width, h.height);
    glDisable(GL_DITHER);
    glDisable(GL_BLEND);
    glUseProgram(h.video_program);
    glUniform1i(glGetUniformLocation(h.video_program, "img"), 0);
    h.quad(-1, 1, 1, -1, float(f.crop_x) / f.storage_width, float(f.crop_y) / f.storage_height,
           float(f.crop_x + f.metadata.width) / f.storage_width,
           float(f.crop_y + f.metadata.height) / f.storage_height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // Every translucent rectangle has its own premultiplied vertex colour.
    // Triangle order is the original rectangle order, so intersections blend
    // identically while the whole overlay batch needs only one GPU draw call.
    h.overlay_vertices.resize(overlays.size() * 36);
    for (std::size_t i = 0; i < overlays.size(); ++i) {
      const auto& q = overlays[i];
      const float left = 2 * q.rect.origin.x / h.width - 1,
                  top = 1 - 2 * q.rect.origin.y / h.height;
      const float right = 2 * (q.rect.origin.x + q.rect.size.width) / h.width - 1;
      const float bottom = 1 - 2 * (q.rect.origin.y + q.rect.size.height) / h.height;
      const auto& c = q.color;
      const std::array<std::array<float, 2>, 6> corners{{{left, top},
                                                         {left, bottom},
                                                         {right, top},
                                                         {right, top},
                                                         {left, bottom},
                                                         {right, bottom}}};
      for (std::size_t v = 0; v < corners.size(); ++v) {
        auto* out = h.overlay_vertices.data() + i * 36 + v * 6;
        out[0] = corners[v][0];
        out[1] = corners[v][1];
        out[2] = c.r * c.a;
        out[3] = c.g * c.a;
        out[4] = c.b * c.a;
        out[5] = c.a;
      }
    }
    if (!h.overlay_vertices.empty()) {
      glBufferData(GL_ARRAY_BUFFER, h.overlay_vertices.size() * sizeof(float),
                   h.overlay_vertices.data(), GL_STREAM_DRAW);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
      glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                            reinterpret_cast<void*>(2 * sizeof(float)));
      glUseProgram(h.overlay_program);
      glDrawArrays(GL_TRIANGLES, 0, GLsizei(overlays.size() * 6));
      // The next video or scene draw uses position/UV vertices on this VBO.
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                            reinterpret_cast<void*>(2 * sizeof(float)));
    }
    check(glGetError() == GL_NO_ERROR, "GPU composition failed");
    h.fence = h.create_sync(h.display, EGL_SYNC_FENCE_KHR, nullptr);
    check(h.fence != EGL_NO_SYNC_KHR, "GPU completion fence unavailable");
    check(h.wait_sync(h.display, h.fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 100000000) ==
              EGL_CONDITION_SATISFIED_KHR,
          "GPU completion deadline exceeded");
    const auto finished = now();
    h.clear_import();
    h.has_frame = true;
    return {imported - began, finished - imported};
  } catch (...) {
    h.faulted = true;
    throw;
  }
}
Image GpuCompositor::readback() {
  auto& h = *impl_;
  check(!h.faulted && h.has_frame, "GPU has no completed frame");
#ifdef HEADUNIT_THEME_HAVE_SKIA
  h.finish_deferred_scene(true);
#endif
  h.current();
  h.raw_state();
  glBindFramebuffer(GL_FRAMEBUFFER, h.framebuffer);
  return read_pixels(h.width, h.height);
}
GpuTimings GpuCompositor::compose_reference(const Image& image) {
  auto& h = *impl_;
  check(!h.faulted, "GPU compositor is faulted");
#ifdef HEADUNIT_THEME_HAVE_SKIA
  h.finish_deferred_scene(true);
#endif
  check(image.width == h.width && image.height == h.height &&
            image.pixels.size() == std::size_t(h.width) * h.height,
        "invalid reference image dimensions");
  try {
    h.current();
    h.raw_state();
    const auto began = now();
    std::vector<std::uint8_t> rgba(image.pixels.size() * 4);
    // Match the target's bottom-up GL orientation, retaining premultiplied alpha.
    for (std::uint32_t y = 0; y < h.height; ++y)
      for (std::uint32_t x = 0; x < h.width; ++x) {
        const auto p = image.pixels[std::size_t(y) * h.width + x];
        const auto at = (std::size_t(h.height - 1 - y) * h.width + x) * 4;
        rgba[at] = p >> 16;
        rgba[at + 1] = p >> 8;
        rgba[at + 2] = p;
        rgba[at + 3] = p >> 24;
      }
    glBindTexture(GL_TEXTURE_2D, h.target);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, h.width, h.height, GL_RGBA, GL_UNSIGNED_BYTE,
                    rgba.data());
    check(glGetError() == GL_NO_ERROR, "reference image upload failed");
    const auto uploaded = now();
    h.fence = h.create_sync(h.display, EGL_SYNC_FENCE_KHR, nullptr);
    check(h.fence != EGL_NO_SYNC_KHR, "reference GPU completion fence unavailable");
    check(h.wait_sync(h.display, h.fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 100000000) ==
              EGL_CONDITION_SATISFIED_KHR,
          "reference GPU upload deadline exceeded");
    h.clear_import();
    h.has_frame = true;
    return {uploaded - began, now() - uploaded};
  } catch (...) {
    h.faulted = true;
    throw;
  }
}
GpuTimings GpuCompositor::compose_conical(std::span<const GpuConicalGradient> paints, Color clear) {
  auto& h = *impl_;
  check(!h.faulted, "GPU compositor is faulted");
#ifdef HEADUNIT_THEME_HAVE_SKIA
  h.finish_deferred_scene(true);
#endif
  const auto scalar = [](double v) {
    check(std::isfinite(v) && std::abs(v) <= 1e7, "invalid GPU conical coordinate");
  };
  const auto color = [](Color c) {
    for (const auto v : {c.r, c.g, c.b, c.a})
      check(std::isfinite(v) && v >= 0 && v <= 1, "invalid GPU conical color");
  };
  color(clear);
  check(paints.size() <= 256, "GPU conical paint count exceeds limit");
  for (const auto& p : paints) {
    const auto& b = p.bounds;
    const auto& t = p.to_output;
    for (const auto v : {b.origin.x, b.origin.y, b.size.width, b.size.height, t.a, t.b, t.c, t.d,
                         t.tx, t.ty, p.start.x, p.start.y, p.end.x, p.end.y, p.corner_radius})
      scalar(v);
    check(
        b.size.width >= 0 && b.size.height >= 0 && p.corner_radius >= 0 && t.inverse().has_value(),
        "invalid GPU conical geometry");
    check(std::isfinite(p.opacity) && p.opacity >= 0 && p.opacity <= 1,
          "invalid GPU conical opacity");
    check(!p.stops.empty() && p.stops.size() <= 32, "invalid GPU conical stop count");
    check(!b.size.width || !b.size.height ||
              std::hypot((p.end.x - p.start.x) * b.size.width,
                         (p.end.y - p.start.y) * b.size.height) > 1e-12,
          "degenerate GPU conical direction");
    double previous = -1;
    for (const auto& s : p.stops) {
      color(s.color);
      check(
          std::isfinite(s.location) && s.location >= 0 && s.location <= 1 && s.location >= previous,
          "invalid GPU conical stops");
      previous = s.location;
    }
  }
  try {
    h.current();
    h.raw_state();
    h.prune_masks();
    const auto began = now();
    glBindFramebuffer(GL_FRAMEBUFFER, h.framebuffer);
    glViewport(0, 0, h.width, h.height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DITHER);
    glClearColor(clear.r * clear.a, clear.g * clear.a, clear.b * clear.a, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (const auto& p : paints)
      h.conical(p);
    check(glGetError() == GL_NO_ERROR, "GPU conical composition failed");
    h.fence = h.create_sync(h.display, EGL_SYNC_FENCE_KHR, nullptr);
    check(h.fence != EGL_NO_SYNC_KHR, "GPU conical completion fence unavailable");
    check(h.wait_sync(h.display, h.fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 100000000) ==
              EGL_CONDITION_SATISFIED_KHR,
          "GPU conical completion deadline exceeded");
    h.clear_import();
    h.has_frame = true;
    return {0, now() - began};
  } catch (...) {
    h.faulted = true;
    throw;
  }
}
GpuMaskStats GpuCompositor::mask_stats() const {
  return impl_->mask_counts;
}
#ifdef HEADUNIT_THEME_HAVE_SKIA
GpuSceneCacheStats GpuCompositor::scene_cache_stats() const {
  auto stats = impl_->scene ? impl_->scene->cache_stats() : GpuSceneCacheStats{};
  stats.deferred_gpu_frames = impl_->deferred_scenes.size();
  stats.max_deferred_gpu_frames = impl_->max_deferred_seen;
  return stats;
}
GpuTimings GpuCompositor::compose_scene(const std::shared_ptr<const Layer>& root,
                                        std::uint64_t completion_timeout_ns,
                                        const std::function<void()>& during_completion) {
  auto& h = *impl_;
  check(!h.faulted, "GPU compositor is faulted");
  if (h.defer_scene_fence) {
    h.finish_deferred_scene(false);
    while (!h.deferred_scenes.empty() && h.deferred_scenes.size() >= h.max_pending_gpu_scenes)
      h.finish_oldest_scene(true);
  } else
    h.finish_deferred_scene(true);
  check(completion_timeout_ns > 0 && completion_timeout_ns <= 2000000000,
        "invalid GPU scene completion deadline");
  const auto validating = now();
  SkiaScene::validate(root, h.width, h.height);
  const auto validation_ns = now() - validating;
  struct Source {
    std::shared_ptr<const video::NativeFrame> frame;
    std::vector<EGLint> attributes;
  };
  std::vector<Source> sources;
  const auto collect = [&](auto&& self, const std::shared_ptr<const Layer>& value) -> void {
    if (!value || value->hidden || value->opacity == 0 || !value->transform.inverse()) return;
    if (value->kind == Kind::video && value->video)
      if (auto frame = value->video->native()) {
        if (std::none_of(sources.begin(), sources.end(),
                         [&](const auto& s) { return s.frame == frame; })) {
          check(sources.size() < 8, "native scene texture limit exceeded");
          sources.push_back({frame, dma_attributes(*frame)});
        }
      }
    for (const auto& child : value->children)
      self(self, child);
    if (value->mask) self(self, value->mask);
  };
  collect(collect, root);  //Validate every descriptor before changing the framebuffer.
  try {
    h.current();
    h.raw_state();
    const auto began = now();
    check(h.scene_imports.empty(), "native scene still owns a prior texture");
    std::vector<SkiaNativeTexture*> native;
    const auto importing = now();
    for (const auto& source : sources) {
      auto& entry = *h.scene_imports.emplace_back(std::make_unique<Impl::SceneImport>());
      entry.view.frame = source.frame;
      entry.image = h.create_image(h.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
                                   source.attributes.data());
      check(entry.image != EGL_NO_IMAGE_KHR, "native scene DMA-buf import failed");
      glGenTextures(1, &entry.view.texture);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, entry.view.texture);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      h.image_texture(GL_TEXTURE_EXTERNAL_OES, entry.image);
      check(glGetError() == GL_NO_ERROR, "native scene texture binding failed");
      native.push_back(&entry.view);
    }
    const auto import_ns = now() - importing;
    if (!h.scene)
      h.scene =
          std::make_unique<SkiaScene>(h.width, h.height, h.framebuffer, h.scene_configuration);
    h.scene->draw(root, native);
    h.fence = h.create_sync(h.display, EGL_SYNC_FENCE_KHR, nullptr);
    check(h.fence != EGL_NO_SYNC_KHR, "GPU scene completion fence unavailable");
    const auto fenced = now();
    if (during_completion) {
      // Start GPU execution before doing CPU work; otherwise the fence's flush
      // flag would first reach the driver only after the callback returns.
      const auto started = h.wait_sync(h.display, h.fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0);
      check(started == EGL_CONDITION_SATISFIED_KHR || started == EGL_TIMEOUT_EXPIRED_KHR,
            "GPU scene fence flush failed");
      during_completion();
    }
    const auto overlap_ns = during_completion ? now() - fenced : 0;
    if (overlap_ns >= completion_timeout_ns)
      throw std::runtime_error("GPU scene completion deadline exceeded during owner work");
    if (h.defer_scene_fence) {
      // GL commands for this scene and its final Wayland blit precede the next
      // scene in the same context. Only immutable borrowed images/imports cross
      // submissions. The script graph is still read on its sole owner thread.
      auto images = h.scene->retain_native();
      h.deferred_scenes.emplace_back();  // Reserve before moving owned imports/fence.
      auto& submission = h.deferred_scenes.back();
      submission.images = std::move(images);
      submission.imports = std::move(h.scene_imports);
      submission.completion = std::exchange(h.fence, EGL_NO_SYNC_KHR);
      submission.deadline_ns = fenced + completion_timeout_ns;
      h.max_deferred_seen = std::max(h.max_deferred_seen, unsigned(h.deferred_scenes.size()));
      h.has_frame = true;
      return {import_ns, now() - began, validation_ns, 0, overlap_ns};
    }
    const auto waiting = now();
    const auto result = h.wait_sync(h.display, h.fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
                                    completion_timeout_ns - overlap_ns);
    const auto completion_wait_ns = now() - waiting;
    if (result != EGL_CONDITION_SATISFIED_KHR)
      throw std::runtime_error(
          "GPU scene completion deadline exceeded: status=" + std::to_string(result) + " draw_ns=" +
          std::to_string(waiting - began) + " wait_ns=" + std::to_string(completion_wait_ns) +
          " limit_ns=" + std::to_string(completion_timeout_ns));
    check(glGetError() == GL_NO_ERROR, "GPU scene composition failed");
    h.scene->release_native();
    for (const auto& entry : h.scene_imports)
      check(entry->view.released, "Skia retained a completed native texture");
    h.clear_scene_imports(true);
    h.clear_import();
    h.has_frame = true;
    return {import_ns, now() - began, validation_ns, completion_wait_ns, overlap_ns};
  } catch (...) {
    h.faulted = true;
    throw;
  }
}
#endif
void GpuCompositor::submit(std::uint64_t token, Image* verification, WindowContent content) {
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  auto& h = *impl_;
  check(h.window && !h.faulted && h.has_frame, "GPU has no completed Wayland frame");
  check(h.window->can_submit(), "GPU Wayland presentation queue is full");
  check(content == WindowContent::rendered_scene || content == WindowContent::reveal_underlay,
        "invalid Wayland window content policy");
  const bool reveal = content == WindowContent::reveal_underlay;
  const bool opaque = h.opaque_background && !reveal;
  try {
    if (verification) {
#ifdef HEADUNIT_THEME_HAVE_SKIA
      h.finish_deferred_scene(true);
#endif
    }
    if (h.readback_presentation) {
      auto image = readback();
      if (reveal)
        std::fill(image.pixels.begin(), image.pixels.end(), 0);
      else if (opaque)
        for (auto& pixel : image.pixels)
          pixel |= 0xff000000;
      h.window->submit_image(token, image, opaque);
      if (verification) *verification = std::move(image);
      h.has_frame = false;
      return;
    }
    check(eglMakeCurrent(h.display, h.window_surface, h.window_surface, h.context),
          "GPU Wayland context binding failed");
    const auto [window_width, window_height] = h.window->buffer_size();
    h.raw_state();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, window_width, window_height);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, reveal ? 0.f : 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    // Preserve the signed asset's coordinate system and aspect ratio. Only the
    // final GPU blit fits it into the compositor-selected physical window.
    if (!reveal) {
      const double fit = std::min(double(window_width) / h.width, double(window_height) / h.height);
      const auto fitted_width = std::max(1u, std::uint32_t(h.width * fit));
      const auto fitted_height = std::max(1u, std::uint32_t(h.height * fit));
      glViewport((window_width - fitted_width) / 2, (window_height - fitted_height) / 2,
                 fitted_width, fitted_height);
      glUseProgram(h.copy_program);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, h.target);
      glUniform1i(glGetUniformLocation(h.copy_program, "img"), 0);
      glUniform1f(glGetUniformLocation(h.copy_program, "opaqueBackground"), opaque ? 1.f : 0.f);
      h.quad(-1, 1, 1, -1, 0, 1, 1, 0);
    }
    check(glGetError() == GL_NO_ERROR, "GPU Wayland composition failed");
    if (verification) *verification = read_pixels(window_width, window_height);
    h.window->request(token, opaque);
    const bool swapped = eglSwapBuffers(h.display, h.window_surface);
    h.window->observe_egl_swap_return(token, swapped);
    check(swapped, "GPU Wayland swap failed");
    h.has_frame = false;
  } catch (...) {
    h.faulted = true;
    throw;
  }
#else
  (void)token;
  (void)verification;
  (void)content;
  throw std::runtime_error("Wayland GPU support is not built");
#endif
}
std::vector<headunit::TouchInput> GpuCompositor::take_touches(bool enabled) {
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  return impl_->window ? impl_->window->take_touches(enabled) : std::vector<headunit::TouchInput>{};
#else
  (void)enabled;
  return {};
#endif
}
bool GpuCompositor::presentation_pending() const {
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  return impl_->window && impl_->window->pending();
#else
  return false;
#endif
}
void GpuCompositor::set_presentation_queue_depth(unsigned depth) {
  check(depth >= 1 && depth <= 2, "invalid Wayland presentation queue bound");
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  check(bool(impl_->window), "presentation queue depth needs a Wayland window");
  impl_->window->set_max_pending(depth);
#ifdef HEADUNIT_THEME_HAVE_SKIA
  impl_->max_pending_gpu_scenes = depth;
#endif
#else
  throw std::runtime_error("Wayland GPU support is not built");
#endif
}
bool GpuCompositor::presentation_slot_available() const {
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  return impl_->window && impl_->window->can_submit();
#else
  return false;
#endif
}
std::optional<PresentationReceipt> GpuCompositor::poll_presentation() {
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  auto& h = *impl_;
  check(h.window && !h.faulted, "GPU has no usable Wayland window");
  try {
    auto receipt = h.window->poll_receipt(h.readback_presentation);
#ifdef HEADUNIT_THEME_HAVE_SKIA
    // With two pending presentations, this receipt may belong to an older
    // scene. It must not block on the latest scene's unrelated GPU fence.
    // Keep that scene's native leases until its own fence signals; compose,
    // readback and synchronous present still explicitly wait when needed.
    h.finish_deferred_scene(false);
#endif
    return receipt;
  } catch (...) {
    h.faulted = true;
    throw;
  }
#else
  throw std::runtime_error("Wayland GPU support is not built");
#endif
}
bool GpuCompositor::preview_feedback_delayed() const {
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  return impl_->readback_presentation && impl_->window && impl_->window->feedback_delayed();
#else
  return false;
#endif
}
PresentationReceipt GpuCompositor::present(std::uint64_t token, Image* verification) {
  submit(token, verification);
#ifdef HEADUNIT_THEME_GPU_WAYLAND
  auto& h = *impl_;
  try {
    auto receipt = h.window->wait();
#ifdef HEADUNIT_THEME_HAVE_SKIA
    h.finish_deferred_scene(true);
#endif
    return receipt;
  } catch (...) {
    h.faulted = true;
    throw;
  }
#else
  throw std::runtime_error("Wayland GPU support is not built");
#endif
}
std::string GpuCompositor::renderer() const {
  return impl_->renderer;
}
}  // namespace headunit::theme::graphics
