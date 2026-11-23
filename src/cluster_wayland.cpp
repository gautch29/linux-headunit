// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster.hpp>
#ifdef HEADUNIT_HAVE_LOCAL_ULTRA_CLUSTER
#include <headunit/theme/local_cluster.hpp>
#endif
#include <headunit/cluster_return.hpp>
#include <csignal>
#include <headunit/socket_vehicle.hpp>
#include <headunit/prephone_publication.hpp>
#include <headunit/dated_simulated_vehicle.hpp>
#include <headunit/vehicle.hpp>

#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <wlr-layer-shell-client-protocol.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}

constexpr std::size_t buffer_count = 3;

struct FrameBuffer {
  wl_buffer* handle = nullptr;
  bool busy = false;
};

struct Client {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  wl_shm* shm = nullptr;
  wp_viewporter* viewporter = nullptr;
  wp_viewport* viewport = nullptr;
  zwlr_layer_shell_v1* layer_shell = nullptr;
  zwlr_layer_surface_v1* layer_surface = nullptr;
  xdg_wm_base* wm = nullptr;
  wl_surface* surface = nullptr;
  xdg_surface* shell_surface = nullptr;
  xdg_toplevel* toplevel = nullptr;
  wl_output* fullscreen_output = nullptr;
  struct Output {
    wl_output* handle = nullptr;
    std::string name;
  };
  std::vector<std::unique_ptr<Output>> outputs;
  std::string requested_output;
  std::string media_file;
  std::string page_file;
  bool background = false;
  std::array<FrameBuffer, buffer_count> buffers{};
  void* mapped = MAP_FAILED;
  unsigned width = 960, height = 360;
  unsigned buffer_width = 960, buffer_height = 360;
  std::size_t frame_bytes = 960 * 360 * 4;
  std::size_t mapped_bytes = frame_bytes * buffer_count;
  int compositor_version = 1;
  bool configured = false;
  bool retiring_surface = false;
  bool configure_size_mismatch = false;
  bool closing = false;
  // Paced by compositor frame callbacks: a covered background layer (under
  // Ultra or Standard) receives none, so it stops drawing until it is shown.
  wl_callback* frame = nullptr;
  headunit::ClusterReturn* recovery = nullptr;
  std::uint64_t recovery_generation = 0, frame_token = 0;
  headunit::Clock::time_point presented{};

  ~Client() {
    if (frame) wl_callback_destroy(frame);
    for (auto& buffer : buffers) {
      if (buffer.handle) wl_buffer_destroy(buffer.handle);
    }
    if (mapped != MAP_FAILED) munmap(mapped, mapped_bytes);
    if (toplevel) xdg_toplevel_destroy(toplevel);
    if (layer_surface) zwlr_layer_surface_v1_destroy(layer_surface);
    if (viewport) wp_viewport_destroy(viewport);
    for (auto& output : outputs)
      if (output->handle) wl_output_destroy(output->handle);
    if (shell_surface) xdg_surface_destroy(shell_surface);
    if (surface) wl_surface_destroy(surface);
    if (wm) xdg_wm_base_destroy(wm);
    if (shm) wl_shm_destroy(shm);
    if (viewporter) wp_viewporter_destroy(viewporter);
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (compositor) wl_compositor_destroy(compositor);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
    if (recovery)
      std::clog << "Native cluster presenter retired; generation=" << recovery_generation
                << " retiredMonotonicNs="
                << std::chrono::duration_cast<std::chrono::nanoseconds>(
                       headunit::Clock::now().time_since_epoch())
                       .count()
                << '\n';
  }
};

void global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
            std::uint32_t version) {
  auto& client = *static_cast<Client*>(data);
  const std::string_view id(interface);
  if (id == wl_compositor_interface.name && !client.compositor) {
    client.compositor_version = static_cast<int>(std::min(version, 4u));
    client.compositor = static_cast<wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, client.compositor_version));
  } else if (id == wl_shm_interface.name && !client.shm) {
    client.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
  } else if (id == wp_viewporter_interface.name && !client.viewporter) {
    client.viewporter =
        static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
  } else if (id == zwlr_layer_shell_v1_interface.name && !client.layer_shell && version >= 3) {
    client.layer_shell = static_cast<zwlr_layer_shell_v1*>(
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 3));
  } else if (id == xdg_wm_base_interface.name && !client.wm) {
    client.wm =
        static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
  } else if (id == wl_output_interface.name && !client.requested_output.empty() && version >= 4) {
    auto output = std::make_unique<Client::Output>();
    output->handle =
        static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 4));
    if (!output->handle) return;
    static const wl_output_listener listener = [] {
      wl_output_listener value{};
      value.geometry = [](void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                          std::int32_t, const char*, const char*, std::int32_t) {
      };
      value.mode = [](void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {
      };
      value.done = [](void*, wl_output*) {
      };
      value.scale = [](void*, wl_output*, std::int32_t) {
      };
      value.name = [](void* data, wl_output*, const char* text) {
        static_cast<Client::Output*>(data)->name = text ? text : "";
      };
      value.description = [](void*, wl_output*, const char*) {
      };
      return value;
    }();
    wl_output_add_listener(output->handle, &listener, output.get());
    client.outputs.push_back(std::move(output));
  }
}

void global_removed(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener registry_listener{global, global_removed};

void ping(void*, xdg_wm_base* wm, std::uint32_t serial) {
  xdg_wm_base_pong(wm, serial);
}
const xdg_wm_base_listener wm_listener{ping};

void configure(void* data, xdg_surface* surface, std::uint32_t serial) {
  auto& client = *static_cast<Client*>(data);
  if (client.retiring_surface) return;
  xdg_surface_ack_configure(surface, serial);
  client.configured = true;
}
const xdg_surface_listener surface_listener{configure};

void layer_configure(void* data, zwlr_layer_surface_v1* surface, std::uint32_t serial,
                     std::uint32_t width, std::uint32_t height) {
  auto& client = *static_cast<Client*>(data);
  if (client.retiring_surface) return;
  if (width != client.width || height != client.height) client.configure_size_mismatch = true;
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  client.configured = true;
}
void layer_closed(void* data, zwlr_layer_surface_v1*) {
  static_cast<Client*>(data)->closing = true;
}
const zwlr_layer_surface_v1_listener layer_listener{layer_configure, layer_closed};

void toplevel_configure(void*, xdg_toplevel*, std::int32_t, std::int32_t, wl_array*) {}
void toplevel_close(void* data, xdg_toplevel*) {
  static_cast<Client*>(data)->closing = true;
}
const xdg_toplevel_listener toplevel_listener{toplevel_configure, toplevel_close, nullptr, nullptr};

void buffer_released(void* data, wl_buffer*) {
  static_cast<FrameBuffer*>(data)->busy = false;
}
const wl_buffer_listener buffer_listener{buffer_released};

void create_buffers(Client& client) {
  static_assert(std::endian::native == std::endian::little);
  char path[] = "/dev/shm/headunit-cluster-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) throw std::runtime_error("cannot create shared display buffer");
  unlink(path);
  if (ftruncate(fd, static_cast<off_t>(client.mapped_bytes)) != 0) {
    close(fd);
    throw std::runtime_error("cannot size shared display buffer");
  }
  client.mapped = mmap(nullptr, client.mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (client.mapped == MAP_FAILED) {
    close(fd);
    throw std::runtime_error("cannot map shared display buffer");
  }
  wl_shm_pool* pool =
      wl_shm_create_pool(client.shm, fd, static_cast<std::int32_t>(client.mapped_bytes));
  close(fd);
  if (!pool) throw std::runtime_error("cannot create Wayland buffer pool");
  for (std::size_t index = 0; index < buffer_count; ++index) {
    auto& buffer = client.buffers[index];
    buffer.handle = wl_shm_pool_create_buffer(
        pool, static_cast<std::int32_t>(index * client.frame_bytes),
        static_cast<std::int32_t>(client.buffer_width),
        static_cast<std::int32_t>(client.buffer_height),
        static_cast<std::int32_t>(client.buffer_width * 4), WL_SHM_FORMAT_XRGB8888);
    if (!buffer.handle) {
      wl_shm_pool_destroy(pool);
      throw std::runtime_error("cannot create Wayland buffer");
    }
    wl_buffer_add_listener(buffer.handle, &buffer_listener, &buffer);
  }
  wl_shm_pool_destroy(pool);
}

void frame_done(void* data, wl_callback* callback, std::uint32_t) {
  auto& client = *static_cast<Client*>(data);
  if (client.frame != callback) return;
  client.frame = nullptr;
  if (client.recovery)
    client.recovery->frame_done(client.recovery_generation, client.frame_token,
                                headunit::Clock::now());
  wl_callback_destroy(callback);
}
const wl_callback_listener frame_listener{frame_done};

// This private connection is read only on the native render owner. Prepared
// reads are cancelled on timeout/signal, so startup never waits for a stalled
// compositor to notice process shutdown. No startup deadline is changed.
void dispatch_native(Client& client, int timeout_ms) {
  if (stopping) return;
  while (wl_display_prepare_read(client.display) != 0) {
    if (wl_display_dispatch_pending(client.display) < 0)
      throw std::runtime_error("native Wayland dispatch failed");
    if (stopping) return;
  }
  if (stopping) {
    wl_display_cancel_read(client.display);
    return;
  }
  const int flushed = wl_display_flush(client.display);
  if (flushed < 0 && errno != EAGAIN) {
    wl_display_cancel_read(client.display);
    if (stopping) return;
    throw std::runtime_error("native Wayland flush failed");
  }
  pollfd ready{wl_display_get_fd(client.display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
  const int result = poll(&ready, 1, timeout_ms);
  if (result > 0 && (ready.revents & POLLIN)) {
    if (wl_display_read_events(client.display) < 0 && !stopping)
      throw std::runtime_error("native Wayland read failed");
  } else {
    wl_display_cancel_read(client.display);
  }
  if (stopping) return;
  if ((result < 0 && errno != EINTR) || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)))
    throw std::runtime_error("native Wayland connection lost");
  if (wl_display_dispatch_pending(client.display) < 0)
    throw std::runtime_error("native Wayland dispatch failed");
}
bool native_roundtrip(Client& client) {
  struct Sync {
    wl_callback* callback = nullptr;
    bool done = false;
    ~Sync() {
      if (callback) wl_callback_destroy(callback);
    }
  } sync;
  sync.callback = wl_display_sync(client.display);
  if (!sync.callback) throw std::runtime_error("cannot synchronize native Wayland connection");
  static const wl_callback_listener listener{[](void* data, wl_callback* callback, std::uint32_t) {
    auto& sync = *static_cast<Sync*>(data);
    sync.callback = nullptr;
    sync.done = true;
    wl_callback_destroy(callback);
  }};
  if (wl_callback_add_listener(sync.callback, &listener, &sync) < 0)
    throw std::runtime_error("cannot install native Wayland synchronization");
  while (!sync.done && !stopping && !client.closing)
    dispatch_native(client, 25);
  return sync.done && !stopping && !client.closing;
}

// Apply the original native geometry/input policy to an initial or renewed
// role. Aborted return keeps the Client/connection and renews its surface/role.
void configure_native_role(Client& client) {
  if (client.layer_surface) {
    zwlr_layer_surface_v1_set_size(client.layer_surface, client.width, client.height);
    zwlr_layer_surface_v1_set_anchor(client.layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                               ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                               ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                               ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(client.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(client.layer_surface, 0);
    wl_region* empty = wl_compositor_create_region(client.compositor);
    if (!empty) throw std::runtime_error("cannot create empty cluster input region");
    wl_surface_set_input_region(client.surface, empty);
    wl_region_destroy(empty);
  } else if (client.toplevel) {
    xdg_toplevel_set_title(client.toplevel, "Backup cluster");
    xdg_toplevel_set_app_id(client.toplevel, "headunit.backup-cluster");
    xdg_toplevel_set_fullscreen(client.toplevel, client.fullscreen_output);
  }
}
void prepare_native_remap(Client& client) {
  // A timed-out unmap can leave old configure events outstanding. Destroy their
  // proxies in order before creating replacement native roles on this SAME
  // connection. Keep the native owner, buffers, busy leases and source state.
  client.configured = false;
  if (client.toplevel) xdg_toplevel_destroy(client.toplevel);
  if (client.shell_surface) xdg_surface_destroy(client.shell_surface);
  if (client.layer_surface) zwlr_layer_surface_v1_destroy(client.layer_surface);
  if (client.viewport) wp_viewport_destroy(client.viewport);
  wl_surface_destroy(client.surface);
  client.toplevel = nullptr;
  client.shell_surface = nullptr;
  client.layer_surface = nullptr;
  client.viewport = nullptr;
  client.retiring_surface = false;
  client.surface = wl_compositor_create_surface(client.compositor);
  if (!client.surface) throw std::runtime_error("cannot restore native cluster surface");
  client.viewport = wp_viewporter_get_viewport(client.viewporter, client.surface);
  if (!client.viewport) throw std::runtime_error("cannot restore native cluster viewport");
  wp_viewport_set_destination(client.viewport, client.width, client.height);
  if (client.background) {
    client.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        client.layer_shell, client.surface, client.fullscreen_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "headunit.backup-cluster");
    if (!client.layer_surface) throw std::runtime_error("cannot restore native background role");
    zwlr_layer_surface_v1_add_listener(client.layer_surface, &layer_listener, &client);
  } else {
    client.shell_surface = xdg_wm_base_get_xdg_surface(client.wm, client.surface);
    if (!client.shell_surface) throw std::runtime_error("cannot restore native shell role");
    xdg_surface_add_listener(client.shell_surface, &surface_listener, &client);
    client.toplevel = xdg_surface_get_toplevel(client.shell_surface);
    if (!client.toplevel) throw std::runtime_error("cannot restore native toplevel");
    xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
  }
  configure_native_role(client);
  wl_surface_commit(client.surface);  // Null: require the replacement role's configure first.
}

bool present(Client& client, const headunit::ClusterContent& content,
             headunit::Clock::time_point now) {
  if (!client.configured || client.closing || stopping) return false;
  auto available = std::find_if(client.buffers.begin(), client.buffers.end(),
                                [](const FrameBuffer& buffer) { return !buffer.busy; });
  if (available == client.buffers.end()) return false;
  const std::size_t index = static_cast<std::size_t>(available - client.buffers.begin());
  auto* pixels = static_cast<std::uint32_t*>(client.mapped) + index * (client.frame_bytes / 4);
  // Phone-free pages are opaque, so the XRGB buffer needs no alpha.
  headunit::draw_native_cluster(content, now, pixels, client.buffer_width, client.buffer_height,
                                std::size_t(client.buffer_width) * 4);
  if (client.frame) wl_callback_destroy(client.frame);
  client.frame = wl_surface_frame(client.surface);
  if (!client.frame) throw std::runtime_error("cannot request cluster frame callback");
  wl_callback_add_listener(client.frame, &frame_listener, &client);
  ++client.frame_token;
  client.presented = now;
  available->busy = true;
  wl_surface_attach(client.surface, available->handle, 0, 0);
  if (client.compositor_version >= 4) {
    wl_surface_damage_buffer(client.surface, 0, 0, client.buffer_width, client.buffer_height);
  } else {
    wl_surface_damage(client.surface, 0, 0, client.width, client.height);
  }
  wl_surface_commit(client.surface);
  if (client.recovery)
    client.recovery->committed(client.recovery_generation, client.frame_token,
                               headunit::Clock::now());
  return true;
}

// A return must unmap the native content on the server before the signed
// successor creates its surface. A sync completion acknowledges request order,
// not physical presentation. Retire this callback before its stack state exits.
enum class NativeUnmap { acknowledged, aborted, stopped };
NativeUnmap retire_native_for_return(Client& client) {
  client.retiring_surface = true;
  client.configured = false;
  if (client.frame) {
    wl_callback_destroy(client.frame);
    client.frame = nullptr;
  }
  struct Unmap {
    wl_callback* callback = nullptr;
    bool acknowledged = false;
    headunit::Clock::time_point observed_at{};
    ~Unmap() {
      if (callback) wl_callback_destroy(callback);
    }
  } unmap;
  wl_surface_attach(client.surface, nullptr, 0, 0);
  wl_surface_commit(client.surface);
  unmap.callback = wl_display_sync(client.display);
  if (!unmap.callback) throw std::runtime_error("cannot acknowledge native cluster unmap");
  static const wl_callback_listener listener{[](void* data, wl_callback* callback, std::uint32_t) {
    auto& state = *static_cast<Unmap*>(data);
    if (state.callback != callback) return;
    state.observed_at = headunit::Clock::now();
    state.acknowledged = true;
    state.callback = nullptr;
    wl_callback_destroy(callback);
  }};
  if (wl_callback_add_listener(unmap.callback, &listener, &unmap) < 0)
    throw std::runtime_error("cannot install native cluster unmap acknowledgement");
  const auto deadline = headunit::Clock::now() + std::chrono::seconds(2);
  while (!unmap.acknowledged && !stopping) {
    while (wl_display_prepare_read(client.display) != 0) {
      if (wl_display_dispatch_pending(client.display) < 0)
        throw std::runtime_error("native cluster retirement dispatch failed");
    }
    if (unmap.acknowledged || stopping) {
      wl_display_cancel_read(client.display);
      break;
    }
    const int flushed = wl_display_flush(client.display);
    if (flushed < 0 && errno != EAGAIN) {
      wl_display_cancel_read(client.display);
      throw std::runtime_error("native cluster retirement flush failed");
    }
    const auto remaining =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - headunit::Clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      wl_display_cancel_read(client.display);
      pollfd connection{wl_display_get_fd(client.display), POLLIN, 0};
      const int result = poll(&connection, 1, 0);
      if ((result < 0 && errno != EINTR) || (connection.revents & (POLLERR | POLLHUP | POLLNVAL)) ||
          wl_display_get_error(client.display) || client.closing)
        throw std::runtime_error("native cluster retirement connection lost");
      return NativeUnmap::aborted;  // No acknowledgement is not evidence of output death.
    }
    pollfd ready{wl_display_get_fd(client.display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
    const int result = poll(&ready, 1, static_cast<int>(std::min(remaining.count(), 25L)));
    if (result > 0 && (ready.revents & POLLIN)) {
      if (wl_display_read_events(client.display) < 0)
        throw std::runtime_error("native cluster retirement read failed");
    } else {
      wl_display_cancel_read(client.display);
    }
    if ((result < 0 && errno != EINTR) || (ready.revents & (POLLERR | POLLHUP | POLLNVAL)))
      throw std::runtime_error("native cluster retirement connection lost");
    if (wl_display_dispatch_pending(client.display) < 0 || client.closing)
      throw std::runtime_error("native cluster output retired during unmap");
  }
  if (stopping) return NativeUnmap::stopped;
  if (unmap.acknowledged)
    std::clog << "Native cluster unmap acknowledged; observedMonotonicNs="
              << std::chrono::duration_cast<std::chrono::nanoseconds>(
                     unmap.observed_at.time_since_epoch())
                     .count()
              << '\n';
  return NativeUnmap::acknowledged;
}

bool finish_native_return(Client& client) {
  const auto result = retire_native_for_return(client);
  if (result != NativeUnmap::aborted) return true;
  if (!client.recovery) throw std::logic_error("native retry has no lifetime owner");
  client.recovery->abort_return(client.recovery_generation);
  // The old sync proxy is already destroyed. Renew native surface/role proxies
  // in order on the same connection; do not reuse old configure or stale pixels.
  prepare_native_remap(client);
  std::clog << "Local Ultra retry aborted; same native owner awaiting remap configure; "
               "retry permanently spent; observedMonotonicNs="
            << std::chrono::duration_cast<std::chrono::nanoseconds>(
                   headunit::Clock::now().time_since_epoch())
                   .count()
            << '\n';
  return false;
}

}  // namespace

int main(int argc, char** argv) try {
  std::unique_ptr<headunit::VehicleSource> vehicle;
  unsigned width = 960;
  unsigned height = 360;
  std::string requested_output;
  std::string media_file;
  std::string page_file;
  bool background = false;
  headunit::NativeClusterConfiguration cluster_configuration;
  bool configured_cluster = false;
  std::string local_ultra_profile;
  bool local_output_retry = false;
  std::string greeting_file;
  std::string prephone_socket;
  bool ambient_enabled = false;

  std::string vehicle_socket;

  bool indicators_enabled = false;
  for (int i = 1; i < argc;) {
    const std::string_view option(argv[i++]);
    if (option == "--simulate" && !vehicle && vehicle_socket.empty()) {
      vehicle = std::make_unique<headunit::SimulatedVehicle>(headunit::Clock::now());
    } else if (option == "--simulate-dated" && !vehicle && vehicle_socket.empty()) {
      vehicle = std::make_unique<headunit::DatedSimulatedVehicle>(headunit::Clock::now(), 1);
    } else if (option == "--background") {
      background = true;
    } else if (option == "--vehicle-socket" && !vehicle && vehicle_socket.empty() && i < argc) {
      vehicle_socket = argv[i++];
      if (vehicle_socket.empty()) throw std::invalid_argument("invalid vehicle socket");
    } else if (option == "--size" && i + 1 < argc) {
      const auto parse = [](const char* value) {
        unsigned result = 0;
        const auto* end = value + std::strlen(value);
        const auto [at, error] = std::from_chars(value, end, result);
        if (error != std::errc{} || at != end) throw std::invalid_argument("invalid cluster size");
        return result;
      };
      width = parse(argv[i++]);
      height = parse(argv[i++]);
      if (width < 640 || width > 4096 || height < 240 || height > 2160)
        throw std::invalid_argument("cluster size outside supported range");
    } else if (option == "--output" && i < argc) {
      requested_output = argv[i++];
      if (requested_output.empty() || requested_output.size() > 100 ||
          requested_output.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid cluster output name");
    } else if (option == "--media-file" && i < argc) {
      media_file = argv[i++];
      if (media_file.empty()) throw std::invalid_argument("invalid media file");
    } else if (option == "--native-cluster-config" && i < argc && !configured_cluster) {
      cluster_configuration = headunit::load_native_cluster_configuration(argv[i++]);
      configured_cluster = true;
    } else if (option == "--local-ultra-output-retry" && !local_output_retry) {
      local_output_retry = true;
    } else if (option == "--local-ultra-profile" && i < argc && local_ultra_profile.empty()) {
      local_ultra_profile = argv[i++];
    } else if (option == "--greeting-sequence-file" && i < argc && greeting_file.empty()) {
      greeting_file = argv[i++];
      if (!std::filesystem::path(greeting_file).is_absolute())
        throw std::invalid_argument("greeting preference must be absolute");
    } else if (option == "--prephone-indicators" && !indicators_enabled) {
      indicators_enabled = true;
    } else if (option == "--ambient-light-software" && !ambient_enabled) {
      ambient_enabled = true;
    } else if (option == "--prephone-publication-socket" && i < argc && prephone_socket.empty()) {
      prephone_socket = argv[i++];
      const auto path = std::filesystem::path(prephone_socket);
      if (!path.is_absolute() || path.lexically_normal() != path || prephone_socket.size() > 103)
        throw std::invalid_argument("invalid private pre-phone publication socket");
    } else if (option == "--page-file" && i < argc) {
      page_file = argv[i++];
      if (page_file.empty()) throw std::invalid_argument("invalid cluster page file");
    } else {
      throw std::invalid_argument(
          "usage: headunit-cluster-wayland [--size WIDTH HEIGHT] [--output NAME] [--background] "
          "[--media-file PATH] [--page-file PATH] [--native-cluster-config PATH] "
          "[--simulate | --simulate-dated | --vehicle-socket PATH]");
    }
  }
  if (local_output_retry && (local_ultra_profile.empty() || !background))
    throw std::invalid_argument("local output retry requires signed profile and background role");
  if (indicators_enabled && prephone_socket.empty())
    throw std::invalid_argument("pre-phone indicators require native publication");
  if (ambient_enabled && prephone_socket.empty())
    throw std::invalid_argument("ambient software model requires native publication");

  if (!vehicle_socket.empty()) vehicle = std::make_unique<headunit::SocketVehicle>(vehicle_socket);
  if (!vehicle) {
    class UnavailableVehicle final : public headunit::VehicleSource {
     public:
      headunit::VehicleSample sample(headunit::Clock::time_point) override { return {}; }
    };
    vehicle = std::make_unique<UnavailableVehicle>();
  }
  if (!prephone_socket.empty())
    vehicle = std::make_unique<headunit::PublishingVehicle>(std::move(vehicle), prephone_socket,
                                                            ambient_enabled, indicators_enabled);
  std::unique_ptr<headunit::GreetingSequenceFile> greeting;
  if (!greeting_file.empty())
    greeting = std::make_unique<headunit::GreetingSequenceFile>(greeting_file,
                                                                headunit::claim_greeting_boot());
  const auto previous_term = std::signal(SIGTERM, stop);
  const auto previous_int = std::signal(SIGINT, stop);
  const auto previous_hup = std::signal(SIGHUP, stop);
  struct Signals {
    decltype(previous_term) term, interrupt, hup;
    ~Signals() {
      std::signal(SIGTERM, term);
      std::signal(SIGINT, interrupt);
      std::signal(SIGHUP, hup);
    }
  } signals{previous_term, previous_int, previous_hup};
  const auto native = [&](headunit::ClusterReturn* recovery, std::uint64_t generation) {
    Client client;
    client.recovery = recovery;
    client.recovery_generation = generation;
    client.requested_output = requested_output;
    client.media_file = media_file;
    client.page_file = page_file;
    client.background = background;
    client.width = width;
    client.height = height;
    // Draw at output resolution: small labels are illegible when upscaled.
    // Frame-callback pacing, not resolution, keeps a hidden cluster cheap.
    client.buffer_width = width;
    client.buffer_height = height;
    client.frame_bytes = std::size_t(client.buffer_width) * client.buffer_height * 4;
    client.mapped_bytes = client.frame_bytes * buffer_count;
    client.display = wl_display_connect(nullptr);
    if (!client.display) throw std::runtime_error("cannot connect to Wayland display");
    client.registry = wl_display_get_registry(client.display);
    if (!client.registry) throw std::runtime_error("cannot read Wayland registry");
    wl_registry_add_listener(client.registry, &registry_listener, &client);
    if (!native_roundtrip(client)) return;
    wl_registry_destroy(client.registry);
    client.registry = nullptr;
    // Registry enumeration precedes each bound output's initial name event.
    if (!requested_output.empty() && !native_roundtrip(client)) return;
    if (!client.compositor || !client.shm || !client.viewporter ||
        (background ? !client.layer_shell : !client.wm)) {
      throw std::runtime_error("Wayland compositor lacks required cluster protocols");
    }
    if (client.wm) xdg_wm_base_add_listener(client.wm, &wm_listener, &client);
    client.surface = wl_compositor_create_surface(client.compositor);
    if (!client.surface) throw std::runtime_error("cannot create cluster surface");
    client.viewport = wp_viewporter_get_viewport(client.viewporter, client.surface);
    if (!client.viewport) throw std::runtime_error("cannot create cluster viewport");
    wp_viewport_set_destination(client.viewport, width, height);
    wl_output*& fullscreen_output = client.fullscreen_output;
    if (!requested_output.empty()) {
      for (const auto& output : client.outputs) {
        if (output->name == requested_output) {
          if (fullscreen_output) throw std::runtime_error("duplicate cluster output name");
          fullscreen_output = output->handle;
        }
      }
      if (!fullscreen_output) throw std::runtime_error("configured cluster output is unavailable");
    }
    if (background) {
      client.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
          client.layer_shell, client.surface, fullscreen_output,
          ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "headunit.backup-cluster");
      if (!client.layer_surface) throw std::runtime_error("cannot create cluster background layer");
      zwlr_layer_surface_v1_add_listener(client.layer_surface, &layer_listener, &client);

    } else {
      client.shell_surface = xdg_wm_base_get_xdg_surface(client.wm, client.surface);
      if (!client.shell_surface) throw std::runtime_error("cannot create cluster shell surface");
      xdg_surface_add_listener(client.shell_surface, &surface_listener, &client);
      client.toplevel = xdg_surface_get_toplevel(client.shell_surface);
      if (!client.toplevel) throw std::runtime_error("cannot create cluster window");
      xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
    }
    configure_native_role(client);
    wl_surface_commit(client.surface);
    while (!client.configured && !client.closing && !stopping)
      dispatch_native(client, 25);
    if (client.configure_size_mismatch)
      throw std::runtime_error("backup output size does not match configured cluster size");
    if (client.closing || stopping) return;
    create_buffers(client);
    // Match the native Standard cluster's 20 Hz refresh while visible. While
    // the compositor withholds frame callbacks, redraw only once a second so
    // the content is current when the layer is uncovered.
    constexpr auto interval = std::chrono::milliseconds(50);
    constexpr auto hidden_interval = std::chrono::seconds(1);
    headunit::TurnSignalAnimation signals;
    auto next_frame = headunit::Clock::now();
    while (!client.closing && !stopping) {
      if (client.configure_size_mismatch)
        throw std::runtime_error("backup output size does not match configured cluster size");
      if (recovery && recovery->request_return(headunit::Clock::now())) {
        const auto observation = recovery->callback_observation();
        const auto ns = [](auto at) {
          return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch())
              .count();
        };
        std::clog << "Local Ultra output-work retry eligible; generation=" << generation
                  << " token=" << client.frame_token
                  << " commitObservedMonotonicNs=" << ns(observation.submitted_at)
                  << " callbackReceivedMonotonicNs=" << ns(observation.received_at)
                  << " eligibilityMonotonicNs=" << ns(headunit::Clock::now())
                  << "; retiring native presenter\n";
        if (finish_native_return(client)) return;
        next_frame = headunit::Clock::now();
      }
      const auto due = [&] {
        return client.frame ? std::max(next_frame, client.presented + hidden_interval) : next_frame;
      };
      const auto now = headunit::Clock::now();
      if (now >= due()) {
        const auto page = headunit::selected_cluster_center(client.page_file, false);
        // With every buffer still held by the compositor, skip this tick.
        const auto sample = vehicle->sample(now);
        const auto lamps = signals.update(sample, now);
        present(client,
                {sample,
                 client.media_file,
                 {},
                 page,
                 false,
                 cluster_configuration,
                 greeting ? greeting->frame(now) : headunit::GreetingFrame{},
                 &lamps},
                now);
        next_frame = now + (lamps.animating ? std::chrono::milliseconds(20) : interval);
      }
      if (wl_display_flush(client.display) < 0) throw std::runtime_error("Wayland flush failed");
      pollfd ready{wl_display_get_fd(client.display), POLLIN, 0};
      // Round up: a truncated sub-millisecond wait would spin until the deadline.
      const auto wait =
          std::chrono::ceil<std::chrono::milliseconds>(due() - headunit::Clock::now());
      const int timeout = std::max(0, static_cast<int>(wait.count()));
      const int result = poll(&ready, 1, timeout);
      if (result < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("Wayland poll failed");
      }
      if (result > 0 && wl_display_dispatch(client.display) < 0) {
        throw std::runtime_error("Wayland connection closed");
      }
      if (wl_display_dispatch_pending(client.display) < 0) {
        throw std::runtime_error("Wayland event failed");
      }
    }
  };
  if (!local_ultra_profile.empty()) {
#ifdef HEADUNIT_HAVE_LOCAL_ULTRA_CLUSTER
    headunit::run_cluster_presenters(
        [&] {
          headunit::theme::script::run_local_cluster(local_ultra_profile, *vehicle, width, height,
                                                     requested_output, greeting.get(),
                                                     [] { return stopping != 0; });
        },
        [&](headunit::ClusterReturn& recovery, std::uint64_t generation) {
          native(&recovery, generation);
        },
        [] { return headunit::Clock::now(); }, [] { return stopping != 0; },
        [](std::string_view error, bool feedback_timeout) {
          std::clog << "Local Ultra exit reason="
                    << (feedback_timeout ? "unresolved-presentation-timeout" : "local-fault")
                    << " observedMonotonicNs="
                    << std::chrono::duration_cast<std::chrono::nanoseconds>(
                           headunit::Clock::now().time_since_epoch())
                           .count()
                    << '\n';
          std::cerr << "Local Ultra gauges unavailable: " << error
                    << "; restoring native instruments\n";
        },
        local_output_retry);
    return 0;
#else
    std::cerr << "Local Ultra gauges are not built; using native instruments\n";
#endif
  }
  native(nullptr, 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << "backup cluster: " << error.what() << '\n';
  return 1;
}
