// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/wayland_preview.hpp"
#include "headunit/preview_touch.hpp"
#include "headunit/native_cluster.hpp"
#include "headunit/socket_vehicle.hpp"
#include <cstdlib>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include <wayland-client.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace headunit {
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}  // namespace
struct WaylandPreview::Impl {
  PreviewTouch input;
  unsigned width, height;
  bool cluster = false;
  std::string display_name, media_file, page_file, output_name;
  std::atomic<ClusterCenter> center{ClusterCenter::navigation};
  Clock::time_point next_page_read{};
  std::unique_ptr<VehicleSource> vehicle;
  struct BackBuffer {
    wl_buffer* handle = nullptr;
    bool busy = false;
  };
  std::array<BackBuffer, 3> backs{};
  void* mapped = MAP_FAILED;
  std::size_t mapped_size = 0;
  unsigned buffer_width = 0, buffer_height = 0;
  std::atomic<std::uint64_t> paint_size{0};
  std::atomic<bool> paint_failed{false};
  std::mutex paint_mutex;
  struct Frame {
    unsigned width, height;
    std::vector<std::uint32_t> pixels;
  };
  std::optional<Frame> pending_frame;
  std::jthread painter;
  NativeClusterConfiguration cluster_configuration;
  std::unique_ptr<GreetingSequenceFile> greeting;
  std::atomic<std::shared_ptr<RouteGuidance>> navigation;

  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  struct Output {
    wl_output* handle = nullptr;
    std::string name;
  };
  std::vector<std::unique_ptr<Output>> outputs;
  wl_compositor* compositor = nullptr;
  wl_subcompositor* subcompositor = nullptr;
  wl_shm* shm = nullptr;
  wl_seat* seat = nullptr;
  wl_touch* touch = nullptr;
  wl_pointer* pointer = nullptr;
  xdg_wm_base* shell = nullptr;
  wp_viewporter* viewporter = nullptr;
  wp_viewport* viewport = nullptr;
  wl_surface* surface = nullptr;
  xdg_surface* xsurface = nullptr;
  xdg_toplevel* toplevel = nullptr;
  wl_buffer* background = nullptr;
  // Cluster only: the phone's map renders into this surface, stacked below
  // the window so the native instruments draw over it on a transparent layer.
  wl_surface* map_surface = nullptr;
  wl_subsurface* map_subsurface = nullptr;
  wp_viewport* map_viewport = nullptr;
  GstElement* sink = nullptr;  // Borrowed; released by pipeline before our destructor.
  bool configured = false, changed = false, closed = false, bad = false, pointer_here = false;
  double pointer_x = 0, pointer_y = 0;
  explicit Impl(unsigned w, unsigned h, bool native, const std::string& name)
      : input(w, h), width(w), height(h), cluster(native), display_name(name) {
    if (cluster) {
      if (const auto* path = std::getenv("HEADUNIT_GREETING_SEQUENCE_FILE"))
        greeting = std::make_unique<GreetingSequenceFile>(path);
      if (const auto* path = std::getenv("HEADUNIT_NATIVE_CLUSTER_CONFIG"))
        cluster_configuration = load_native_cluster_configuration(path);
      if (const auto* path = std::getenv("HEADUNIT_VEHICLE_SOCKET"))
        vehicle = std::make_unique<SocketVehicle>(path);
      else if (const auto* simulate = std::getenv("HEADUNIT_SIMULATE_VEHICLE");
               simulate && std::string_view(simulate) == "1")
        vehicle = std::make_unique<SimulatedVehicle>(Clock::now());
      if (const auto* path = std::getenv("HEADUNIT_MEDIA_STATE")) media_file = path;
      if (const auto* path = std::getenv("HEADUNIT_CLUSTER_PAGE")) page_file = path;
      if (const auto* name = std::getenv("HEADUNIT_STANDARD_CLUSTER_OUTPUT")) output_name = name;
    } else if (const auto* name = std::getenv("HEADUNIT_MAIN_OUTPUT"))
      output_name = name;
  }
  void clear_buffers() {
    for (auto& b : backs) {
      if (b.handle) wl_buffer_destroy(b.handle);
      b = {};
    }
    if (mapped != MAP_FAILED) munmap(mapped, mapped_size);
    mapped = MAP_FAILED;
  }
  void paint_cluster() {
    if (!cluster || !configured) return;
    const auto selected_at = Clock::now();
    if (selected_at >= next_page_read) {
      next_page_read = selected_at + std::chrono::milliseconds(100);
      const auto selected = selected_cluster_center(page_file, true);
      if (selected != center.exchange(selected) && sink) map_rectangle();
    }
    require(!paint_failed, "native cluster renderer failed");
    require(width >= 640 && width <= 4096 && height >= 240 && height <= 2160,
            "native cluster output size outside limits");
    paint_size.store((std::uint64_t(width) << 32) | height);
    if (!painter.joinable())
      painter = std::jthread([this](std::stop_token stop) {
        try {
          TurnSignalAnimation signals;
          while (!stop.stop_requested()) {
            const auto now = Clock::now();
            const auto size = paint_size.load();
            const auto route = navigation.load();
            Frame frame{unsigned(size >> 32), unsigned(size), {}};
            frame.pixels.resize(std::size_t(frame.width) * frame.height);
            const auto sample = vehicle ? vehicle->sample(now) : VehicleSample{};
            const auto lamps = signals.update(sample, now);
            draw_native_cluster(
                {sample, media_file, route ? route->snapshot(now) : NavigationSnapshot{},
                 center.load(), true, cluster_configuration,
                 greeting ? greeting->frame(now) : GreetingFrame{}, &lamps},
                now, frame.pixels.data(), frame.width, frame.height, std::size_t(frame.width) * 4);
            {
              std::lock_guard lock(paint_mutex);
              pending_frame = std::move(frame);
            }
            std::this_thread::sleep_until(now +
                                          std::chrono::milliseconds(lamps.animating ? 20 : 50));
          }
        } catch (...) {
          paint_failed = true;
        }
      });
    if (buffer_width != width || buffer_height != height) {
      require(width >= 640 && width <= 4096 && height >= 240 && height <= 2160,
              "native cluster output size outside limits");
      clear_buffers();
      buffer_width = width;
      buffer_height = height;
      mapped_size = std::size_t(width) * height * 4 * backs.size();
      char path[] = "/dev/shm/headunit-native-cluster-XXXXXX";
      const int fd = mkstemp(path);
      require(fd >= 0, "cluster buffer allocation failed");
      unlink(path);
      if (ftruncate(fd, off_t(mapped_size)) != 0) {
        close(fd);
        throw std::runtime_error("cluster buffer size failed");
      }
      mapped = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mapped == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("cluster mapping failed");
      }
      auto* pool = wl_shm_create_pool(shm, fd, int(mapped_size));
      close(fd);
      require(pool, "cluster pool failed");
      for (std::size_t i = 0; i < backs.size(); ++i) {
        auto& b = backs[i];
        b.handle = wl_shm_pool_create_buffer(pool, int(i * width * height * 4), width, height,
                                             width * 4, WL_SHM_FORMAT_ARGB8888);
        require(b.handle, "cluster buffer failed");
        static const wl_buffer_listener release{[](void* p, wl_buffer*) {
          static_cast<BackBuffer*>(p)->busy = false;
        }};
        wl_buffer_add_listener(b.handle, &release, &b);
      }
      wl_shm_pool_destroy(pool);
    }
    const auto free =
        std::find_if(backs.begin(), backs.end(), [](const auto& b) { return !b.busy; });
    if (free == backs.end()) return;
    std::optional<Frame> ready;
    {
      std::lock_guard lock(paint_mutex);
      ready = std::move(pending_frame);
      pending_frame.reset();
    }
    if (!ready || ready->width != width || ready->height != height) return;
    auto* pixels =
        static_cast<uint32_t*>(mapped) + (free - backs.begin()) * std::size_t(width) * height;
    std::copy(ready->pixels.begin(), ready->pixels.end(), pixels);
    free->busy = true;
    wl_surface_attach(surface, free->handle, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    // waylandsink positions its own subsurfaces inside the map surface; they
    // take effect on that parent's commit, which only this owner makes.
    wl_surface_commit(map_surface);
  }

  ~Impl() {
    if (painter.joinable()) {
      painter.request_stop();
      painter.join();
    }
    clear_buffers();
    if (touch) wl_touch_destroy(touch);
    if (pointer) wl_pointer_destroy(pointer);
    if (seat) wl_seat_destroy(seat);
    if (background) wl_buffer_destroy(background);
    if (map_viewport) wp_viewport_destroy(map_viewport);
    if (map_subsurface) wl_subsurface_destroy(map_subsurface);
    if (map_surface) wl_surface_destroy(map_surface);
    if (subcompositor) wl_subcompositor_destroy(subcompositor);
    if (viewport) wp_viewport_destroy(viewport);
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (xsurface) xdg_surface_destroy(xsurface);
    if (surface) wl_surface_destroy(surface);
    if (viewporter) wp_viewporter_destroy(viewporter);
    if (shell) xdg_wm_base_destroy(shell);
    if (shm) wl_shm_destroy(shm);
    if (compositor) wl_compositor_destroy(compositor);
    for (auto& output : outputs)
      if (output->handle) wl_output_destroy(output->handle);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
  }
  template <class F>
  void callback(F fn) noexcept {
    try {
      fn();
    } catch (...) {
      bad = true;
    }
  }
  void devices(std::uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !touch) {
      touch = wl_seat_get_touch(seat);
      static const wl_touch_listener listener{
          [](void* p, wl_touch*, std::uint32_t, std::uint32_t, wl_surface* s, std::int32_t id,
             wl_fixed_t x, wl_fixed_t y) {
            auto& w = *static_cast<Impl*>(p);
            w.callback([&] {
              if (s == w.surface) w.input.down(id, wl_fixed_to_double(x), wl_fixed_to_double(y));
            });
          },
          [](void* p, wl_touch*, std::uint32_t, std::uint32_t, std::int32_t id) {
            auto& w = *static_cast<Impl*>(p);
            w.callback([&] { w.input.up(id); });
          },
          [](void* p, wl_touch*, std::uint32_t, std::int32_t id, wl_fixed_t x, wl_fixed_t y) {
            auto& w = *static_cast<Impl*>(p);
            w.callback([&] { w.input.motion(id, wl_fixed_to_double(x), wl_fixed_to_double(y)); });
          },
          [](void*, wl_touch*) {},
          [](void* p, wl_touch*) {
            auto& w = *static_cast<Impl*>(p);
            w.callback([&] { w.input.cancel(); });
          },
          nullptr,
          nullptr};
      wl_touch_add_listener(touch, &listener, this);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && touch) {
      input.cancel();
      wl_touch_destroy(touch);
      touch = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
      pointer = wl_seat_get_pointer(seat);
      static const wl_pointer_listener listener = [] {
        wl_pointer_listener l{};
        l.enter = [](void* p, wl_pointer* ptr, std::uint32_t serial, wl_surface* s, wl_fixed_t x,
                     wl_fixed_t y) {
          auto& w = *static_cast<Impl*>(p);
          w.pointer_here = s == w.surface;
          w.pointer_x = wl_fixed_to_double(x);
          w.pointer_y = wl_fixed_to_double(y);
          if (w.pointer_here) wl_pointer_set_cursor(ptr, serial, nullptr, 0, 0);
        };
        l.leave = [](void* p, wl_pointer*, std::uint32_t, wl_surface*) {
          auto& w = *static_cast<Impl*>(p);
          w.pointer_here = false;
          w.callback([&] { w.input.up(-1); });
        };
        l.motion = [](void* p, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
          auto& w = *static_cast<Impl*>(p);
          w.pointer_x = wl_fixed_to_double(x);
          w.pointer_y = wl_fixed_to_double(y);
          w.callback([&] {
            if (w.pointer_here) w.input.motion(-1, w.pointer_x, w.pointer_y);
          });
        };
        l.button = [](void* p, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t button,
                      std::uint32_t state) {
          auto& w = *static_cast<Impl*>(p);
          w.callback([&] {
            if (button != 0x110) return;
            if (state == WL_POINTER_BUTTON_STATE_PRESSED && w.pointer_here)
              w.input.down(-1, w.pointer_x, w.pointer_y);
            else
              w.input.up(-1);
          });
        };
        l.axis = [](void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {
        };
        return l;
      }();
      wl_pointer_add_listener(pointer, &listener, this);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
      input.up(-1);
      wl_pointer_destroy(pointer);
      pointer = nullptr;
      pointer_here = false;
    }
  }
  void dispatch(int timeout) {
    require(!bad && !closed, "CarPlay Wayland surface closed or failed");
    while (wl_display_prepare_read(display) != 0)
      require(wl_display_dispatch_pending(display) >= 0, "Wayland input dispatch failed");
    const auto flushed = wl_display_flush(display);
    if (flushed < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      throw std::runtime_error("Wayland input flush failed");
    }
    pollfd fd{wl_display_get_fd(display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
    const int n = poll(&fd, 1, timeout);
    if (n > 0 && (fd.revents & POLLIN))
      require(wl_display_read_events(display) >= 0, "Wayland input read failed");
    else
      wl_display_cancel_read(display);
    require(n >= 0 || errno == EINTR, "Wayland input poll failed");
    require(!(fd.revents & (POLLHUP | POLLERR | POLLNVAL)), "Wayland input connection lost");
    require(wl_display_dispatch_pending(display) >= 0 && !bad && !closed,
            "Wayland input event failed");
    if (configured && changed) resize();
    paint_cluster();
  }
  void resize() {
    input.resize(width, height);
    wp_viewport_set_destination(viewport, int(width), int(height));
    xdg_surface_set_window_geometry(xsurface, 0, 0, int(width), int(height));
    if (sink) map_rectangle();
    if (cluster) {
      wp_viewport_set_destination(map_viewport, int(width), int(height));
      wl_surface_attach(map_surface, background, 0, 0);
      wl_surface_damage(map_surface, 0, 0, int(width), int(height));
      wl_surface_commit(map_surface);
    } else
      wl_surface_attach(surface, background, 0, 0);
    wl_surface_damage(surface, 0, 0, int(width), int(height));
    wl_surface_commit(surface);
    changed = false;
  }
  void map_rectangle() {
    if (!sink) return;
    const auto ordinary = input.rectangle();
    const auto r = cluster
                       ? native_cluster_map(width, height)
                       : ClusterMapRect{ordinary.x, ordinary.y, ordinary.width, ordinary.height};
    // Keep the stream alive with a valid destination that is entirely clipped
    // outside the window while Media or Vehicle owns the center. A 1x1 target
    // causes waylandsink to submit an invalid zero-sized viewport.
    const bool map = cluster && center.load() == ClusterCenter::navigation;
    require(gst_video_overlay_set_render_rectangle(
                GST_VIDEO_OVERLAY(sink), cluster && !map ? -64 : r.x, cluster && !map ? -64 : r.y,
                cluster && !map ? 64 : r.width, cluster && !map ? 64 : r.height),
            "Wayland video rectangle rejected");
    gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
  }
  void connect() {
    display = wl_display_connect(display_name.empty() ? nullptr : display_name.c_str());
    require(display, "cannot connect CarPlay Wayland surface");
    registry = wl_display_get_registry(display);
    require(registry, "missing Wayland registry");
    static const wl_registry_listener listener{
        [](void* p, wl_registry* r, std::uint32_t id, const char* type, std::uint32_t version) {
          auto& w = *static_cast<Impl*>(p);
          if (!strcmp(type, "wl_compositor") && !w.compositor)
            w.compositor = static_cast<wl_compositor*>(
                wl_registry_bind(r, id, &wl_compositor_interface, std::min(version, 4u)));
          else if (!strcmp(type, "wl_subcompositor") && !w.subcompositor)
            w.subcompositor = static_cast<wl_subcompositor*>(
                wl_registry_bind(r, id, &wl_subcompositor_interface, 1));
          else if (!strcmp(type, "wl_shm") && !w.shm)
            w.shm = static_cast<wl_shm*>(wl_registry_bind(r, id, &wl_shm_interface, 1));
          else if (!strcmp(type, "xdg_wm_base") && !w.shell)
            w.shell = static_cast<xdg_wm_base*>(wl_registry_bind(r, id, &xdg_wm_base_interface, 1));
          else if (!strcmp(type, "wp_viewporter") && !w.viewporter)
            w.viewporter =
                static_cast<wp_viewporter*>(wl_registry_bind(r, id, &wp_viewporter_interface, 1));
          else if (!strcmp(type, "wl_seat") && !w.seat) {
            w.seat = static_cast<wl_seat*>(wl_registry_bind(r, id, &wl_seat_interface, 1));
            static const wl_seat_listener seat_listener{[](void* p, wl_seat*, std::uint32_t c) {
                                                          auto& w = *static_cast<Impl*>(p);
                                                          w.callback([&] { w.devices(c); });
                                                        },
                                                        nullptr};
            wl_seat_add_listener(w.seat, &seat_listener, &w);
          } else if (!strcmp(type, "wl_output") && !w.output_name.empty() && version >= 4) {
            auto output = std::make_unique<Output>();
            output->handle =
                static_cast<wl_output*>(wl_registry_bind(r, id, &wl_output_interface, 4));
            require(output->handle, "cannot bind configured cluster output");
            static const wl_output_listener output_listener = [] {
              wl_output_listener l{};
              l.geometry = [](void*, wl_output*, std::int32_t, std::int32_t, std::int32_t,
                              std::int32_t, std::int32_t, const char*, const char*, std::int32_t) {
              };
              l.mode = [](void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t,
                          std::int32_t) {
              };
              l.done = [](void*, wl_output*) {
              };
              l.scale = [](void*, wl_output*, std::int32_t) {
              };
              l.name = [](void* p, wl_output*, const char* name) {
                static_cast<Output*>(p)->name = name ? name : "";
              };
              l.description = [](void*, wl_output*, const char*) {
              };
              return l;
            }();
            wl_output_add_listener(output->handle, &output_listener, output.get());
            w.outputs.push_back(std::move(output));
          }
        },
        [](void*, wl_registry*, std::uint32_t) {
        }};
    wl_registry_add_listener(registry, &listener, this);
    require(wl_display_roundtrip(display) >= 0 && compositor && shm && shell && viewporter,
            "CarPlay requires Wayland xdg-shell and viewporter");
    require(!cluster || subcompositor, "native cluster requires Wayland subsurfaces");
    wl_output* fullscreen_output = nullptr;
    if (!output_name.empty()) {
      require(wl_display_roundtrip(display) >= 0, "cannot query configured cluster output");
      for (const auto& output : outputs)
        if (output->name == output_name) {
          require(!fullscreen_output, "duplicate configured cluster output");
          fullscreen_output = output->handle;
        }
      require(fullscreen_output, "configured cluster output unavailable");
    }
    static const xdg_wm_base_listener shell_listener{
        [](void*, xdg_wm_base* s, std::uint32_t serial) {
          xdg_wm_base_pong(s, serial);
        }};
    xdg_wm_base_add_listener(shell, &shell_listener, this);
    surface = wl_compositor_create_surface(compositor);
    require(surface, "cannot create CarPlay surface");
    viewport = wp_viewporter_get_viewport(viewporter, surface);
    require(viewport, "cannot create CarPlay viewport");
    xsurface = xdg_wm_base_get_xdg_surface(shell, surface);
    require(xsurface, "cannot create CarPlay shell surface");
    static const xdg_surface_listener surface_listener{
        [](void* p, xdg_surface* s, std::uint32_t serial) {
          auto& w = *static_cast<Impl*>(p);
          xdg_surface_ack_configure(s, serial);
          w.configured = true;
          w.changed = true;
        }};
    xdg_surface_add_listener(xsurface, &surface_listener, this);
    toplevel = xdg_surface_get_toplevel(xsurface);
    require(toplevel, "cannot create CarPlay window");
    static const xdg_toplevel_listener top_listener{
        [](void* p, xdg_toplevel*, std::int32_t width, std::int32_t height, wl_array*) {
          auto& w = *static_cast<Impl*>(p);
          if (width < 0 || height < 0 || width > 16384 || height > 16384) {
            w.bad = true;
            return;
          }
          if (width > 0) w.width = unsigned(width);
          if (height > 0) w.height = unsigned(height);
        },
        [](void* p, xdg_toplevel*) { static_cast<Impl*>(p)->closed = true; }, nullptr, nullptr};
    xdg_toplevel_add_listener(toplevel, &top_listener, this);
    xdg_toplevel_set_title(toplevel, cluster ? "Native cluster" : "CarPlay");
    xdg_toplevel_set_app_id(toplevel,
                            cluster ? "org.headunit.cluster" : "org.headunit.carplay.main");
    xdg_toplevel_set_fullscreen(toplevel, fullscreen_output);
    char path[] = "/dev/shm/headunit-preview-XXXXXX";
    int fd = mkstemp(path);
    require(fd >= 0, "cannot allocate CarPlay background");
    unlink(path);
    if (ftruncate(fd, 4) != 0) {
      close(fd);
      throw std::runtime_error("cannot size CarPlay background");
    }
    void* pixels = mmap(nullptr, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      throw std::runtime_error("cannot map CarPlay background");
    }
    *static_cast<std::uint32_t*>(pixels) = 0xff000000u;
    munmap(pixels, 4);
    auto* pool = wl_shm_create_pool(shm, fd, 4);
    close(fd);
    require(pool, "cannot create CarPlay buffer pool");
    background = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    require(background, "cannot create CarPlay background buffer");
    if (cluster) {
      map_surface = wl_compositor_create_surface(compositor);
      require(map_surface, "cannot create cluster map surface");
      map_subsurface = wl_subcompositor_get_subsurface(subcompositor, map_surface, surface);
      require(map_subsurface, "cannot create cluster map subsurface");
      map_viewport = wp_viewporter_get_viewport(viewporter, map_surface);
      require(map_viewport, "cannot create cluster map viewport");
      // Below the instruments, and desynchronized so phone video frames do
      // not wait for the 20 Hz instrument commits.
      wl_subsurface_place_below(map_subsurface, surface);
      wl_subsurface_set_desync(map_subsurface);
      wl_region* none = wl_compositor_create_region(compositor);
      require(none, "cannot create cluster map input region");
      wl_surface_set_input_region(map_surface, none);
      wl_region_destroy(none);
    }
    wl_surface_commit(surface);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!configured) {
      require(std::chrono::steady_clock::now() < deadline, "CarPlay Wayland configure timed out");
      dispatch(20);
    }
  }
};
WaylandPreview::WaylandPreview(unsigned width, unsigned height, bool cluster,
                               const std::string& display)
    : impl_(std::make_unique<Impl>(width, height, cluster, display)) {
  impl_->connect();
}
WaylandPreview::~WaylandPreview() = default;
void WaylandPreview::attach(GstElement* sink) {
  auto& w = *impl_;
  require(sink && GST_IS_VIDEO_OVERLAY(sink) && !w.sink, "invalid Wayland preview sink");
  auto* context = gst_context_new("GstWlDisplayHandleContextType", TRUE);
  gst_structure_set(gst_context_writable_structure(context), "display", G_TYPE_POINTER, w.display,
                    nullptr);
  gst_element_set_context(sink, context);
  gst_context_unref(context);
  gst_video_overlay_set_window_handle(
      GST_VIDEO_OVERLAY(sink), reinterpret_cast<guintptr>(w.cluster ? w.map_surface : w.surface));
  w.sink = sink;
  w.resize();
  wl_display_flush(w.display);
}
void WaylandPreview::set_navigation(std::shared_ptr<RouteGuidance> source) {
  impl_->navigation.store(std::move(source));
}
void WaylandPreview::pump() {
  impl_->dispatch(0);
}
int WaylandPreview::input_fd() const noexcept {
  return wl_display_get_fd(impl_->display);
}
std::vector<TouchInput> WaylandPreview::take_touches() {
  pump();
  return impl_->input.take();
}
}  // namespace headunit
