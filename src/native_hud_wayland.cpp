// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/hud_configuration.hpp>
#include <headunit/native_hud.hpp>
#include <headunit/navigation_socket.hpp>
#include <headunit/socket_vehicle.hpp>

#include <wayland-client.h>
#include <wlr-layer-shell-client-protocol.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) {
  stopped = 1;
}
void require(bool valid, const char* reason) {
  if (!valid) throw std::runtime_error(reason);
}

struct HudOwner {
  int descriptor = -1;
  explicit HudOwner(const headunit::HudConfiguration& config) {
    const auto* directory = std::getenv("XDG_RUNTIME_DIR");
    require(directory && *directory, "HUD requires a private runtime directory");
    const std::filesystem::path runtime(directory);
    struct stat parent{};
    require(runtime.is_absolute() && ::lstat(runtime.c_str(), &parent) == 0 &&
                S_ISDIR(parent.st_mode) && parent.st_uid == ::getuid() &&
                (parent.st_mode & 07777) == 0700 && std::filesystem::canonical(runtime) == runtime,
            "HUD requires an owned canonical 0700 runtime directory");
    // The length prefix keeps pairs unambiguous even when names contain '-'.
    const auto path = runtime / ("headunit-hud-" + std::to_string(config.wayland_display.size()) +
                                 "-" + config.wayland_display + "-" + config.output_name + ".lock");
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat opened{}, named{};
    if (fd < 0 || ::fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        opened.st_uid != ::getuid() || opened.st_nlink != 1 || (opened.st_mode & 07777) != 0600 ||
        ::flock(fd, LOCK_EX | LOCK_NB) != 0 || ::lstat(path.c_str(), &named) != 0 ||
        named.st_dev != opened.st_dev || named.st_ino != opened.st_ino) {
      if (fd >= 0) ::close(fd);
      throw std::runtime_error("HUD output already owned or unsafe owner lock");
    }
    descriptor = fd;
  }
  ~HudOwner() {
    if (descriptor >= 0) ::close(descriptor);
  }
  HudOwner(const HudOwner&) = delete;
  HudOwner& operator=(const HudOwner&) = delete;
};

// A private, noninteractive client. It never gives the compositor an unspecified
// output, so an absent or withdrawn HUD cannot become a main/cluster window.
struct HudWindow {
  struct Output {
    HudWindow* owner = nullptr;
    std::uint32_t id = 0;
    wl_output* handle = nullptr;
    std::string name;
    ~Output() {
      if (handle) wl_output_release(handle);
    }
  };
  struct Buffer {
    wl_buffer* handle = nullptr;
    bool busy = false;
  };
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  wl_compositor* compositor = nullptr;
  wl_shm* shm = nullptr;
  zwlr_layer_shell_v1* shell = nullptr;
  std::uint32_t compositor_id = 0, shm_id = 0, shell_id = 0;
  wl_surface* surface = nullptr;
  zwlr_layer_surface_v1* layer = nullptr;
  wl_callback* sync = nullptr;
  wl_callback* frame = nullptr;
  std::vector<std::unique_ptr<Output>> outputs;
  Output* selected = nullptr;
  std::array<Buffer, 3> buffers{};
  void* mapping = MAP_FAILED;
  std::size_t frame_bytes = 0, mapped_bytes = 0;
  unsigned width = 0, height = 0;
  bool configured = false, closing = false, invalid = false;
  headunit::Clock::time_point submitted{};

  ~HudWindow() {
    if (sync) wl_callback_destroy(sync);
    if (frame) wl_callback_destroy(frame);
    for (auto& buffer : buffers)
      if (buffer.handle) wl_buffer_destroy(buffer.handle);
    if (mapping != MAP_FAILED) ::munmap(mapping, mapped_bytes);
    if (layer) zwlr_layer_surface_v1_destroy(layer);
    if (surface) wl_surface_destroy(surface);
    outputs.clear();
    if (shell) zwlr_layer_shell_v1_destroy(shell);
    if (shm) wl_shm_destroy(shm);
    if (compositor) wl_compositor_destroy(compositor);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
  }

  void dispatch(int timeout) {
    while (wl_display_prepare_read(display) != 0)
      require(wl_display_dispatch_pending(display) >= 0, "HUD event dispatch failed");
    const int flushed = wl_display_flush(display);
    if (flushed < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      throw std::runtime_error("HUD display flush failed");
    }
    pollfd fd{wl_display_get_fd(display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
    const int result = ::poll(&fd, 1, timeout);
    if (result > 0 && (fd.revents & POLLIN))
      require(wl_display_read_events(display) >= 0, "HUD display read failed");
    else
      wl_display_cancel_read(display);
    require(result >= 0 || errno == EINTR, "HUD display poll failed");
    require(!(fd.revents & (POLLHUP | POLLERR | POLLNVAL)), "HUD display connection lost");
    require(wl_display_dispatch_pending(display) >= 0, "HUD event dispatch failed");
    require(!invalid, "HUD output or configured geometry changed");
  }

  void synchronize() {
    sync = wl_display_sync(display);
    require(sync, "cannot synchronize HUD display");
    static const wl_callback_listener listener{
        [](void* data, wl_callback* callback, std::uint32_t) {
          static_cast<HudWindow*>(data)->sync = nullptr;
          wl_callback_destroy(callback);
        }};
    wl_callback_add_listener(sync, &listener, this);
    const auto deadline = headunit::Clock::now() + std::chrono::seconds(2);
    while (sync && !stopped && !closing) {
      require(headunit::Clock::now() < deadline, "HUD display discovery timed out");
      dispatch(25);
    }
  }

  void connect(const headunit::HudConfiguration& config) {
    const auto& output_name = config.output_name;
    const auto pixel_width = config.width, pixel_height = config.height;
    width = pixel_width;
    height = pixel_height;
    display = wl_display_connect(config.wayland_display.c_str());
    require(display, "cannot connect to HUD Wayland display");
    registry = wl_display_get_registry(display);
    require(registry, "cannot obtain HUD Wayland registry");
    static const wl_registry_listener listener{
        [](void* data, wl_registry* registry, std::uint32_t id, const char* interface,
           std::uint32_t version) {
          auto& client = *static_cast<HudWindow*>(data);
          if (!std::strcmp(interface, "wl_compositor") && !client.compositor && version >= 4) {
            client.compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, id, &wl_compositor_interface, 4));
            client.compositor_id = id;
          } else if (!std::strcmp(interface, "wl_shm") && !client.shm) {
            client.shm = static_cast<wl_shm*>(wl_registry_bind(registry, id, &wl_shm_interface, 1));
            client.shm_id = id;
          } else if (!std::strcmp(interface, "zwlr_layer_shell_v1") && !client.shell &&
                     version >= 3) {
            client.shell = static_cast<zwlr_layer_shell_v1*>(
                wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface, 3));
            client.shell_id = id;
          } else if (!std::strcmp(interface, "wl_output") && version >= 4) {
            try {
              if (client.outputs.size() >= 64) {
                client.invalid = true;
                return;
              }
              auto output = std::make_unique<Output>();
              output->owner = &client;
              output->id = id;
              output->handle =
                  static_cast<wl_output*>(wl_registry_bind(registry, id, &wl_output_interface, 4));
              if (!output->handle) {
                client.invalid = true;
                return;
              }
              static const wl_output_listener output_listener = [] {
                wl_output_listener value{};
                value.geometry = [](void*, wl_output*, std::int32_t, std::int32_t, std::int32_t,
                                    std::int32_t, std::int32_t, const char*, const char*,
                                    std::int32_t) {
                };
                value.mode = [](void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t,
                                std::int32_t) {
                };
                value.done = [](void*, wl_output*) {
                };
                value.scale = [](void*, wl_output*, std::int32_t) {
                };
                value.name = [](void* data, wl_output*, const char* name) {
                  auto& output = *static_cast<Output*>(data);
                  try {
                    if (!name || std::strlen(name) > 100 ||
                        (output.owner->selected == &output && output.name != name) ||
                        (output.owner->selected && output.owner->selected != &output &&
                         output.owner->selected->name == name)) {
                      output.owner->invalid = true;
                      return;
                    }
                    output.name = name;
                  } catch (...) {
                    output.owner->invalid = true;
                  }
                };
                value.description = [](void*, wl_output*, const char*) {
                };
                return value;
              }();
              wl_output_add_listener(output->handle, &output_listener, output.get());
              client.outputs.push_back(std::move(output));
            } catch (...) {
              client.invalid = true;
            }
          }
        },
        [](void* data, wl_registry*, std::uint32_t id) {
          auto& client = *static_cast<HudWindow*>(data);
          if (id == client.compositor_id || id == client.shm_id || id == client.shell_id ||
              (client.selected && client.selected->id == id))
            client.closing = true;
          else {
            const auto removed =
                std::find_if(client.outputs.begin(), client.outputs.end(),
                             [id](const auto& output) { return output->id == id; });
            if (removed != client.outputs.end()) client.outputs.erase(removed);
          }
        }};
    wl_registry_add_listener(registry, &listener, this);
    synchronize();
    if (stopped || closing) return;
    synchronize();  // Bound output names arrive after registry enumeration.
    if (stopped || closing) return;
    require(compositor && shm && shell, "HUD requires named outputs and layer-shell");
    for (auto& output : outputs)
      if (output->name == output_name) {
        require(!selected, "duplicate configured HUD output name");
        selected = output.get();
      }
    // Fail before creating any surface. There is deliberately no default output.
    require(selected, "configured HUD output is unavailable");
    surface = wl_compositor_create_surface(compositor);
    require(surface, "cannot create HUD surface");
    layer = zwlr_layer_shell_v1_get_layer_surface(
        shell, surface, selected->handle, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "headunit.native-hud");
    require(layer, "cannot create HUD layer");
    static const zwlr_layer_surface_v1_listener layer_listener{
        [](void* data, zwlr_layer_surface_v1* layer, std::uint32_t serial, std::uint32_t w,
           std::uint32_t h) {
          auto& client = *static_cast<HudWindow*>(data);
          // The calibrated renderer fits its authored canvas over black. Accept
          // the initial acknowledged output size instead of stretching pixels to
          // a guessed physical mode. A live resize retires this bounded pool.
          if (w < 320 || w > 4096 || h < 160 || h > 2160 ||
              (client.configured && (w != client.width || h != client.height)))
            client.invalid = true;
          else {
            client.width = w;
            client.height = h;
          }
          zwlr_layer_surface_v1_ack_configure(layer, serial);
          client.configured = true;
        },
        [](void* data, zwlr_layer_surface_v1*) {
          static_cast<HudWindow*>(data)->closing = true;
        }};
    zwlr_layer_surface_v1_add_listener(layer, &layer_listener, this);
    zwlr_layer_surface_v1_set_size(layer, width, height);
    zwlr_layer_surface_v1_set_anchor(
        layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer, 0);
    auto* empty = wl_compositor_create_region(compositor);
    require(empty, "cannot create HUD input region");
    wl_surface_set_input_region(surface, empty);
    wl_region_destroy(empty);
    wl_surface_commit(surface);
    const auto deadline = headunit::Clock::now() + std::chrono::seconds(2);
    while (!configured && !closing && !stopped) {
      require(headunit::Clock::now() < deadline, "HUD configure timed out");
      dispatch(25);
    }
    if (!closing && !stopped) create_buffers();
  }

  void create_buffers() {
    static_assert(std::endian::native == std::endian::little);
    frame_bytes = std::size_t(width) * height * 4;
    mapped_bytes = frame_bytes * buffers.size();
    char path[] = "/dev/shm/headunit-hud-XXXXXX";
    const int fd = ::mkstemp(path);
    require(fd >= 0, "cannot create HUD display buffers");
    ::unlink(path);
    if (::ftruncate(fd, static_cast<off_t>(mapped_bytes)) != 0) {
      ::close(fd);
      throw std::runtime_error("cannot size HUD display buffers");
    }
    mapping = ::mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
      ::close(fd);
      throw std::runtime_error("cannot map HUD display buffers");
    }
    auto* pool = wl_shm_create_pool(shm, fd, static_cast<std::int32_t>(mapped_bytes));
    ::close(fd);
    require(pool, "cannot create HUD display buffer pool");
    static const wl_buffer_listener listener{[](void* data, wl_buffer*) {
      static_cast<Buffer*>(data)->busy = false;
    }};
    for (std::size_t index = 0; index < buffers.size(); ++index) {
      auto& buffer = buffers[index];
      buffer.handle =
          wl_shm_pool_create_buffer(pool, static_cast<std::int32_t>(index * frame_bytes), width,
                                    height, width * 4, WL_SHM_FORMAT_XRGB8888);
      if (!buffer.handle) {
        wl_shm_pool_destroy(pool);
        throw std::runtime_error("cannot create HUD display buffer");
      }
      wl_buffer_add_listener(buffer.handle, &listener, &buffer);
    }
    wl_shm_pool_destroy(pool);
  }

  bool present(const headunit::NativeHudContent& content, headunit::Clock::time_point now,
               const headunit::HudCalibration& calibration) {
    auto available = std::find_if(buffers.begin(), buffers.end(),
                                  [](const Buffer& buffer) { return !buffer.busy; });
    if (available == buffers.end()) return false;
    auto* pixels =
        static_cast<std::uint32_t*>(mapping) + (available - buffers.begin()) * (frame_bytes / 4);
    headunit::draw_native_hud(content, now, pixels, width, height, std::size_t(width) * 4,
                              calibration);
    if (frame) wl_callback_destroy(frame);
    frame = wl_surface_frame(surface);
    require(frame, "cannot request HUD frame callback");
    static const wl_callback_listener listener{
        [](void* data, wl_callback* callback, std::uint32_t) {
          static_cast<HudWindow*>(data)->frame = nullptr;
          wl_callback_destroy(callback);
        }};
    wl_callback_add_listener(frame, &listener, this);
    available->busy = true;
    wl_surface_attach(surface, available->handle, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    submitted = headunit::Clock::now();
    wl_surface_commit(surface);
    return true;
  }
};
}  // namespace

int main(int argc, char** argv) try {
  std::string config_file, vehicle_socket, navigation_socket, speed_limit_file;
  bool simulate = false;
  constexpr auto usage =
      "usage: headunit-native-hud-wayland --config PATH "
      "[--vehicle-socket PATH | --simulate-vehicle] [--navigation-socket PATH] [--speed-limit-file "
      "PATH]";
  for (int i = 1; i < argc;) {
    const std::string_view option(argv[i++]);
    const auto path = [&](std::string& destination) {
      if (!destination.empty() || i == argc || !*argv[i]) throw std::invalid_argument(usage);
      destination = argv[i++];
    };
    if (option == "--config")
      path(config_file);
    else if (option == "--vehicle-socket" && !simulate)
      path(vehicle_socket);
    else if (option == "--navigation-socket")
      path(navigation_socket);
    else if (option == "--speed-limit-file")
      path(speed_limit_file);
    else if (option == "--simulate-vehicle" && !simulate && vehicle_socket.empty())
      simulate = true;
    else if (option == "--help" && argc == 2) {
      std::cout << usage << '\n';
      return 0;
    } else
      throw std::invalid_argument(usage);
  }
  if (config_file.empty()) throw std::invalid_argument(usage);
  const auto config = headunit::load_hud_configuration(config_file);
  std::signal(SIGTERM, stop);
  std::signal(SIGINT, stop);
  HudOwner owner(config);  // Stable lock remains held through HudWindow teardown.
  HudWindow window;
  window.connect(config);
  if (stopped || window.closing) return 0;
  std::unique_ptr<headunit::VehicleSource> vehicle;
  if (simulate)
    vehicle = std::make_unique<headunit::SimulatedVehicle>(headunit::Clock::now());
  else if (!vehicle_socket.empty())
    vehicle = std::make_unique<headunit::SocketVehicle>(vehicle_socket);
  std::unique_ptr<headunit::SocketNavigation> navigation;
  if (!navigation_socket.empty())
    navigation = std::make_unique<headunit::SocketNavigation>(navigation_socket);
  const auto interval =
      std::chrono::ceil<std::chrono::microseconds>(std::chrono::duration<double>(1.0 / config.fps));
  auto next_frame = headunit::Clock::now();
  while (!stopped && !window.closing) {
    const auto due = [&] {
      return window.frame ? std::max(next_frame, window.submitted + std::chrono::seconds(1))
                          : next_frame;
    };
    const auto now = headunit::Clock::now();
    if (now >= due()) {
      headunit::NativeHudContent content;
      content.country = config.country;
      if (vehicle) content.vehicle = vehicle->sample(now);
      if (navigation) content.navigation = navigation->sample(now);
      if (!speed_limit_file.empty())
        content.speed_limit = headunit::read_hud_speed_limit(speed_limit_file, now);
      const bool submitted = window.present(content, now, config.calibration);
      next_frame = (submitted ? window.submitted : now) + interval;
    }
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(due() - headunit::Clock::now());
    window.dispatch(static_cast<int>(std::clamp<std::int64_t>(wait.count(), 0, 250)));
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << "native HUD: " << error.what() << '\n';
  return 1;
}
