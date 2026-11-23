// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Exercise the actual native owner, ordered remap and late callback retirement
// on a private compositor. No signed pack/GPU or physical presentation claim.
#include <wayland-client.h>
void fixture_surface_commit(wl_surface*);
#define wl_surface_commit fixture_surface_commit
#define main native_cluster_entry_point
#include "../src/cluster_wayland.cpp"
#undef main
#undef wl_surface_commit
#include <sys/wait.h>
#include <thread>
#include <headunit/presentation_feedback_timeout.hpp>

namespace {
bool pause_at_commit = false;
pid_t fixture_compositor = 0;
int startup_marker = -1;
}  // namespace
void fixture_surface_commit(wl_surface* surface) {
  if (pause_at_commit) {
    pause_at_commit = false;
    kill(fixture_compositor, SIGSTOP);
    const char marker = 'C';
    (void)write(startup_marker, &marker, 1);
  }
  wl_surface_commit(surface);
}
namespace {
void require(bool value) {
  if (!value) throw std::runtime_error("native fallback preservation check failed");
}
template <class F>
void await(Client& client, F predicate) {
  const auto deadline = headunit::Clock::now() + std::chrono::seconds(3);
  while (!predicate()) {
    require(headunit::Clock::now() < deadline);
    while (wl_display_prepare_read(client.display) != 0)
      require(wl_display_dispatch_pending(client.display) >= 0);
    const int flushed = wl_display_flush(client.display);
    require(flushed >= 0 || errno == EAGAIN);
    pollfd fd{wl_display_get_fd(client.display), POLLIN, 0};
    if (poll(&fd, 1, 10) > 0 && (fd.revents & POLLIN))
      require(wl_display_read_events(client.display) >= 0);
    else
      wl_display_cancel_read(client.display);
    require(wl_display_dispatch_pending(client.display) >= 0);
    require(wl_display_get_error(client.display) == 0 && !client.closing);
  }
}
bool startup_signal(pid_t compositor, bool layer, bool configure_wait) {
  int markers[2];
  require(pipe(markers) == 0);
  if (!configure_wait) require(kill(compositor, SIGSTOP) == 0);
  const pid_t child = fork();
  require(child >= 0);
  if (!child) {
    close(markers[0]);
    pause_at_commit = configure_wait;
    fixture_compositor = compositor;
    startup_marker = markers[1];
    char name[] = "native-startup-fixture", size[] = "--size", width[] = "1280";
    char height[] = "720", background[] = "--background";
    char* args[]{name, size, width, height, background};
    _exit(native_cluster_entry_point(layer ? 5 : 4, args));
  }
  close(markers[1]);
  if (configure_wait) {
    pollfd ready{markers[0], POLLIN, 0};
    if (poll(&ready, 1, 2000) <= 0) {
      kill(child, SIGKILL);
      waitpid(child, nullptr, 0);
      close(markers[0]);
      kill(compositor, SIGCONT);
      throw std::runtime_error("startup fixture did not reach production configure boundary");
    }
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  close(markers[0]);
  require(kill(child, SIGTERM) == 0);
  int status = 0;
  const auto deadline = headunit::Clock::now() + std::chrono::seconds(1);
  bool reaped = false;
  while (headunit::Clock::now() < deadline) {
    if (waitpid(child, &status, WNOHANG) == child) {
      reaped = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!reaped) {
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
  }
  require(kill(compositor, SIGCONT) == 0);
  const bool clean = reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  std::cout << "Production startup SIGTERM " << (configure_wait ? "configure" : "registry")
            << " cleanExit=" << clean << '\n';
  return clean;
}
void configure_fixture(Client& client, bool layer) {
  client.background = layer;
  client.width = client.buffer_width = 1280;
  client.height = client.buffer_height = 720;
  client.frame_bytes = std::size_t(client.width) * client.height * 4;
  client.mapped_bytes = client.frame_bytes * buffer_count;
  client.display = wl_display_connect(nullptr);
  require(client.display != nullptr);
  auto* registry = wl_display_get_registry(client.display);
  wl_registry_add_listener(registry, &registry_listener, &client);
  require(wl_display_roundtrip(client.display) >= 0);
  wl_registry_destroy(registry);
  require(client.compositor && client.shm && client.viewporter);
  client.surface = wl_compositor_create_surface(client.compositor);
  client.viewport = wp_viewporter_get_viewport(client.viewporter, client.surface);
  wp_viewport_set_destination(client.viewport, client.width, client.height);
  if (layer) {
    require(client.layer_shell != nullptr);
    client.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        client.layer_shell, client.surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        "headunit.backup-cluster");
    zwlr_layer_surface_v1_add_listener(client.layer_surface, &layer_listener, &client);
    zwlr_layer_surface_v1_set_size(client.layer_surface, client.width, client.height);
    zwlr_layer_surface_v1_set_anchor(client.layer_surface, 15);
    zwlr_layer_surface_v1_set_exclusive_zone(client.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(client.layer_surface, 0);
  } else {
    require(client.wm != nullptr);
    xdg_wm_base_add_listener(client.wm, &wm_listener, &client);
    client.shell_surface = xdg_wm_base_get_xdg_surface(client.wm, client.surface);
    xdg_surface_add_listener(client.shell_surface, &surface_listener, &client);
    client.toplevel = xdg_surface_get_toplevel(client.shell_surface);
    xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
    xdg_toplevel_set_fullscreen(client.toplevel, nullptr);
  }
  wl_surface_commit(client.surface);
  await(client, [&] { return client.configured; });
  require(!client.configure_size_mismatch);
  create_buffers(client);
}
void paint(Client& client) {
  const auto now = headunit::Clock::now();
  // Fresh explicit simulator acquisition, passed unchanged to the actual native
  // renderer. This is neither a physical observation nor a phone video frame.
  headunit::SimulatedVehicle source(now);
  headunit::ClusterContent content;
  content.vehicle = source.sample(now);
  content.center = headunit::ClusterCenter::trip;
  require(present(client, content, now));
  await(client, [&] { return !client.frame; });
}
}  // namespace
int main(int argc, char**) try {
  const auto* value = std::getenv("HEADUNIT_THEME_TEST_WESTON_PID");
  require(value != nullptr);
  const auto compositor = static_cast<pid_t>(std::stoi(value));
  require(compositor > 1);
  struct Resume {
    pid_t pid;
    ~Resume() { kill(pid, SIGCONT); }
  } resume{compositor};
  const bool registry_stopped = startup_signal(compositor, argc > 1, false);
  const bool configure_stopped = startup_signal(compositor, argc > 1, true);
  require(registry_stopped && configure_stopped);
  unsigned signed_starts = 0;
  headunit::run_cluster_presenters(
      [&] {
        ++signed_starts;
        throw headunit::PresentationFeedbackTimeout();
      },
      [&](headunit::ClusterReturn& recovery, std::uint64_t generation) {
        Client client;
        client.recovery = &recovery;
        client.recovery_generation = generation;
        configure_fixture(client, argc > 1);
        paint(client);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        paint(client);
        require(recovery.request_return(headunit::Clock::now()));
        auto* original_display = client.display;
        const auto original_surface_id =
            wl_proxy_get_id(reinterpret_cast<wl_proxy*>(client.surface));
        auto* original_buffer = client.buffers.front().handle;
        const auto token = client.frame_token;
        require(kill(compositor, SIGSTOP) == 0);
        const auto began = headunit::Clock::now();
        require(!finish_native_return(client));  // Must recover in SAME native owner.
        require(headunit::Clock::now() - began >= std::chrono::seconds(2));
        require(client.display == original_display &&
                wl_proxy_get_id(reinterpret_cast<wl_proxy*>(client.surface)) !=
                    original_surface_id &&
                client.buffers.front().handle == original_buffer);
        require(!recovery.request_return(headunit::Clock::now()) && signed_starts == 1);
        require(!client.configured);
        headunit::ClusterContent unavailable;
        require(!present(client, unavailable, headunit::Clock::now()));
        require(kill(compositor, SIGCONT) == 0);
        await(client, [&] { return client.configured; });  // Late sync + real remap configure.
        paint(client);
        require(client.frame_token > token && !recovery.request_return(headunit::Clock::now()));
        require(wl_display_roundtrip(client.display) >= 0);  // Late old stack callback is retired.
        require(signed_starts == 1 && client.recovery_generation == generation);

        // Genuine ack remains usable; no stale timeout sync can grant another retry.
        require(retire_native_for_return(client) == NativeUnmap::acknowledged);
        prepare_native_remap(client);
        await(client, [&] { return client.configured; });
        paint(client);
        require(!recovery.request_return(headunit::Clock::now()));
        require(kill(compositor, SIGSTOP) == 0);
        const auto previous = std::signal(SIGTERM, stop);
        const auto stopped_at = headunit::Clock::now();
        std::thread terminal([] {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          kill(getpid(), SIGTERM);
        });
        try {
          require(finish_native_return(client));
        } catch (...) {
          terminal.join();
          std::signal(SIGTERM, previous);
          throw;
        }
        terminal.join();
        std::signal(SIGTERM, previous);
        require(stopping && headunit::Clock::now() - stopped_at < std::chrono::seconds(2));
        require(kill(compositor, SIGCONT) == 0);
        require(wl_display_roundtrip(client.display) >= 0);
      },
      [] { return headunit::Clock::now(); }, [] { return stopping != 0; },
      [](std::string_view, bool) {}, true);
  require(signed_starts == 1);
  std::cout << "Actual native owner retained: timeout/late ack -> same owner/connection/buffers "
               "with renewed native surface "
               "-> new configure/fresh native commit/actual frame callback; no signed successor; "
               "terminal and successful acknowledgement paths passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
