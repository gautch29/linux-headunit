// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_worker.hpp>
#include <headunit/theme/output_fault.hpp>
#include <headunit/binary_plist.hpp>
#include <headunit/ultra_ui_envelope.hpp>
#include <array>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace headunit;
using namespace headunit::theme::script;
using namespace std::chrono_literals;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
const std::string center = "aabbccdd-0000-0000-0000-000000000001";
const std::string cluster = "aabbccdd-0000-0000-0000-000000000002";
template <class F>
void until(F predicate, std::chrono::milliseconds timeout = 3s) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::sleep_for(1ms);
  }
}
DataMessage message(const std::string& id, std::int64_t value) {
  DataMessage m;
  m.type = 0x636f6d6d;
  m.payload = encode_ultra_ui_envelope(
      {id, encode_binary_plist(PlistValue{PlistValue::Dictionary{{"value", PlistValue{value}}}})});
  return m;
}
UiWorker::Configuration config() {
  UiWorker::Configuration c;
  c.displays = {{"Center_Display"}, {"Cluster_Display"}};
  c.routes = {{center, "Center_Display"}, {cluster, "Cluster_Display"}};
  c.resources = [](std::string_view path) -> std::string {
    CHECK(path == "main.lua");
    return R"(
      CP.DPLuaTask.create():run(function()
        while true do
          assert(CP.PTS == nil)
          local m = CP.receiveMessage()
          if m then assert(CP.sendMessage(m)) end
          coroutine.yield()
        end
      end)
      return {}
    )";
  };
  return c;
}
void echo_and_pressure() {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto c = config();
  const auto caller = std::this_thread::get_id();
  c.configure = [&](Channel&) {
    CHECK(std::this_thread::get_id() != caller);
    entered.set_value();
    released.wait();
  };
  UiWorker worker(7, std::move(c));
  CHECK(entered.get_future().wait_for(3s) == std::future_status::ready);
  // While initialization is suspended, every receiver-facing operation works.
  const auto input = message(center, 42);
  bool bounded = !worker.receive(8, input);
  for (unsigned i = 0; i < 64; ++i)
    bounded &= worker.receive(7, input);
  bounded &= !worker.receive(7, input);
  bounded &= worker.status().state == UiWorker::State::starting;
  release.set_value();  // Release even if an assertion would fail.
  CHECK(bounded);
  until([&] { return worker.status().queued_output > 0 || worker.finished(); });
  CHECK(!worker.finished());
  std::optional<DataMessage> held;
  CHECK(!worker.send_pending([&](const DataMessage& m) {
    held = m;
    return false;
  }));
  CHECK(held && held->payload == input.payload);
  CHECK(!worker.send_pending([&](const DataMessage& m) {
    CHECK(m == *held);
    return false;
  }));
  std::size_t responses = 0;
  until([&] {
    while (worker.send_pending([&](const DataMessage& m) {
      CHECK(m.payload == input.payload);
      ++responses;
      return true;
    })) {
    }
    return responses == 64;
  });
  CHECK(worker.status().received == 64 && worker.status().sent == 64);
  const auto second = message(cluster, 99);
  CHECK(worker.receive(7, second));
  until([&] {
    return worker.send_pending([&](const DataMessage& m) {
      CHECK(m.payload == second.payload);
      return true;
    });
  });
  worker.request_stop();
  CHECK(!worker.receive(7, input));
  CHECK(!worker.send_pending([](const auto&) { return true; }));
  until([&] { return worker.finished(); });
  CHECK(worker.status().state == UiWorker::State::stopped);
}
void stop_during_startup(bool presentation = false) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<bool> loaded = false;
  auto c = config();
  if (presentation)
    c.presentation = [&](Runtime&, const Display&, UiWorker::DisplayPhase) {
      entered.set_value();
      released.wait();
      return UiWorker::DisplayTurn{0.0, true};
    };
  else
    c.configure = [&](Channel&) {
      entered.set_value();
      released.wait();
    };
  c.resources = [&](std::string_view) -> std::string {
    loaded = true;
    return "return {}";
  };
  UiWorker worker(9, std::move(c));
  CHECK(entered.get_future().wait_for(3s) == std::future_status::ready);
  DataMessage large;
  large.payload.resize(512 * 1024);
  bool queued = true;
  for (unsigned i = 0; i < 3; ++i)
    queued &= worker.receive(9, large);
  queued &= !worker.receive(9, large);  // Byte bound, before the count bound.
  large.payload.push_back(0);
  queued &= !worker.receive(9, large);  // Per-message bound.
  worker.request_stop();
  const auto status = worker.status();
  release.set_value();
  CHECK(queued && status.state == UiWorker::State::stopping && status.queued_input == 0);
  until([&] { return worker.finished(); });
  CHECK(!loaded && worker.status().state == UiWorker::State::stopped);
}
void faults_and_new_connection() {
  {
    auto c = config();
    c.resources = [](std::string_view) -> std::string {
      return "error('fixture startup fault')";
    };
    UiWorker failed(10, std::move(c));
    until([&] { return failed.finished(); });
    CHECK(failed.status().state == UiWorker::State::faulted);
    CHECK(!failed.receive(10, message(center, 1)));
  }
  UiWorker fresh(11, config());
  until([&] { return fresh.status().state == UiWorker::State::running; });
  DataMessage malformed;
  malformed.type = 0x636f6d6d;
  malformed.payload = {0xff};
  CHECK(fresh.receive(11, malformed));
  until([&] { return fresh.finished(); });
  CHECK(fresh.status().state == UiWorker::State::faulted && fresh.status().queued_output == 0);
  auto c = config();
  c.limits.frame_ms = 5;
  c.limits.startup_ms = 100;
  c.resources = [](std::string_view) -> std::string {
    return R"(
    CP.DPLuaTask.create():run(function()
      coroutine.yield()
      while true do end
    end)
    return {}
  )";
  };
  UiWorker overdue(12, std::move(c));
  until([&] { return overdue.finished(); });
  CHECK(overdue.status().state == UiWorker::State::faulted);
}
void presentation_hook() {
  auto c = config();
  c.resources = [](std::string_view) -> std::string {
    return R"(
    assert(CP.PTS == 100)
    CP.DPLuaTask.create():run(function()
      assert(CP.PTS == 100)
      assert(CP.sendMessage(CP.createPlist({value=100})))
    end)
    return {}
  )";
  };
  const auto caller = std::this_thread::get_id();
  c.presentation = [caller](Runtime&, const Display&, UiWorker::DisplayPhase) {
    CHECK(std::this_thread::get_id() != caller);
    return UiWorker::DisplayTurn{100.0, true};  // Explicit synthetic compositor fixture.
  };
  UiWorker worker(13, std::move(c));
  std::size_t responses = 0;
  until([&] {
    CHECK(!worker.finished());
    while (worker.send_pending([&](const DataMessage& m) {
      CHECK(m.payload == message(center, 100).payload ||
            m.payload == message(cluster, 100).payload);
      ++responses;
      return true;
    })) {
    }
    return responses == 2;
  });
  worker.request_stop();
  until([&] { return worker.finished(); });
}
void blocked_socket_expires() {
  auto c = config();
  c.resources = [](std::string_view) -> std::string {
    return "assert(CP.sendMessage(CP.createPlist({value=1}))); return {}";
  };
  UiWorker worker(14, std::move(c));
  until([&] { return worker.status().queued_output == 2; });
  CHECK(!worker.send_pending([](const DataMessage&) { return false; }));
  until([&] { return worker.finished(); }, 7s);
  CHECK(worker.status().state == UiWorker::State::faulted && worker.status().queued_output == 0);
  CHECK(worker.failure_reason() == "UI transport stalled");
}
void suspended_display_withholds_tasks() {
  std::atomic<bool> allow = false, loaded = false;
  std::atomic<unsigned> paused_calls = 0, ticks = 0;
  auto c = config();
  c.resources = [&](std::string_view) -> std::string {
    loaded = true;
    return R"(
      assert(CP.PTS==100)
      CP.DPLuaTask.create():run(function()
        while true do CP.log(3,'fixture','tick'); coroutine.yield() end
      end)
      return {}
    )";
  };
  c.configure = [&](Channel& channel) {
    for (const auto* name : {"Center_Display", "Cluster_Display"})
      channel.runtime(name).bind_log_sink(
          [&](int, std::string_view, std::string_view) { ++ticks; });
  };
  c.presentation = [&](Runtime&, const Display&, UiWorker::DisplayPhase) {
    const bool active = allow.load();
    if (!active) ++paused_calls;
    return UiWorker::DisplayTurn{active ? std::optional<double>{100} : std::nullopt, active};
  };
  UiWorker worker(15, std::move(c));
  until([&] { return paused_calls > 0; });
  CHECK(!loaded && worker.status().state == UiWorker::State::starting);
  allow = true;
  until([&] { return worker.status().state == UiWorker::State::running && ticks >= 4; });
  const auto before_pause = paused_calls.load();
  allow = false;
  until([&] { return paused_calls >= before_pause + 4; });
  const auto at_pause = ticks.load(), observed = paused_calls.load();
  until([&] {
    CHECK(ticks == at_pause);
    return paused_calls >= observed + 10;
  });
  CHECK(!worker.finished());
  allow = true;
  until([&] { return ticks > at_pause; });
  worker.request_stop();
  until([&] { return worker.finished(); });
}
void feedback_polls_do_not_tick() {
  auto c = config();
  c.frame_interval_ns = 66666667;
  c.feedback_poll_ns = 1000000;
  std::atomic<unsigned> polls = 0, frames = 0;
  c.resources = [](std::string_view) -> std::string {
    return R"(
      CP.DPLuaTask.create():run(function()
        while true do assert(CP.PTS==100); coroutine.yield() end
      end)
      return {}
    )";
  };
  c.presentation = [&](Runtime&, const Display&, UiWorker::DisplayPhase phase) {
    if (phase == UiWorker::DisplayPhase::feedback) {
      ++polls;
      return UiWorker::DisplayTurn{{}, true};  // Must be ignored by the worker.
    }
    if (phase == UiWorker::DisplayPhase::frame) ++frames;
    return UiWorker::DisplayTurn{100.0, true};
  };
  UiWorker worker(16, std::move(c));
  until([&] { return (polls >= 4 && frames >= 4) || worker.finished(); });
  CHECK(!worker.finished() && worker.status().state == UiWorker::State::running);
  worker.request_stop();
  until([&] { return worker.finished(); });
  CHECK(worker.status().state == UiWorker::State::stopped);
}
struct PanelCounts {
  std::atomic<unsigned> loaded{0}, started{0}, ticks{0}, bootstrap{0};
};
UiWorker::Configuration deferred_config(std::array<PanelCounts, 2>& panels) {
  auto c = config();
  c.feedback_poll_ns = 1000000;
  c.configure = [&](Channel& channel) {
    for (std::size_t i = 0; i < panels.size(); ++i) {
      // Bind to the fixed display set, without retaining a stack Configuration
      // or a Runtime outside its owning worker thread.
      auto& runtime = channel.runtime(i ? "Cluster_Display" : "Center_Display");
      runtime.bind_log_sink([&, i](int, std::string_view, std::string_view value) {
        if (value == "loaded")
          ++panels[i].loaded;
        else if (value == "started")
          ++panels[i].started;
        else if (value == "tick")
          ++panels[i].ticks;
      });
    }
  };
  c.resources = [](std::string_view path) -> std::string {
    CHECK(path == "main.lua");
    return R"(
      local expected=CP.enumerateOutputStreams()[1].identifier=='Center_Display' and 100 or 200
      assert(CP.PTS==expected)
      CP.log(3,'fixture','loaded')
      CP.DPLuaTask.create():run(function()
        assert(CP.PTS==expected)
        CP.log(3,'fixture','started')
        while true do
          assert(CP.PTS==expected)
          local m=CP.receiveMessage()
          if m then assert(CP.sendMessage(m)) end
          CP.log(3,'fixture','tick')
          coroutine.yield()
        end
      end)
      return {}
    )";
  };
  return c;
}
void deferred_panel_and_resume(bool after_evaluation) {
  std::array<PanelCounts, 2> panels;
  std::atomic<bool> hidden = true, ready = false;
  auto c = deferred_config(panels);
  c.presentation = [&](Runtime&, const Display& display, UiWorker::DisplayPhase phase) {
    const std::size_t i = display.identifier == "Center_Display" ? 0 : 1;
    if (phase == UiWorker::DisplayPhase::bootstrap) {
      ++panels[i].bootstrap;
      if (!i && (!after_evaluation || panels[0].loaded > 0)) {
        if (hidden) return UiWorker::DisplayTurn{{}, false, 0, true};
        if (!ready) return UiWorker::DisplayTurn{{}, false};
      }
    }
    if (phase == UiWorker::DisplayPhase::feedback) return UiWorker::DisplayTurn{{}, false};
    return UiWorker::DisplayTurn{i ? 200. : 100., true};  // Explicit synthetic receipt fixture.
  };
  UiWorker worker(after_evaluation ? 101 : 100, std::move(c));
  const auto id = after_evaluation ? 101u : 100u;
  until([&] { return panels[1].ticks >= 4; });
  CHECK(!worker.finished() && worker.status().state == UiWorker::State::running &&
        panels[0].loaded == unsigned(after_evaluation) && panels[0].started == 0 &&
        panels[0].ticks == 0);
  const auto waiting = message(center, 41), active = message(cluster, 99);
  CHECK(worker.receive(id, waiting) && worker.receive(id, active));
  std::size_t main_responses = 0, cluster_responses = 0;
  const auto receive = [&] {
    while (worker.send_pending([&](const DataMessage& response) {
      if (response.payload == waiting.payload)
        ++main_responses;
      else {
        CHECK(response.payload == active.payload);
        ++cluster_responses;
      }
      return true;
    })) {
    }
  };
  until([&] {
    receive();
    return cluster_responses == 1;
  });
  CHECK(main_responses == 0 && worker.status().received == 2);
  const auto calls = panels[0].bootstrap.load(), cluster_ticks = panels[1].ticks.load();
  hidden = false;
  until([&] { return panels[0].bootstrap >= calls + 4; });
  CHECK(panels[0].started == 0 && panels[0].loaded == unsigned(after_evaluation) &&
        panels[1].ticks > cluster_ticks);
  ready = true;
  until([&] {
    receive();
    return main_responses == 1 && panels[0].ticks >= 4;
  });
  CHECK(panels[0].loaded == 1 && panels[0].started == 1 && panels[1].loaded == 1 &&
        panels[1].started == 1);
  const auto before = panels[0].ticks.load();
  hidden = true;
  until([&] {
    return panels[0].ticks >= before + 4;
  });  // Started main uses cached actual PTS; it cannot start twice.
  CHECK(panels[0].loaded == 1 && panels[0].started == 1 && !worker.finished());
  worker.request_stop();
  until([&] { return worker.finished(); });
  CHECK(worker.status().state == UiWorker::State::stopped);
}
void deferred_input_bound_and_stop() {
  std::array<PanelCounts, 2> panels;
  auto c = deferred_config(panels);
  c.presentation = [&](Runtime&, const Display& display, UiWorker::DisplayPhase phase) {
    if (display.identifier == "Center_Display") {
      CHECK(phase == UiWorker::DisplayPhase::bootstrap);
      ++panels[0].bootstrap;
      return UiWorker::DisplayTurn{{}, false, 0, true};
    }
    return UiWorker::DisplayTurn{200., phase != UiWorker::DisplayPhase::feedback};
  };
  UiWorker worker(102, std::move(c));
  until([&] { return panels[1].ticks >= 2; });
  const auto input = message(center, 1);
  for (unsigned i = 0; i < 64; ++i)
    CHECK(worker.receive(102, input));
  until([&] { return worker.status().received == 64; });
  CHECK(worker.receive(102, input));
  const auto ticks = panels[1].ticks.load();
  until([&] { return panels[1].ticks >= ticks + 4; });
  CHECK(worker.status().received == 64 && worker.status().queued_input == 1 &&
        worker.status().queued_output == 0 && panels[0].loaded == 0 && panels[0].started == 0 &&
        !worker.finished());
  // The sixty-fifth route command stays backpressured. It is never dropped or
  // echoed/acknowledged to conceal the deferred Runtime's normal queue bound.
  const auto began = std::chrono::steady_clock::now();
  worker.request_stop();
  until([&] { return worker.finished(); }, 1s);
  CHECK(std::chrono::steady_clock::now() - began < 1s &&
        worker.status().state == UiWorker::State::stopped);
}
void malformed_deferral() {
  const std::array bad_bootstrap = {UiWorker::DisplayTurn{{}, true, 0, true},
                                    UiWorker::DisplayTurn{100., false, 0, true},
                                    UiWorker::DisplayTurn{{}, false, 1, true}};
  for (const auto bad : bad_bootstrap) {
    auto c = config();
    c.presentation = [bad](Runtime&, const Display&, UiWorker::DisplayPhase) {
      return bad;
    };
    UiWorker worker(103, std::move(c));
    until([&] { return worker.finished(); });
    CHECK(worker.status().state == UiWorker::State::faulted &&
          worker.failure_reason() == "invalid deferred display startup turn");
  }
  for (const auto bad_phase : {UiWorker::DisplayPhase::frame, UiWorker::DisplayPhase::feedback}) {
    auto c = config();
    c.feedback_poll_ns = 1000000;
    c.resources = [](std::string_view) -> std::string {
      return "return {}";
    };
    c.presentation = [bad_phase](Runtime&, const Display&, UiWorker::DisplayPhase phase) {
      return phase == bad_phase ? UiWorker::DisplayTurn{{}, false, 0, true}
                                : UiWorker::DisplayTurn{100., true};
    };
    UiWorker worker(104, std::move(c));
    until([&] { return worker.finished(); });
    CHECK(worker.status().state == UiWorker::State::faulted &&
          worker.failure_reason() == "invalid deferred display startup turn");
  }
  std::array<PanelCounts, 2> panels;
  auto c = deferred_config(panels);
  c.presentation = [&](Runtime&, const Display& display, UiWorker::DisplayPhase) {
    if (display.identifier == "Center_Display") {
      if (panels[0].bootstrap++ == 0) return UiWorker::DisplayTurn{{}, false, 0, true};
      return UiWorker::DisplayTurn{
          {}, true};  // Deferral cannot be escaped with an invented nil-PTS startup.
    }
    return UiWorker::DisplayTurn{200., true};
  };
  UiWorker worker(105, std::move(c));
  until([&] { return worker.finished(); });
  CHECK(panels[0].loaded == 0 && worker.status().state == UiWorker::State::faulted &&
        worker.failure_reason() == "deferred display startup needs a presented timestamp");
}
void deferred_bootstrap_deadline() {
  std::array<PanelCounts, 2> panels;
  std::atomic<bool> hidden = true;
  auto c = deferred_config(panels);
  c.presentation = [&](Runtime&, const Display& display, UiWorker::DisplayPhase) {
    if (display.identifier == "Center_Display") {
      ++panels[0].bootstrap;
      return UiWorker::DisplayTurn{{}, false, 0, hidden.load()};
    }
    return UiWorker::DisplayTurn{200., true};
  };
  UiWorker worker(106, std::move(c));
  until([&] { return panels[1].ticks >= 2; });
  const auto began = std::chrono::steady_clock::now();
  hidden = false;
  until([&] { return worker.finished(); }, 12s);
  CHECK(std::chrono::steady_clock::now() - began >= 10s && panels[0].loaded == 0 &&
        panels[1].ticks > 100 && worker.status().state == UiWorker::State::faulted &&
        worker.failure_reason() == "UI display bootstrap timed out");
}
void fault_survives_stop_during_cleanup() {
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  struct Cleanup {
    std::promise<void>& entered;
    std::shared_future<void> release;
    ~Cleanup() {
      entered.set_value();
      release.wait_for(3s);
    }
  };
  auto cleanup = std::make_shared<Cleanup>(entered, gate);
  auto c = config();
  c.resources = [](std::string_view) -> std::string {
    return "error('retained startup fault')";
  };
  c.presentation = [cleanup](Runtime&, const Display&, UiWorker::DisplayPhase) {
    return UiWorker::DisplayTurn{{}, true};
  };
  cleanup.reset();
  UiWorker worker(99, std::move(c));
  c = {};  // A moved-from std::function may retain a copy of its small target.
  const bool cleaning = entered.get_future().wait_for(2s) == std::future_status::ready;
  const auto reason = worker.failure_reason();
  worker.request_stop();
  release.set_value();
  until([&] { return worker.finished(); });
  if (!cleaning || reason.find("retained startup fault") == reason.npos)
    throw std::runtime_error("cleanup gate=" + std::to_string(cleaning) + " reason=" + reason);
  CHECK(!worker.failure_reason().empty());
  CHECK(worker.failure_reason() == reason && worker.status().state == UiWorker::State::stopped);
}
// Synthetic output receipts exercise the real two-VM worker and UI transport.
// They do not establish GPU recovery, visible gauges or physical presentation.
struct OutputFaultProbe {
  std::atomic<bool> inject_cluster_failure{false}, clock_ready{true}, owner_cleanup{false};
  std::atomic<std::uint64_t> clock_epoch{7};
  std::atomic<unsigned> faults{0}, cluster_receipts{0}, control_frames{0}, retired_frames{0},
      feedback_turns{0};
};
struct OutputFaultOwner {
  OutputFaultProbe& probe;
  std::optional<headunit::theme::graphics::OutputFaultIsolation> policy;
  std::optional<double> actual_cluster_pts;
  std::thread::id owner;
  explicit OutputFaultOwner(OutputFaultProbe& value) : probe(value) {}
  ~OutputFaultOwner() {
    if (policy) probe.owner_cleanup = std::this_thread::get_id() == owner;
  }
};
struct SyntheticClusterOutputFailure final : std::runtime_error {
  SyntheticClusterOutputFailure() : std::runtime_error("fixture cluster output failure") {}
};
UiWorker::Configuration output_fault_config(std::array<PanelCounts, 2>& panels,
                                            OutputFaultProbe& probe, bool enabled = true,
                                            bool script_failure = false) {
  namespace graphics = headunit::theme::graphics;
  auto c = deferred_config(panels);
  auto state = std::make_shared<OutputFaultOwner>(probe);
  const auto bind = c.configure;
  c.configure = [bind, state, enabled](Channel& channel) {
    state->owner = std::this_thread::get_id();
    state->policy.emplace(enabled);  // Policy creation and every call stay on this owner.
    bind(channel);
  };
  if (script_failure) {
    const auto resources = c.resources;
    c.resources = [resources](std::string_view path) {
      auto source = resources(path);
      const std::string echo = "if m then assert(CP.sendMessage(m)) end";
      const auto at = source.find(echo);
      CHECK(at != source.npos);
      source.replace(at, echo.size(), R"(
        if m then
          if expected==200 and CP.parsePlist(m).value==-999 then
            error('fixture isolated cluster runtime fault')
          end
          assert(CP.sendMessage(m))
        end
      )");
      return source;
    };
  }
  c.presentation = [&, state](Runtime& runtime, const Display& display,
                              UiWorker::DisplayPhase phase) {
    if (display.identifier == "Center_Display")
      return UiWorker::DisplayTurn{
          phase == UiWorker::DisplayPhase::feedback ? std::nullopt : std::optional<double>{100.},
          phase != UiWorker::DisplayPhase::feedback};
    CHECK(display.identifier == "Cluster_Display" && state->policy);
    const graphics::OutputClockQualification clock{probe.clock_ready.load(),
                                                   probe.clock_epoch.load()};
    if (phase == UiWorker::DisplayPhase::bootstrap) {
      ++panels[1].bootstrap;
      if (!state->actual_cluster_pts) {
        state->actual_cluster_pts = 200.;
        ++probe.cluster_receipts;
      }
      return UiWorker::DisplayTurn{state->actual_cluster_pts, true};
    }
    if (!state->policy->isolated()) {
      try {
        if (probe.inject_cluster_failure) throw SyntheticClusterOutputFailure{};
      } catch (const SyntheticClusterOutputFailure&) {
        const graphics::OutputFaultContext context{panels[1].started == 1, runtime.stats().faulted,
                                                   state->actual_cluster_pts, clock};
        if (state->policy->fault(context) == graphics::OutputFaultAction::propagate) throw;
        ++probe.faults;
      }
    }
    if (state->policy->isolated()) {
      const auto control = state->policy->control_turn(clock, runtime.stats().faulted);
      if (phase == UiWorker::DisplayPhase::feedback) ++probe.feedback_turns;
      if (control.escalate) throw std::runtime_error("fixture terminal Runtime fault");
      if (control.allow_tasks) {
        CHECK(control.pts == state->actual_cluster_pts && control.pts == 200.);
        if (phase == UiWorker::DisplayPhase::frame) ++probe.control_frames;
      } else {
        CHECK(!control.pts);
        if (phase == UiWorker::DisplayPhase::frame) ++probe.retired_frames;
      }
      // No draw/submission/receipt is generated by the control-only path.
      return UiWorker::DisplayTurn{control.pts,
                                   control.allow_tasks && phase == UiWorker::DisplayPhase::frame};
    }
    return UiWorker::DisplayTurn{
        phase == UiWorker::DisplayPhase::feedback ? std::nullopt : state->actual_cluster_pts,
        phase != UiWorker::DisplayPhase::feedback};
  };
  return c;
}
void output_fault_keeps_main_and_control_transport() {
  std::array<PanelCounts, 2> panels;
  OutputFaultProbe probe;
  auto c = output_fault_config(panels, probe);
  c.feedback_poll_ns = 1000000;  // Bounded receipt polls must never authorize tasks.
  UiWorker worker(200, std::move(c));
  c = {};
  until([&] { return panels[0].ticks >= 2 && panels[1].ticks >= 2; });
  CHECK(panels[0].started == 1 && panels[1].started == 1 && probe.cluster_receipts == 1);
  const auto main_before = panels[0].ticks.load(), cluster_before = panels[1].ticks.load();
  probe.inject_cluster_failure = true;
  until(
      [&] { return probe.faults == 1 && probe.control_frames >= 3 && probe.feedback_turns >= 4; });
  CHECK(!worker.finished() && worker.status().state == UiWorker::State::running &&
        panels[0].ticks > main_before && panels[1].ticks > cluster_before &&
        probe.cluster_receipts == 1);
  const auto main_command = message(center, 41), cluster_command = message(cluster, 99);
  CHECK(worker.receive(200, main_command) && worker.receive(200, cluster_command));
  until([&] { return worker.status().queued_output == 2; });
  std::optional<DataMessage> retained;
  CHECK(!worker.send_pending([&](const DataMessage& response) {
    retained = response;
    return false;
  }));
  CHECK(retained);
  CHECK(!worker.send_pending([&](const DataMessage& response) {
    CHECK(response == *retained);
    return false;
  }));
  unsigned main_responses = 0, cluster_responses = 0;
  while (worker.send_pending([&](const DataMessage& response) {
    if (response.payload == main_command.payload)
      ++main_responses;
    else {
      CHECK(response.payload == cluster_command.payload);
      ++cluster_responses;
    }
    return true;
  })) {
  }
  CHECK(main_responses == 1 && cluster_responses == 1 && worker.status().received == 2 &&
        worker.status().sent == 2 && probe.cluster_receipts == 1);
  // deferred_config's script asserts its original 100/200 PTS on every task
  // turn, so successful echoes prove containment did not invent a newer PTS.
  CHECK(panels[0].loaded == 1 && panels[1].loaded == 1 && panels[1].started == 1);
  const auto began = std::chrono::steady_clock::now();
  worker.request_stop();
  CHECK(!worker.receive(200, cluster_command));
  until([&] { return worker.finished(); }, 1s);
  CHECK(std::chrono::steady_clock::now() - began < 1s && probe.owner_cleanup &&
        worker.status().state == UiWorker::State::stopped && worker.status().queued_input == 0 &&
        worker.status().queued_output == 0 && worker.failure_reason().empty());
}
void output_fault_clock_retirement(bool replace_epoch) {
  std::array<PanelCounts, 2> panels;
  OutputFaultProbe probe;
  auto c = output_fault_config(panels, probe);
  const auto id = replace_epoch ? 202u : 201u;
  UiWorker worker(id, std::move(c));
  c = {};
  until([&] { return panels[0].ticks >= 2 && panels[1].ticks >= 2; });
  probe.inject_cluster_failure = true;
  until([&] { return probe.faults == 1 && probe.control_frames >= 2; });
  if (replace_epoch)
    probe.clock_epoch = 8;
  else
    probe.clock_ready = false;
  until([&] { return probe.retired_frames >= 2; });
  const auto cluster_at_retirement = panels[1].ticks.load(),
             main_at_retirement = panels[0].ticks.load();
  const auto main_command = message(center, 43), cluster_command = message(cluster, 101);
  CHECK(worker.receive(id, main_command) && worker.receive(id, cluster_command));
  unsigned main_responses = 0;
  const auto drain = [&] {
    while (worker.send_pending([&](const DataMessage& response) {
      CHECK(response.payload == main_command.payload);
      ++main_responses;
      return true;
    })) {
    }
  };
  until([&] {
    drain();
    return main_responses == 1 && worker.status().received == 2 &&
           panels[0].ticks >= main_at_retirement + 3;
  });
  CHECK(panels[1].ticks == cluster_at_retirement && probe.cluster_receipts == 1 &&
        !worker.finished());
  // Relock in the original epoch, or readiness in a replacement epoch, cannot
  // restore the retired actual PTS or run/acknowledge the waiting cluster task.
  probe.clock_ready = true;
  const auto retired_before = probe.retired_frames.load(), main_before = panels[0].ticks.load();
  until([&] {
    drain();
    return probe.retired_frames >= retired_before + 3 && panels[0].ticks >= main_before + 3;
  });
  CHECK(main_responses == 1 && panels[1].ticks == cluster_at_retirement && panels[1].loaded == 1 &&
        panels[1].started == 1 && probe.cluster_receipts == 1);
  const auto began = std::chrono::steady_clock::now();
  worker.request_stop();
  until([&] { return worker.finished(); }, 1s);
  CHECK(std::chrono::steady_clock::now() - began < 1s && probe.owner_cleanup &&
        worker.status().state == UiWorker::State::stopped && worker.status().queued_input == 0 &&
        worker.status().queued_output == 0);
}
void output_fault_does_not_hide_runtime_script_error() {
  std::array<PanelCounts, 2> panels;
  OutputFaultProbe probe;
  auto c = output_fault_config(panels, probe, true, true);
  UiWorker worker(203, std::move(c));
  c = {};
  until([&] { return panels[0].ticks >= 2 && panels[1].ticks >= 2; });
  probe.inject_cluster_failure = true;
  until([&] { return probe.faults == 1 && probe.control_frames >= 2; });
  CHECK(worker.receive(203, message(cluster, -999)));
  until([&] { return worker.finished(); }, 1s);
  CHECK(worker.status().state == UiWorker::State::faulted && probe.owner_cleanup &&
        worker.failure_reason().find("fixture isolated cluster runtime fault") !=
            std::string::npos &&
        worker.status().queued_output == 0 && !worker.receive(203, message(center, 1)));
  CHECK(probe.cluster_receipts == 1 && panels[1].started == 1);
}
void output_fault_disabled_policy_still_propagates() {
  std::array<PanelCounts, 2> panels;
  OutputFaultProbe probe;
  auto c = output_fault_config(panels, probe, false);
  UiWorker worker(204, std::move(c));
  c = {};
  until([&] { return panels[0].ticks >= 2 && panels[1].ticks >= 2; });
  probe.inject_cluster_failure = true;
  until([&] { return worker.finished(); }, 1s);
  CHECK(worker.status().state == UiWorker::State::faulted && probe.owner_cleanup &&
        worker.failure_reason() == "fixture cluster output failure" && probe.faults == 0 &&
        probe.control_frames == 0 && probe.cluster_receipts == 1);
}
void command_diagnostic_owner_and_boundary() {
  using Dict = PlistValue::Dictionary;
  auto c = config();
  const auto caller = std::this_thread::get_id();
  std::atomic<unsigned> incoming = 0, outgoing = 0;
  std::atomic<bool> valid = true;
  c.command_diagnostic_limit = 2;
  c.command_diagnostic = [&](const UiCommandDiagnostic& event) {
    if (std::this_thread::get_id() == caller || event.panel != 0 || event.command_type != 8 ||
        event.focus_token != 23)
      valid = false;
    if (event.direction == UiCommandDiagnostic::Direction::incoming_vm_queue) {
      if (event.count != 1 || event.budget_exhausted) valid = false;
      ++incoming;
    } else {
      if (event.count != 2 || !event.budget_exhausted) valid = false;
      ++outgoing;
    }
    throw std::runtime_error("optional diagnostic sink failure");
  };
  UiWorker worker(7, std::move(c));
  c = {};
  // Independent synthetic fixture. These events observe queue boundaries,
  // without claiming the asset executed focus or a socket transmitted it.
  const auto command = encode_binary_plist(
      PlistValue{Dict{{"type", PlistValue{std::int64_t(8)}},
                      {"data", PlistValue{Dict{{"tokenID", PlistValue{std::int64_t(23)}}}}}}});
  DataMessage input;
  input.type = 0x636f6d6d;
  input.payload = encode_ultra_ui_envelope(
      {center, encode_binary_plist(PlistValue{
                   Dict{{"type", PlistValue{std::int64_t(2)}}, {"data", PlistValue{command}}}})});
  CHECK(worker.receive(7, input));
  until([&] { return worker.status().queued_output == 1 || worker.finished(); });
  CHECK(!worker.finished() && valid && incoming == 1 && outgoing == 1);
  CHECK(!worker.send_pending([&](const DataMessage& value) {
    CHECK(value.payload == input.payload);
    return false;
  }));
  CHECK(incoming == 1 && outgoing == 1);  // A socket retry cannot add worker-queue evidence.
  CHECK(worker.send_pending([&](const DataMessage& value) {
    CHECK(value.payload == input.payload);
    return true;
  }));
  CHECK(incoming == 1 && outgoing == 1);
  worker.request_stop();
  until([&] { return worker.finished(); });
  CHECK(worker.failure_reason().empty());
}
int main() try {
  echo_and_pressure();
  stop_during_startup();
  stop_during_startup(true);
  faults_and_new_connection();
  presentation_hook();
  blocked_socket_expires();
  suspended_display_withholds_tasks();
  feedback_polls_do_not_tick();
  deferred_panel_and_resume(false);
  deferred_panel_and_resume(true);
  deferred_input_bound_and_stop();
  malformed_deferral();
  deferred_bootstrap_deadline();
  fault_survives_stop_during_cleanup();
  output_fault_keeps_main_and_control_transport();
  output_fault_clock_retirement(false);
  output_fault_clock_retirement(true);
  output_fault_does_not_hide_runtime_script_error();
  output_fault_disabled_policy_still_propagates();
  command_diagnostic_owner_and_boundary();
  std::cout << "Output fault isolation: 5 two-VM worker/transport cases passed\n";
  std::cout << "UI worker: owner isolation, ordered flow, bounds, cancellation and faults passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
