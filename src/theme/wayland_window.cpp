// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "wayland_window.hpp"
#include <headunit/theme/local_presentation_diagnostics.hpp>
#include <headunit/presentation_feedback_timeout.hpp>
#include <headunit/preview_touch.hpp>
#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "wlr-layer-shell-client-protocol.h"
#include <wayland-client.h>
#include <wayland-egl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <iostream>
#include <syncstream>
#include <poll.h>
#include <stdexcept>
#include <vector>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

namespace headunit::theme::graphics {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
std::uint64_t timestamp(clockid_t clock) {
  timespec ts{};
  check(::clock_gettime(clock, &ts) == 0 && ts.tv_sec >= 0 && ts.tv_nsec >= 0 &&
            ts.tv_nsec < 1000000000,
        "cannot sample presentation clock");
  check(std::uint64_t(ts.tv_sec) <=
            (std::numeric_limits<std::uint64_t>::max() - std::uint64_t(ts.tv_nsec)) / 1000000000,
        "presentation clock overflow");
  return std::uint64_t(ts.tv_sec) * 1000000000 + std::uint64_t(ts.tv_nsec);
}
video::ClockCorrelation correlate(std::uint32_t id) {
  // These are the two non-wall-clock domains supported by this Linux host.
  // Do not guess another clock's epoch or silently use the frame callback.
  check(id == CLOCK_MONOTONIC || id == CLOCK_MONOTONIC_RAW,
        "unsupported Wayland presentation clock domain");
  if (id == CLOCK_MONOTONIC) {
    // These are the same clock, so one real read is an exact correlation.
    // A three-read midpoint adds sampling jitter to an already monotonic
    // receipt and can reverse commits presented at the same compositor time.
    const auto sample = timestamp(CLOCK_MONOTONIC);
    return {id, sample, sample, sample};
  }
  // A scheduler pause between the three local reads can exceed the strict
  // 2 ms correlation span even when the compositor receipt is sound. Retry
  // that local measurement, retaining the narrowest actual sample. Never
  // synthesize a timestamp or widen PresentationClock's quality limit.
  video::ClockCorrelation best;
  std::uint64_t best_span = std::numeric_limits<std::uint64_t>::max();
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    video::ClockCorrelation sample;
    sample.clock_id = id;
    sample.before_ns = timestamp(CLOCK_MONOTONIC);
    sample.source_ns = timestamp(clockid_t(id));
    sample.after_ns = timestamp(CLOCK_MONOTONIC);
    check(sample.after_ns >= sample.before_ns, "presentation clock sample regressed");
    const auto span = sample.after_ns - sample.before_ns;
    if (span < best_span) {
      best = sample;
      best_span = span;
    }
    if (span <= 2000000) return sample;
  }
  return best;
}
}  // namespace
struct WaylandWindow::Impl {
  struct ShmBuffer {
    wl_buffer* buffer = nullptr;
    void* pixels = MAP_FAILED;
    std::size_t size = 0;
    bool busy = false;
    ~ShmBuffer() {
      if (buffer) wl_buffer_destroy(buffer);
      if (pixels != MAP_FAILED) ::munmap(pixels, size);
    }
  };
  std::unique_ptr<headunit::PreviewTouch> input;
  bool input_enabled = false, pointer_here = false;
  bool touch_diagnostics = false;
  bool opaque_background = false;
  void apply_opaque_region(bool opaque, bool force = false) {
    // Ordinary frames retain the committed policy without allocating a region
    // or sending extra requests. Configure must reapply the logical dimensions.
    if (!force && opaque == opaque_background) return;
    if (opaque) {
      auto* region = wl_compositor_create_region(compositor);
      check(region, "cannot create opaque Wayland region");
      wl_region_add(region, 0, 0, logical_width, logical_height);
      wl_surface_set_opaque_region(surface, region);
      wl_region_destroy(region);
    } else
      wl_surface_set_opaque_region(surface, nullptr);
    opaque_background = opaque;
  }
  unsigned diagnostic_downs = 0;
  double pointer_x = 0, pointer_y = 0;
  wl_seat* seat = nullptr;
  wl_touch* touch = nullptr;
  wl_pointer* pointer = nullptr;
  std::uint32_t seat_name = 0;
  wl_shm* shm = nullptr;
  std::array<std::unique_ptr<ShmBuffer>, 3> images;
  std::uint32_t width = 0, height = 0;
  std::uint32_t buffer_width = 0, buffer_height = 0, logical_width = 0, logical_height = 0,
                buffer_scale = 1;
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  struct Output {
    wl_output* handle = nullptr;
    std::string name;
  };
  std::vector<std::unique_ptr<Output>> outputs;
  std::string requested_output_name;
  xdg_wm_base* shell = nullptr;
  wl_surface* surface = nullptr;
  xdg_surface* xsurface = nullptr;
  xdg_toplevel* toplevel = nullptr;
  zwlr_layer_shell_v1* layer_shell = nullptr;
  zwlr_layer_surface_v1* layer_surface = nullptr;
  bool background_layer = false;
  LocalPresentationDiagnostics* local_diagnostics = nullptr;
  wl_egl_window* window = nullptr;
  wl_callback* sync = nullptr;
  wp_presentation* presentation = nullptr;
  struct Pending {
    Impl* owner = nullptr;
    struct wp_presentation_feedback* feedback = nullptr;
    bool resolved = false;
    std::chrono::steady_clock::time_point deadline{};
    PresentationReceipt receipt;
    ~Pending() {
      if (feedback) wp_presentation_feedback_destroy(feedback);
    }
  };
  std::deque<std::unique_ptr<Pending>> pending;
  unsigned max_pending = 1;
  bool synced = false, configured = false, closed = false, clock_known = false, malformed = false;
  std::uint32_t clock_id = 0;
  std::uint64_t last_token = 0;
  ~Impl() {
    if (touch) wl_touch_destroy(touch);
    if (pointer) wl_pointer_destroy(pointer);
    if (seat) wl_seat_destroy(seat);
    for (auto& image : images)
      image.reset();
    if (shm) wl_shm_destroy(shm);
    pending.clear();
    if (sync) wl_callback_destroy(sync);
    if (window) wl_egl_window_destroy(window);
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (layer_surface) zwlr_layer_surface_v1_destroy(layer_surface);
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (xsurface) xdg_surface_destroy(xsurface);
    if (surface) wl_surface_destroy(surface);
    if (presentation) wp_presentation_destroy(presentation);
    if (shell) xdg_wm_base_destroy(shell);
    if (compositor) wl_compositor_destroy(compositor);
    for (auto& output : outputs)
      if (output->handle) wl_output_destroy(output->handle);
    if (registry) wl_registry_destroy(registry);
    if (display) {
      wl_display_flush(display);
      wl_display_disconnect(display);
    }
  }
  template <class F>
  void callback(F fn) noexcept {
    try {
      fn();
    } catch (...) {
      malformed = true;
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
              if (w.input_enabled && s == w.surface) {
                const auto sx = wl_fixed_to_double(x), sy = wl_fixed_to_double(y);
                if (w.touch_diagnostics && w.diagnostic_downs < 16) {
                  ++w.diagnostic_downs;
                  const auto r = w.input->rectangle();
                  std::osyncstream(std::cerr)
                      << "Ultra touch surface=" << w.logical_width << 'x' << w.logical_height
                      << " canvas=" << w.width << 'x' << w.height << " rect=" << r.x << ',' << r.y
                      << ',' << r.width << ',' << r.height << " down=" << sx << ',' << sy << '\n';
                }
                w.input->down(id, sx, sy);
              }
            });
          },
          [](void* p, wl_touch*, std::uint32_t, std::uint32_t, std::int32_t id) {
            auto& w = *static_cast<Impl*>(p);
            w.callback([&] { w.input->up(id); });
          },
          [](void* p, wl_touch*, std::uint32_t, std::int32_t id, wl_fixed_t x, wl_fixed_t y) {
            auto& w = *static_cast<Impl*>(p);
            w.callback([&] { w.input->motion(id, wl_fixed_to_double(x), wl_fixed_to_double(y)); });
          },
          [](void*, wl_touch*) {},
          [](void* p, wl_touch*) {
            auto& w = *static_cast<Impl*>(p);
            w.callback([&] { w.input->cancel(); });
          },
          nullptr,
          nullptr};
      wl_touch_add_listener(touch, &listener, this);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && touch) {
      input->cancel();
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
          w.callback([&] { w.input->up(-1); });
        };
        l.motion = [](void* p, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
          auto& w = *static_cast<Impl*>(p);
          w.pointer_x = wl_fixed_to_double(x);
          w.pointer_y = wl_fixed_to_double(y);
          w.callback([&] {
            if (w.pointer_here) w.input->motion(-1, w.pointer_x, w.pointer_y);
          });
        };
        l.button = [](void* p, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t button,
                      std::uint32_t state) {
          auto& w = *static_cast<Impl*>(p);
          w.callback([&] {
            if (button != 0x110) return;
            if (state == WL_POINTER_BUTTON_STATE_PRESSED && w.input_enabled && w.pointer_here)
              w.input->down(-1, w.pointer_x, w.pointer_y);
            else
              w.input->up(-1);
          });
        };
        l.axis = [](void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {
        };
        return l;
      }();
      wl_pointer_add_listener(pointer, &listener, this);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
      input->up(-1);
      wl_pointer_destroy(pointer);
      pointer = nullptr;
      pointer_here = false;
    }
  }
  void dispatch(int timeout_ms) {
    if (local_diagnostics) local_diagnostics->last_dispatch_before_ns = local_diagnostic_clock();
    check(!closed && !malformed, "invalid Wayland video state or receipt");
    // Always cancel a prepared read when poll does not report readable data.
    // No other code reads this private connection on the owner's behalf.
    while (wl_display_prepare_read(display) != 0)
      check(wl_display_dispatch_pending(display) >= 0, "Wayland video dispatch failed");
    const int flushed = wl_display_flush(display);
    if (flushed < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      throw std::runtime_error("Wayland video flush failed");
    }
    pollfd fd{wl_display_get_fd(display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
    const auto n = ::poll(&fd, 1, timeout_ms);
    if (n > 0 && (fd.revents & POLLIN))
      check(wl_display_read_events(display) >= 0, "Wayland video read failed");
    else
      wl_display_cancel_read(display);
    check(n >= 0 || errno == EINTR, "Wayland video poll failed");
    check(!(fd.revents & (POLLHUP | POLLERR | POLLNVAL)), "Wayland video connection lost");
    check(wl_display_dispatch_pending(display) >= 0, "Wayland video dispatch failed");
    if (local_diagnostics) local_diagnostics->last_dispatch_after_ns = local_diagnostic_clock();
    check(!closed && !malformed, "invalid Wayland video state or receipt");
  }
  void pump(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
      check(!closed, "Wayland video window closed");
      check(std::chrono::steady_clock::now() < deadline,
            "Wayland video feedback deadline exceeded");
      dispatch(25);
    }
    check(!closed && !malformed, "invalid Wayland video state or receipt");
  }
  void connect(std::uint32_t width, std::uint32_t height, const std::string& title, unsigned scale,
               const std::string& display_name, bool accept_touch, const std::string& app_id,
               const std::string& output_name) {
    check(width && height && width <= 4096 && height <= 2160 && title.size() <= 256 &&
              title.find('\0') == title.npos,
          "invalid Wayland video options");
    this->width = width;
    this->height = height;
    check((scale == 1 || scale == 2) && width % scale == 0 && height % scale == 0,
          "invalid Wayland preview scale");
    buffer_width = width;
    buffer_height = height;
    buffer_scale = scale;
    logical_width = width / scale;
    logical_height = height / scale;
    if (accept_touch) {
      input = std::make_unique<headunit::PreviewTouch>(width, height);
      input->resize(logical_width, logical_height);
    }
    check(display_name.size() <= 100 && display_name.find('\0') == display_name.npos,
          "invalid Wayland display name");
    check(output_name.size() <= 100 && output_name.find('\0') == output_name.npos,
          "invalid Wayland output name");
    requested_output_name = output_name;
    display = wl_display_connect(display_name.empty() ? nullptr : display_name.c_str());
    check(display, "cannot connect to Wayland video display");
    registry = wl_display_get_registry(display);
    check(registry, "cannot obtain Wayland video registry");
    static const wp_presentation_listener clock_listener{
        [](void* data, wp_presentation*, std::uint32_t id) {
          auto& w = *static_cast<Impl*>(data);
          w.malformed |= w.clock_known && w.clock_id != id;
          w.clock_known = true;
          w.clock_id = id;
        }};
    static const wl_registry_listener registry_listener{
        [](void* data, wl_registry* registry, std::uint32_t name, const char* interface,
           std::uint32_t version) {
          auto& w = *static_cast<Impl*>(data);
          if (!std::strcmp(interface, "wl_compositor") && !w.compositor)
            w.compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
          else if (!std::strcmp(interface, "xdg_wm_base") && !w.shell)
            w.shell = static_cast<xdg_wm_base*>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
          else if (!std::strcmp(interface, "zwlr_layer_shell_v1") && !w.layer_shell && version >= 3)
            w.layer_shell = static_cast<zwlr_layer_shell_v1*>(
                wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 3));
          else if (!std::strcmp(interface, "wl_shm") && !w.shm)
            w.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
          else if (!std::strcmp(interface, "wl_seat") && w.input && !w.seat) {
            w.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
            w.seat_name = name;
            static const wl_seat_listener listener{[](void* p, wl_seat*, std::uint32_t caps) {
                                                     auto& w = *static_cast<Impl*>(p);
                                                     w.callback([&] { w.devices(caps); });
                                                   },
                                                   nullptr};
            wl_seat_add_listener(w.seat, &listener, &w);
          } else if (!std::strcmp(interface, "wl_output") && !w.requested_output_name.empty() &&
                     version >= 4) {
            auto output = std::make_unique<Output>();
            output->handle =
                static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 4));
            if (!output->handle) {
              w.malformed = true;
              return;
            }
            static const wl_output_listener listener = [] {
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
              l.name = [](void* data, wl_output*, const char* value) {
                static_cast<Output*>(data)->name = value ? value : "";
              };
              l.description = [](void*, wl_output*, const char*) {
              };
              return l;
            }();
            wl_output_add_listener(output->handle, &listener, output.get());
            w.outputs.push_back(std::move(output));
          } else if (!std::strcmp(interface, "wp_presentation") && !w.presentation) {
            w.presentation = static_cast<wp_presentation*>(
                wl_registry_bind(registry, name, &wp_presentation_interface, 1));
            if (w.presentation) wp_presentation_add_listener(w.presentation, &clock_listener, &w);
          }
        },
        [](void* p, wl_registry*, std::uint32_t name) {
          auto& w = *static_cast<Impl*>(p);
          if (w.seat && name == w.seat_name)
            w.callback([&] {
              w.devices(0);
              wl_seat_destroy(w.seat);
              w.seat = nullptr;
            });
        }};
    wl_registry_add_listener(registry, &registry_listener, this);
    sync = wl_display_sync(display);
    check(sync, "cannot synchronize Wayland video registry");
    static const wl_callback_listener sync_listener{[](void* data, wl_callback* cb, std::uint32_t) {
      auto& w = *static_cast<Impl*>(data);
      w.synced = true;
      w.sync = nullptr;
      wl_callback_destroy(cb);
    }};
    wl_callback_add_listener(sync, &sync_listener, this);
    pump([&] { return synced; });
    check(compositor && (background_layer ? bool(layer_shell) : bool(shell)) && presentation,
          "Wayland video requires xdg-shell and presentation-time");
    wl_output* fullscreen_output = nullptr;
    if (!requested_output_name.empty()) {
      // A second roundtrip receives the initial wl_output.name events. The
      // first sync only guarantees delivery of the registry globals.
      synced = false;
      sync = wl_display_sync(display);
      check(sync, "cannot synchronize Wayland outputs");
      wl_callback_add_listener(sync, &sync_listener, this);
      pump([&] { return synced; });
      for (const auto& output : outputs)
        if (output->name == requested_output_name) {
          check(!fullscreen_output, "duplicate Wayland output name");
          fullscreen_output = output->handle;
        }
      check(fullscreen_output, "configured Wayland output is unavailable");
    }
    static const xdg_wm_base_listener shell_listener{
        [](void*, xdg_wm_base* shell, std::uint32_t serial) {
          xdg_wm_base_pong(shell, serial);
        }};
    if (shell) xdg_wm_base_add_listener(shell, &shell_listener, this);
    surface = wl_compositor_create_surface(compositor);
    check(surface, "cannot create Wayland video surface");
    if (scale != 1) {
      check(wl_surface_get_version(surface) >= 3, "Wayland compositor lacks buffer scaling");
      wl_surface_set_buffer_scale(surface, scale);
    }
    if (background_layer) {
      layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, fullscreen_output,
                                                            ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
                                                            "headunit.backup-cluster");
      check(layer_surface, "cannot create local cluster background");
      static const zwlr_layer_surface_v1_listener listener{
          [](void* data, zwlr_layer_surface_v1* surface, std::uint32_t serial, std::uint32_t width,
             std::uint32_t height) {
            auto& w = *static_cast<Impl*>(data);
            w.callback([&] {
              check(width == w.logical_width && height == w.logical_height,
                    "local cluster background size mismatch");
              zwlr_layer_surface_v1_ack_configure(surface, serial);
              w.apply_opaque_region(true, true);
              w.configured = true;
            });
          },
          [](void* data, zwlr_layer_surface_v1*) {
            static_cast<Impl*>(data)->closed = true;
          }};
      zwlr_layer_surface_v1_add_listener(layer_surface, &listener, this);
      zwlr_layer_surface_v1_set_size(layer_surface, logical_width, logical_height);
      zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                          ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                          ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                          ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
      zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
      zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 0);
      auto* empty = wl_compositor_create_region(compositor);
      check(empty, "cannot create empty local cluster input region");
      wl_surface_set_input_region(surface, empty);
      wl_region_destroy(empty);
    } else {
      xsurface = xdg_wm_base_get_xdg_surface(shell, surface);
      check(xsurface, "cannot create Wayland video xdg surface");
      static const xdg_surface_listener surface_listener{
          [](void* data, xdg_surface* surface, std::uint32_t serial) {
            auto& w = *static_cast<Impl*>(data);
            if (w.malformed) return;
            xdg_surface_ack_configure(surface, serial);
            if (w.input && (w.buffer_width != w.logical_width * w.buffer_scale ||
                            w.buffer_height != w.logical_height * w.buffer_scale))
              w.callback([&] { w.input->resize(w.logical_width, w.logical_height); });
            w.buffer_width = w.logical_width * w.buffer_scale;
            w.buffer_height = w.logical_height * w.buffer_scale;
            xdg_surface_set_window_geometry(surface, 0, 0, w.logical_width, w.logical_height);
            w.callback([&] { w.apply_opaque_region(w.opaque_background, true); });
            if (w.window) wl_egl_window_resize(w.window, w.buffer_width, w.buffer_height, 0, 0);
            w.configured = true;
          }};
      xdg_surface_add_listener(xsurface, &surface_listener, this);
      toplevel = xdg_surface_get_toplevel(xsurface);
      check(toplevel, "cannot create Wayland video toplevel");
      static const xdg_toplevel_listener top_listener{
          [](void* data, xdg_toplevel*, std::int32_t width, std::int32_t height, wl_array*) {
            auto& w = *static_cast<Impl*>(data);
            if (width < 0 || height < 0 || width > 4096 / int(w.buffer_scale) ||
                height > 2160 / int(w.buffer_scale)) {
              w.malformed = true;
              return;
            }
            if (width) w.logical_width = width;
            if (height) w.logical_height = height;
          },
          [](void* data, xdg_toplevel*) { static_cast<Impl*>(data)->closed = true; }, nullptr,
          nullptr};
      xdg_toplevel_add_listener(toplevel, &top_listener, this);
      check(!app_id.empty() && app_id.size() <= 127 && app_id.find('\0') == app_id.npos,
            "invalid Wayland app ID");
      xdg_toplevel_set_title(toplevel, title.c_str());
      xdg_toplevel_set_app_id(toplevel, app_id.c_str());
      if (fullscreen_output) xdg_toplevel_set_fullscreen(toplevel, fullscreen_output);
      xdg_surface_set_window_geometry(xsurface, 0, 0, width / scale, height / scale);
    }
    wl_surface_commit(surface);
    pump([&] { return configured && clock_known; });
    window = wl_egl_window_create(surface, buffer_width, buffer_height);
    check(window, "cannot create Wayland video EGL window");
  }
};
WaylandWindow::WaylandWindow(std::uint32_t width, std::uint32_t height, const std::string& title,
                             unsigned scale, const std::string& display_name, bool accept_touch,
                             unsigned max_pending, const std::string& app_id,
                             const std::string& output_name, bool touch_diagnostics,
                             bool opaque_background, bool background_layer,
                             LocalPresentationDiagnostics* local_diagnostics)
    : impl_(std::make_unique<Impl>()) {
  check(max_pending >= 1 && max_pending <= 2, "invalid Wayland presentation queue bound");
  impl_->max_pending = max_pending;
  impl_->touch_diagnostics = touch_diagnostics;
  check(!background_layer || !accept_touch, "local cluster background cannot accept touch");
  impl_->background_layer = background_layer;
  impl_->local_diagnostics = background_layer ? local_diagnostics : nullptr;
  impl_->opaque_background = opaque_background;
  impl_->connect(width, height, title, scale, display_name, accept_touch, app_id, output_name);
}
WaylandWindow::~WaylandWindow() = default;
void* WaylandWindow::display() const {
  return impl_->display;
}
void* WaylandWindow::window() const {
  return impl_->window;
}
std::pair<std::uint32_t, std::uint32_t> WaylandWindow::buffer_size() const {
  return {impl_->buffer_width, impl_->buffer_height};
}
void WaylandWindow::request(std::uint64_t token, bool opaque_background) {
  auto& w = *impl_;
  check(token > w.last_token && w.pending.size() < w.max_pending && !w.closed && !w.malformed,
        "invalid or concurrent Wayland video submission");
  auto item = std::make_unique<Impl::Pending>();
  item->owner = &w;
  item->receipt.token = token;
  item->receipt.clock_id = w.clock_id;
  item->feedback = wp_presentation_feedback(w.presentation, w.surface);
  check(item->feedback, "cannot request Wayland video feedback");
  item->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  static const struct wp_presentation_feedback_listener listener{
      [](void*, struct wp_presentation_feedback*, wl_output*) {},
      [](void* data, struct wp_presentation_feedback* feedback, std::uint32_t high,
         std::uint32_t low, std::uint32_t ns, std::uint32_t refresh, std::uint32_t seq_high,
         std::uint32_t seq_low, std::uint32_t flags) {
        auto& item = *static_cast<Impl::Pending*>(data);
        item.receipt.seconds = (std::uint64_t(high) << 32) | low;
        item.receipt.nanoseconds = ns;
        item.receipt.refresh_ns = refresh;
        item.receipt.sequence = (std::uint64_t(seq_high) << 32) | seq_low;
        item.receipt.flags = flags;
        item.owner->malformed |= ns >= 1000000000u || (flags & ~15u);
        item.receipt.presented = !item.owner->malformed;
        if (auto* diagnostic = item.owner->local_diagnostics;
            diagnostic && !diagnostic->first_feedback)
          diagnostic->first_feedback =
              LocalPresentationDiagnostics::Feedback{item.receipt, false, local_diagnostic_clock()};
        item.resolved = true;
        item.feedback = nullptr;
        wp_presentation_feedback_destroy(feedback);
      },
      [](void* data, struct wp_presentation_feedback* feedback) {
        auto& item = *static_cast<Impl::Pending*>(data);
        if (auto* diagnostic = item.owner->local_diagnostics;
            diagnostic && !diagnostic->first_feedback)
          diagnostic->first_feedback =
              LocalPresentationDiagnostics::Feedback{item.receipt, true, local_diagnostic_clock()};
        item.resolved = true;
        item.feedback = nullptr;
        wp_presentation_feedback_destroy(feedback);
      }};
  if (wp_presentation_feedback_add_listener(item->feedback, &listener, item.get()) < 0) {
    throw std::runtime_error("cannot install Wayland feedback listener");
  }
  w.pending.push_back(std::move(item));
  if (w.local_diagnostics && !w.local_diagnostics->first_request)
    w.local_diagnostics->first_request =
        LocalPresentationDiagnostics::Request{token, local_diagnostic_clock()};
  w.apply_opaque_region(opaque_background);
  w.last_token = token;
}
void WaylandWindow::observe_egl_swap_return(std::uint64_t token, bool success) noexcept {
  auto* diagnostic = impl_->local_diagnostics;
  if (diagnostic && !diagnostic->first_swap_return)
    diagnostic->first_swap_return =
        LocalPresentationDiagnostics::SwapReturn{token, success, local_diagnostic_clock()};
}
std::vector<headunit::TouchInput> WaylandWindow::take_touches(bool enabled) {
  auto& w = *impl_;
  if (!w.input) return {};
  w.input_enabled = enabled;
  if (!enabled) w.input->cancel();
  w.dispatch(0);
  auto reports = w.input->take();
  if (!enabled) std::erase_if(reports, [](const auto& r) { return r.pressed; });
  return reports;
}
bool WaylandWindow::pending() const {
  return !impl_->pending.empty();
}
bool WaylandWindow::can_submit() const {
  return impl_->pending.size() < impl_->max_pending;
}
void WaylandWindow::set_max_pending(unsigned max_pending) {
  check(max_pending >= 1 && max_pending <= 2, "invalid Wayland presentation queue bound");
  impl_->max_pending = max_pending;
}
void WaylandWindow::submit_image(std::uint64_t token, const Image& image, bool opaque_background) {
  auto& w = *impl_;
  check(w.shm && image.width == w.width && image.height == w.height &&
            image.pixels.size() == std::size_t(w.width) * w.height,
        "invalid Wayland preview image");
  w.dispatch(0);
  const auto slot =
      std::find_if(w.images.begin(), w.images.end(), [](const auto& b) { return !b || !b->busy; });
  check(slot != w.images.end(), "Wayland preview buffers were not released");
  if (!*slot) {
    auto b = std::make_unique<Impl::ShmBuffer>();
    b->size = image.pixels.size() * sizeof(std::uint32_t);
    const int fd = ::memfd_create("headunit-ultra-preview", MFD_CLOEXEC);
    check(fd >= 0, "cannot allocate private Wayland preview memory");
    const bool sized = ::ftruncate(fd, b->size) == 0;
    if (sized) b->pixels = ::mmap(nullptr, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    wl_shm_pool* pool = nullptr;
    if (b->pixels != MAP_FAILED) pool = wl_shm_create_pool(w.shm, fd, b->size);
    ::close(fd);
    check(pool, "cannot map Wayland preview memory");
    b->buffer =
        wl_shm_pool_create_buffer(pool, 0, w.width, w.height, w.width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    check(b->buffer, "cannot create Wayland preview buffer");
    static const wl_buffer_listener listener{[](void* data, wl_buffer*) {
      static_cast<Impl::ShmBuffer*>(data)->busy = false;
    }};
    wl_buffer_add_listener(b->buffer, &listener, b.get());
    *slot = std::move(b);
  }
  auto& b = **slot;
  request(token, opaque_background);
  std::memcpy(b.pixels, image.pixels.data(), b.size);
  b.busy = true;
  wl_surface_attach(w.surface, b.buffer, 0, 0);
  wl_surface_damage(w.surface, 0, 0, w.width, w.height);
  wl_surface_commit(w.surface);
  const int flushed = wl_display_flush(w.display);
  check(flushed >= 0 || errno == EAGAIN, "Wayland preview flush failed");
}
bool WaylandWindow::feedback_delayed() const {
  return !impl_->pending.empty() &&
         std::chrono::steady_clock::now() >= impl_->pending.front()->deadline;
}
std::optional<PresentationReceipt> WaylandWindow::poll_receipt(bool wait_for_visibility) {
  auto& w = *impl_;
  // A hidden desktop surface can legitimately keep one content update pending.
  // Never synthesize a receipt or issue more commits to make its timer pass.
  if (!wait_for_visibility && !w.pending.empty() && feedback_delayed()) {
    if (w.local_diagnostics && !w.local_diagnostics->timeout)
      w.local_diagnostics->timeout = LocalPresentationDiagnostics::Timeout{
          w.pending.front()->receipt.token, unsigned(w.pending.size()), w.pending.front()->resolved,
          local_diagnostic_clock()};
    if (!w.pending.front()->resolved) throw headunit::PresentationFeedbackTimeout();
    throw std::runtime_error("Wayland video feedback deadline exceeded");
  }
  w.dispatch(0);
  if (w.pending.empty() || !w.pending.front()->resolved) return {};
  auto& item = *w.pending.front();
  if (item.receipt.presented) {
    const auto correlation = correlate(item.receipt.clock_id);
    // Keep this exact receipt pending until an actual clock read reaches its
    // reported time. No sleep, new commit, invented timestamp or duplicate PTS.
    if (!video::presentation_time_reached(item.receipt, correlation)) return {};
    item.receipt.correlation = correlation;
  }
  item.receipt.received_ns = timestamp(CLOCK_MONOTONIC);
  auto receipt = item.receipt;
  if (w.local_diagnostics && !w.local_diagnostics->first_returned_receipt)
    w.local_diagnostics->first_returned_receipt = receipt;
  w.pending.pop_front();
  return receipt;
}
PresentationReceipt WaylandWindow::wait() {
  auto& w = *impl_;
  check(!w.pending.empty(), "no Wayland video submission");
  for (;;) {
    if (auto receipt = poll_receipt()) return *receipt;
    w.dispatch(10);
  }
}
}  // namespace headunit::theme::graphics
