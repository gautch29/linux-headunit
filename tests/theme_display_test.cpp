// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_display.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <csignal>
#include <charconv>
#include <cstdlib>
#include <limits>

using namespace headunit;
using namespace headunit::theme;
using namespace std::chrono_literals;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
std::uint64_t ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
template <class F>
void until(F test) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!test()) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(5ms);
  }
}
void isolated_cluster_output(graphics::GpuDisplay& main, script::Runtime& main_runtime,
                             bool preview) {
  const auto origin = ns();
  bool locked = true;
  auto source = [&]() -> std::optional<AirplayClockSample> {
    if (!locked) return {};
    const auto at = ns(), elapsed = at - origin;
    const auto media = ((std::uint64_t{2208989800} + elapsed / 1000000000) << 32) +
                       ((elapsed % 1000000000) << 32) / 1000000000;
    return AirplayClockSample{media, at, at, 1000};  // Explicit synthetic network clock.
  };
  const script::Display spec{"isolated-cluster", 96, 64};
  auto resources = [](std::string_view) -> std::string {
    throw std::runtime_error("no resource");
  };
  script::Runtime runtime(resources, spec);
  auto overlay = std::make_shared<graphics::Layer>();
  overlay->set_frame({{16, 16}, {16, 16}});
  overlay->background = {1, 0, 0, 1};
  graphics::GpuDisplayOptions options;
  options.window.opaque_background = true;
  if (preview) options.window.readback_render_node = "/dev/dri/renderD128";
  options.isolate_output_faults = true;
  options.overlay = [&](graphics::Size) {
    return std::shared_ptr<const graphics::Layer>(overlay);
  };
  unsigned faults = 0;
  options.isolated_output_fault = [&](std::string_view reason) {
    CHECK(!reason.empty() && reason.size() <= 512);
    ++faults;
  };
  graphics::GpuDisplay display(runtime, spec, source, options);
  script::UiWorker::DisplayTurn turn;
  until([&] {
    turn = display.turn(script::UiWorker::DisplayPhase::bootstrap);
    return turn.allow_tasks;
  });
  CHECK(turn.pts);
  runtime.tick(turn.pts);
  constexpr auto scene = R"(
    local root=CP.DPLayer.create()
    root.frame=CP.DPRect(0,0,96,64)
    root.backgroundColor=CP.DPColor(0,1,0,1)
    CP.setRootLayer(CP.enumerateOutputStreams()[1].identifier,root)
  )";
  runtime.evaluate(scene);
  runtime.start_tasks(turn.pts);
  until([&] {
    turn = display.turn();
    if (turn.allow_tasks) runtime.tick(turn.pts);
    return display.stats().scene_presentations > 0;
  });
  // A receiver-owned invalid layer reaches the real GPU scene validator. The
  // VM and its protocol state remain healthy; this is not a Lua fault fixture.
  overlay->opacity = std::numeric_limits<double>::quiet_NaN();
  until([&] {
    turn = display.turn();
    if (turn.allow_tasks) runtime.tick(turn.pts);
    return display.stats().output_isolated;
  });
  const auto failed = display.stats();
  const auto retained = turn.pts;
  CHECK(faults == 1 && retained && turn.allow_tasks && failed.faulted && failed.surface_withdrawn &&
        failed.waiting && !failed.pending && failed.isolated_output_faults == 1 &&
        !runtime.stats().faulted);
  for (unsigned i = 0; i < 5; ++i) {
    turn = display.turn();
    CHECK(turn.allow_tasks && turn.pts == retained);
    runtime.tick(turn.pts);
    CHECK(!display.turn(script::UiWorker::DisplayPhase::feedback).allow_tasks);
    CHECK(display.stats().submissions == failed.submissions &&
          display.stats().presentations == failed.presentations && faults == 1);
  }
  const auto main_scenes = main.stats().scene_presentations;
  until([&] {
    const auto healthy = main.turn();
    if (healthy.allow_tasks) main_runtime.tick(healthy.pts);
    return main.stats().scene_presentations > main_scenes;
  });
  CHECK(!main.stats().faulted);  // Real healthy-panel progress after cluster withdrawal.
  locked = false;
  turn = display.turn();
  CHECK(!turn.allow_tasks && !turn.pts);
  locked = true;
  turn = display.turn();
  CHECK(!turn.allow_tasks && !turn.pts);
  CHECK(display.stats().submissions == failed.submissions && faults == 1);
  // A fresh connection uses a fresh VM/display; relock never revives the failed
  // owner. Only real new bootstrap/scene receipts qualify this new renderer.
  script::Runtime fresh(resources, spec);
  auto fresh_options = options;
  fresh_options.overlay = {};
  graphics::GpuDisplay replacement(fresh, spec, source, fresh_options);
  until([&] {
    turn = replacement.turn(script::UiWorker::DisplayPhase::bootstrap);
    return turn.allow_tasks;
  });
  CHECK(turn.pts && *turn.pts > *retained && replacement.stats().presentations > 0);
  fresh.tick(turn.pts);
  fresh.evaluate(scene);
  fresh.start_tasks(turn.pts);
  until([&] {
    turn = replacement.turn();
    if (turn.allow_tasks) fresh.tick(turn.pts);
    return replacement.stats().scene_presentations > 0;
  });
  CHECK(!replacement.stats().faulted && !replacement.stats().output_isolated);
}
int main(int argc, char** argv) try {
  CHECK(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--preview"));
  const auto origin = ns();
  bool locked = true;
  std::uint64_t phase = 0;
  auto source = [&]() -> std::optional<AirplayClockSample> {
    if (!locked) return {};
    const auto at = ns(), elapsed = at - origin;
    // Synthetic network clock; all compositor timestamps/receipts are real.
    const auto media =
        ((900 + elapsed / 1000000000) << 32) + ((elapsed % 1000000000) << 32) / 1000000000;
    return AirplayClockSample{media + (std::uint64_t{2208988800} << 32) + phase, at, at, 1000};
  };
  script::Display spec{"fixture", 96, 64};
  script::Runtime runtime(
      [](std::string_view) -> std::string { throw std::runtime_error("no resource"); }, spec);
  graphics::GpuDisplayOptions options;
  unsigned overlay_calls = 0;
  auto native_overlay = std::make_shared<graphics::Layer>();
  native_overlay->set_frame({{16, 16}, {16, 16}});
  native_overlay->background = {1, 0, 0, 1};
  options.overlay = [&](graphics::Size output) {
    CHECK(output.width == 96 && output.height == 64);
    ++overlay_calls;
    return std::shared_ptr<const graphics::Layer>(native_overlay);
  };
  const bool overlap = std::getenv("HEADUNIT_THEME_TEST_OVERLAP_SCRIPT_TICK") != nullptr;
  options.overlap_script_tick = overlap;
  if (argc == 2) options.window.readback_render_node = "/dev/dri/renderD128";
  graphics::GpuDisplay display(runtime, spec, source, options);
  auto turn = display.turn();
  CHECK(!turn.allow_tasks && !turn.pts);  // Submission is never presentation.
  until([&] {
    turn = display.turn();
    return turn.allow_tasks;
  });
  CHECK(turn.pts && *turn.pts >= 900 && *turn.pts < 905 && display.stats().presentations == 1);
  runtime.tick(turn.pts);
  runtime.evaluate(R"(
    assert(CP.PTS ~= nil)
    local root=CP.DPLayer.create()
    root.frame=CP.DPRect(0,0,96,64)
    root.backgroundColor=CP.DPColor(0,1,0,1)
    CP.setRootLayer('fixture',root)
  )");
  runtime.start_tasks(turn.pts);
  auto last = *turn.pts;
  // Real compositor receipts may release the next scene between script turns.
  // This path may render, but it must never authorize an extra Lua tick.
  const auto eager_initial = display.stats().scene_presentations;
  until([&] {
    turn = display.turn(script::UiWorker::DisplayPhase::feedback);
    CHECK(!turn.allow_tasks);
    return display.stats().scene_presentations > eager_initial;
  });
  // A bootstrap hook may run after main has created a scene. It must leave no
  // outstanding scene receipt while another display performs slow module loads.
  until([&] {
    turn = display.turn(script::UiWorker::DisplayPhase::bootstrap);
    return turn.allow_tasks && !display.stats().pending;
  });
  std::this_thread::sleep_for(3100ms);
  until([&] {
    turn = display.turn(script::UiWorker::DisplayPhase::bootstrap);
    return turn.allow_tasks;
  });
  CHECK(turn.pts && *turn.pts > last && !display.stats().pending);
  CHECK(display.stats().owner_gap_suspensions >= 1);
  last = *turn.pts;
  unsigned inline_ticks = 0;
  // Keep calling bootstrap hooks while another entry point starts. No single
  // owner gap exceeds a second, but the previous correlation becomes old.
  for (unsigned i = 0; i < 4; ++i) {
    std::this_thread::sleep_for(350ms);
    turn = display.turn(script::UiWorker::DisplayPhase::bootstrap);
    CHECK(turn.allow_tasks && !display.stats().pending);
  }
  until([&] {
    turn = display.turn();
    CHECK(turn.pts && *turn.pts >= last);
    last = *turn.pts;
    if (turn.allow_tasks)
      runtime.tick(turn.pts);
    else {
      CHECK(overlap && turn.inline_task_ns > 0);
      ++inline_ticks;
    }
    return display.stats().scene_presentations >= 3;
  });
  const auto measured = display.stats();
  if (overlap) CHECK(inline_ticks > 0 && measured.overlap_ns >= turn.inline_task_ns);
  CHECK(measured.scene_compositions >= 3 && measured.scene_submissions >= 3);
  CHECK(measured.compose_ns > 0 && measured.max_compose_ns > 0 &&
        measured.compose_ns >= measured.max_compose_ns);
  CHECK(measured.submit_ns > 0 && measured.max_submit_ns > 0 &&
        measured.submit_ns >= measured.max_submit_ns);
  CHECK(measured.scene_submissions <= measured.scene_compositions);
  CHECK(overlay_calls == measured.scene_compositions && native_overlay->parent.expired());
  const auto ignored = display.stats().ignored_receipts;
  // Wait for a pending scene, then lose clock before consuming its receipt.
  until([&] {
    display.turn();
    return display.stats().submissions > display.stats().presentations;
  });
  locked = false;
  until([&] {
    turn = display.turn();
    CHECK(!turn.allow_tasks && !turn.pts && display.stats().waiting);
    return display.stats().ignored_receipts > ignored;
  });
  CHECK(display.stats().clock_unavailable_suspensions >= 1);
  CHECK(display.stats().suspensions ==
        display.stats().clock_epoch_suspensions + display.stats().clock_unavailable_suspensions +
            display.stats().owner_gap_suspensions + display.stats().feedback_delay_suspensions);
  locked = true;
  until([&] {
    turn = display.turn();
    return turn.allow_tasks;
  });
  CHECK(turn.pts && *turn.pts > last && !display.stats().faulted);
  const auto scenes = display.stats().scene_presentations;
  until([&] {
    turn = display.turn();
    if (turn.allow_tasks) runtime.tick(turn.pts);
    return display.stats().scene_presentations > scenes;
  });
  if (argc == 2) {
    const auto* raw = std::getenv("HEADUNIT_THEME_TEST_WESTON_PID");
    CHECK(raw);
    const std::string_view value(raw);
    int pid = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), pid);
    CHECK(error == std::errc{} && end == value.data() + value.size() && pid > 1);
    struct Resume {
      int pid;
      ~Resume() { ::kill(pid, SIGCONT); }
    } resume{pid};
    const auto before_stall = display.stats();
    CHECK(::kill(pid, SIGSTOP) == 0);
    const auto deadline = std::chrono::steady_clock::now() + 3300ms;
    while (std::chrono::steady_clock::now() < deadline) {
      turn = display.turn();
      std::this_thread::sleep_for(5ms);
    }
    CHECK(!turn.allow_tasks && !turn.pts && display.stats().waiting);
    CHECK(display.stats().feedback_delay_suspensions > before_stall.feedback_delay_suspensions);
    CHECK(::kill(pid, SIGCONT) == 0);
    until([&] {
      turn = display.turn();
      return turn.allow_tasks;
    });
    CHECK(turn.pts && !display.stats().faulted);
    CHECK(*turn.pts >= last && display.stats().ignored_receipts > before_stall.ignored_receipts);
  }
  isolated_cluster_output(display, runtime, argc == 2);
  // A phase jump cannot silently relabel an old presentation as current.
  phase = std::uint64_t{1} << 32;
  bool failed = false;
  try {
    (void)display.turn();
  } catch (const std::exception&) {
    failed = true;
  }
  CHECK(failed && display.stats().faulted && display.stats().waiting);
  std::cout << "GPU display: actual bootstrap/scene feedback, clock suspension, stale receipt "
               "rejection, isolated output and terminal faults passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
