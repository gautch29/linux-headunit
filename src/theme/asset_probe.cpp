// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/asset_bundle.hpp"
#include "drive_mode_probe.hpp"
#include <charconv>
// Offline compatibility probe. No Apple source or asset bytes are embedded.
#include <headunit/theme/script_channel.hpp>
#include <headunit/theme/ui_transport.hpp>
#include <headunit/theme/ui_worker.hpp>
#include <headunit/ultra_ui_envelope.hpp>
#include <headunit/binary_plist.hpp>
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
#include "bench_video.hpp"
#endif
#include <headunit/theme/local_vehicle_profile.hpp>
#include <headunit/theme/scene_lifecycle.hpp>
#include <headunit/theme/brand_greeting.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <headunit/theme/gauge_input.hpp>
#include <headunit/vehicle.hpp>
#ifdef HEADUNIT_THEME_HAVE_SKIA
#include <headunit/theme/gpu_compositor.hpp>
#include <headunit/theme/gpu_display.hpp>
#endif
#ifdef HEADUNIT_THEME_HAVE_WAYLAND
#include <headunit/theme/wayland_presenter.hpp>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace headunit::theme;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
Bytes read_root(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() <= 0 || f.tellg() > 65536)
    throw std::runtime_error("invalid trust root file");
  Bytes bytes(static_cast<std::size_t>(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
    throw std::runtime_error("cannot read trust root");
  return bytes;
}
void worker_probe(const script::ResourceReader& asset, const char* profile_path, bool gpu = false) {
  std::atomic<std::size_t> declarations = 0;
  std::array<std::atomic<std::size_t>, 2> scene_counts{}, report_counts{};
  struct BenchPanel {
    std::atomic<std::uint64_t> compositions = 0, compose_ns = 0, max_compose_ns = 0,
                               pending_turns = 0;
    std::atomic<std::uint64_t> receipts = 0, present_ns = 0, poll_ns = 0;
    std::atomic<std::uint64_t> zero_copy_presentations = 0;
    std::atomic<std::uint64_t> preparation_ns = 0, traversal_ns = 0, flush_ns = 0, wait_ns = 0;
    std::atomic<std::uint64_t> max_preparation_ns = 0, max_traversal_ns = 0, max_flush_ns = 0,
                               max_wait_ns = 0;
    std::atomic<std::uint64_t> retained_hits = 0, retained_builds = 0, stroke_hits = 0,
                               stroke_builds = 0, group_saves = 0;
    std::atomic<std::uint64_t> stroke_raster_ns = 0;
    std::atomic<std::uint64_t> max_deferred_gpu_frames = 0;
    std::atomic<std::uint64_t> max_gap_ns = 0, gaps_over_20ms = 0, gaps_over_25ms = 0;
  };
  std::array<BenchPanel, 2> bench_stats;
  struct BenchNative {
    std::atomic<std::uint64_t> submitted = 0, decoded = 0, presented = 0, late = 0, superseded = 0,
                               max_staged = 0;
    std::atomic<std::uint64_t> discarded = 0, clock_dropped = 0, visible_draws = 0, import_ns = 0;
  };
  std::array<BenchNative, 2> native_stats;
#ifdef HEADUNIT_THEME_HAVE_SKIA
  std::array<std::vector<graphics::GpuSlowFrameTrace>, 2> slow_frames;
  std::array<std::vector<graphics::GpuPresentationGapTrace>, 2> slow_gaps;
  const bool trace_slow_frames = std::getenv("HEADUNIT_THEME_PROBE_SLOW_FRAMES") != nullptr;
#endif
  const std::array names{"Center_Display", "Cluster_Display"};
  const std::array<std::string, 2> ids{"aabbccdd-0000-0000-0000-000000000001",
                                       "aabbccdd-0000-0000-0000-000000000002"};
  std::array<bool, 2> included{true, true};
  if (const auto* one = std::getenv("HEADUNIT_THEME_PROBE_SINGLE_PANEL")) {
    if (std::string_view(one) == names[0])
      included[1] = false;
    else if (std::string_view(one) == names[1])
      included[0] = false;
    else
      throw std::invalid_argument("unknown probe panel");
  }
  const bool benchmark = std::getenv("HEADUNIT_THEME_PROBE_BENCH_60HZ") != nullptr;
  if (benchmark && !gpu) throw std::invalid_argument("60 Hz probe needs GPU display mode");
  const std::array<const char*, 2> video_paths{std::getenv("HEADUNIT_THEME_PROBE_MAIN_VIDEO"),
                                               std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_VIDEO")};
  const bool with_video = video_paths[0] || video_paths[1];
  if (with_video &&
      (!benchmark || !video_paths[0] || !video_paths[1] || !included[0] || !included[1]))
    throw std::invalid_argument("native video benchmark needs two fixtures and both GPU panels");
  [[maybe_unused]] const auto video_origin =
      std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
  auto replay = with_video ? std::make_shared<bench::VideoReplay>(video_paths) : nullptr;
#else
  if (with_video) throw std::invalid_argument("probe was built without native video");
#endif
  unsigned cluster_render_width = 1920;
  if (benchmark)
    if (const auto* render_width = std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_RENDER_WIDTH")) {
      const std::string_view value(render_width);
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), cluster_render_width);
      if (error != std::errc{} || end != value.data() + value.size() ||
          (cluster_render_width != 1920 && cluster_render_width != 1280 &&
           cluster_render_width != 960))
        throw std::invalid_argument("invalid cluster benchmark render width");
    }
  script::UiWorker::Configuration config;
  std::atomic<std::uint64_t> worker_max_ns = 0, worker_missed_slots = 0;
  std::atomic<std::uint64_t> worker_turns = 0, worker_owner_ns = 0;
  std::array<std::atomic<std::uint64_t>, 2> worker_presentation_ns{}, worker_task_ns{};
  if (benchmark)
    config.frame_report = [&](const script::UiWorker::FrameReport& report) {
      auto observed = worker_max_ns.load();
      while (observed < report.max_owner_ns &&
             !worker_max_ns.compare_exchange_weak(observed, report.max_owner_ns)) {
      }
      worker_missed_slots += report.missed_slots;
      worker_turns += report.turns;
      worker_owner_ns += report.owner_ns;
      for (unsigned i = 0; i < 2; ++i) {
        worker_presentation_ns[i] += report.presentation_ns[i];
        worker_task_ns[i] += report.task_ns[i];
      }
    };
  if (benchmark)
    if (const auto* interval = std::getenv("HEADUNIT_THEME_PROBE_EAGER_PRESENTATION")) {
      const std::string_view value(interval);
      unsigned milliseconds = 0;
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), milliseconds);
      if (error != std::errc{} || end != value.data() + value.size() || milliseconds < 1 ||
          milliseconds > 8)
        throw std::invalid_argument("invalid eager presentation interval");
      config.feedback_poll_ns = std::uint64_t(milliseconds) * 1000000;
    }
  config.resources = asset;
  for (unsigned i = 0; i < 2; ++i)
    if (included[i]) {
      config.displays.push_back({names[i], 1920, 720});
      config.routes.push_back({ids[i], names[i]});
    }
  config.limits.startup_ms = 10000;
  // Offline-only compositor fixture. Full main requires an initial PTS even
  // before foregrounding. Do not derive this sample from CPU elapsed time or
  // present it as a live receipt. Keep the ordinary 20 ms turn bound.
  config.presentation = [](script::Runtime&, const script::Display&,
                           script::UiWorker::DisplayPhase) {
    return script::UiWorker::DisplayTurn{0.0, true};
  };
  const auto profile_bytes = read_root(profile_path);
  config.configure = [profile_bytes, &declarations, included, names](script::Channel& channel) {
    auto profile = vehicle::decode_local_profile(profile_bytes);
    for (unsigned i = 0; i < 2; ++i)
      if (included[i]) {
        const auto* display = names[i];
        channel.runtime(display).bind_vehicle(profile.source, profile.bindings);
        channel.runtime(display).bind_verification_reporter(
            [display, &declarations](const script::VerificationReport& report) {
              CHECK(report.display_identifier == display);
              ++declarations;
              // Declaration collector only: no pixels or gauge values verified.
            });
      }
  };
#ifdef HEADUNIT_THEME_HAVE_SKIA
  if (gpu) {
    struct Outputs {
      std::array<std::unique_ptr<graphics::GpuDisplay>, 2> displays;
      std::unique_ptr<vehicle::GaugeInput> gauges;
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
      std::array<std::shared_ptr<video::NativeInput>, 2> inputs;
      std::array<std::shared_ptr<graphics::Layer>, 2> video_backdrops;
#endif
      std::uint64_t origin = 0;
    };
    auto outputs = std::make_shared<Outputs>();
    outputs->origin = video_origin;
    config.configure = [profile_bytes, outputs, included, benchmark, cluster_render_width,
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
                        replay,
#endif
                        trace_slow_frames, &slow_frames, &slow_gaps](script::Channel& channel) {
      auto profile = vehicle::decode_local_profile(profile_bytes);
      if (std::getenv("HEADUNIT_THEME_PROBE_SIMULATE_GAUGES"))
        outputs->gauges = std::make_unique<vehicle::GaugeInput>(
            profile, std::make_unique<headunit::SimulatedVehicle>(headunit::Clock::now()));
      const std::array names{"Center_Display", "Cluster_Display"};
      for (std::size_t index = 0; index < names.size(); ++index) {
        if (!included[index]) continue;
        auto& runtime = channel.runtime(names[index]);
        runtime.bind_vehicle(profile.source, profile.bindings);
        if (index == 1) {
          runtime.select_remote_map_component();
          if (const auto* layout = std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT"))
            runtime.select_startup_layout(layout);
        }
        graphics::GpuDisplayOptions options;
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
        if (replay) {
          auto stream = runtime.add_video_input({"iPhoneVideo", 1920, 720});
          outputs->inputs[index] =
              std::make_shared<video::NativeInput>(stream, replay->queues[index], "v4l2slh265dec");
          options.inputs.push_back(outputs->inputs[index]);
          auto background = std::make_shared<graphics::Layer>(graphics::Kind::video);
          background->name = "Synthetic benchmark video backdrop";
          background->set_frame({{0, 0}, {1920, 720}});
          background->video = std::make_shared<video::View>();
          background->video->stream = stream;
          background->video->subscribed = true;
          outputs->video_backdrops[index] = std::move(background);
        }
#endif
        options.window.title = std::string("Independent Ultra worker probe: ") + names[index];
        options.window.defer_scene_fence =
            std::getenv("HEADUNIT_THEME_PROBE_DEFER_FENCE") != nullptr;
        options.window.opaque_background =
            std::getenv("HEADUNIT_THEME_PROBE_OPAQUE_WINDOWS") != nullptr;
        if (std::getenv("HEADUNIT_THEME_PROBE_TWO_PENDING"))
          options.window.max_pending_presentations = 2;
        if (const auto* depth = std::getenv(index == 0 ? "HEADUNIT_THEME_PROBE_MAIN_PENDING"
                                                       : "HEADUNIT_THEME_PROBE_CLUSTER_PENDING")) {
          const std::string_view value(depth);
          unsigned parsed = 0;
          const auto [end, error] =
              std::from_chars(value.data(), value.data() + value.size(), parsed);
          if (error != std::errc{} || end != value.data() + value.size() || parsed < 1 ||
              parsed > 2)
            throw std::invalid_argument("invalid per-panel presentation queue depth");
          options.window.max_pending_presentations = parsed;
        }
        if (const auto* socket = std::getenv(index == 0 ? "HEADUNIT_THEME_PROBE_MAIN_WAYLAND"
                                                        : "HEADUNIT_THEME_PROBE_CLUSTER_WAYLAND"))
          options.window.display_name = socket;
        if (const auto* output = std::getenv(index == 0 ? "HEADUNIT_THEME_PROBE_MAIN_OUTPUT"
                                                        : "HEADUNIT_THEME_PROBE_CLUSTER_OUTPUT"))
          options.window.output_name = output;
        options.window.app_id = index ? "org.headunit.ultra.cluster" : "org.headunit.ultra.main";
        options.overlap_script_tick =
            std::getenv("HEADUNIT_THEME_PROBE_OVERLAP_SCRIPT_TICK") != nullptr;
        if (index == 1) options.asset_canvas = {1920, 720};
        // Explicit offline cold-scene budget and cadence, never a live policy.
        options.scene_completion_timeout_ns = 2000000000;
        options.frame_interval_ns = benchmark ? headunit::ui_frame_period_ns : 200000000;
        if (benchmark && trace_slow_frames) {
          options.slow_frame_threshold_ns = 16000000;
          options.slow_frame = [&slow_frames, index](const graphics::GpuSlowFrameTrace& trace) {
            auto& top = slow_frames[index];
            if (top.size() < 32) {
              top.push_back(trace);
              return;
            }
            const auto smallest = std::min_element(
                top.begin(), top.end(),
                [](const auto& a, const auto& b) { return a.compose_ns < b.compose_ns; });
            if (trace.compose_ns > smallest->compose_ns) *smallest = trace;
          };
          options.presentation_gap_threshold_ns = 20000000;
          options.presentation_gap = [&slow_gaps,
                                      index](const graphics::GpuPresentationGapTrace& trace) {
            if (trace.scene_ordinal <= 64) return;  // Omit first-scene cache/shader warmup.
            auto& top = slow_gaps[index];
            if (top.size() < 32) {
              top.push_back(trace);
              return;
            }
            const auto smallest =
                std::min_element(top.begin(), top.end(),
                                 [](const auto& a, const auto& b) { return a.gap_ns < b.gap_ns; });
            if (trace.gap_ns > smallest->gap_ns) *smallest = trace;
          };
        }
        outputs->displays[index] = std::make_unique<graphics::GpuDisplay>(
            runtime,
            script::Display{names[index], index == 1 ? cluster_render_width : 1920,
                            index == 1 ? cluster_render_width * 3 / 8 : 720},
            [origin = outputs->origin]() {
              const auto at = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count());
              const auto elapsed = at - origin;
              const auto media = ((900 + elapsed / 1000000000) << 32) +
                                 ((elapsed % 1000000000) << 32) / 1000000000;
              // Simulated phone synchronization; Wayland receipts are real.
              return std::optional<headunit::AirplayClockSample>{
                  {media + (std::uint64_t{2208988800} << 32), at, at, 1000}};
            },
            options);
      }
    };
    config.presentation = [outputs, &scene_counts, &report_counts, &bench_stats, &native_stats](
                              script::Runtime& runtime, const script::Display& display,
                              script::UiWorker::DisplayPhase phase) {
      const auto index = display.identifier == "Center_Display" ? 0u : 1u;
      if (outputs->gauges && phase == script::UiWorker::DisplayPhase::frame)
        outputs->gauges->pump(headunit::Clock::now());
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
      if (const auto background = outputs->video_backdrops[index])
        if (auto root = runtime.scene_root()) {
          // Explicit offline augmentation: the signed entry point still paints
          // every instrument/mask/filter, but no phone negotiates its map/video
          // layer. Add our original moving backdrop only on this VM owner. The
          // Runtime owns a mutable Layer; the public view is const to consumers.
          auto owner = std::const_pointer_cast<graphics::Layer>(root);
          // The reference pack has an opaque full-canvas backing container. Its
          // background is painted before children; place our video inside it so
          // that black backing cannot hide the synthetic input. Other pack
          // structures are not qualified by this reference-layout benchmark.
          for (const auto& child : owner->children) {
            const auto frame = child->frame();
            if (child != background && child->kind == graphics::Kind::layer && !child->hidden &&
                child->opacity == 1 && child->background.a == 1 && frame.origin.x == 0 &&
                frame.origin.y == 0 && frame.size.width == 1920 && frame.size.height == 720) {
              owner = child;
              break;
            }
          }
          if (background->parent.lock() != owner) owner->insert(background, 0);
        }
#endif
      auto& output = *outputs->displays[index];
      const auto result = output.turn(phase);
      const auto status = output.stats();
      scene_counts[index] = status.scene_presentations;
      report_counts[index] = status.verification_declarations;
      bench_stats[index].compositions = status.scene_compositions;
      bench_stats[index].compose_ns = status.compose_ns;
      bench_stats[index].max_compose_ns = status.max_compose_ns;
      bench_stats[index].pending_turns = status.pending_turns;
      bench_stats[index].receipts = status.measured_scene_receipts;
      bench_stats[index].zero_copy_presentations = status.scene_zero_copy_presentations;
      bench_stats[index].present_ns = status.present_latency_ns;
      bench_stats[index].poll_ns = status.presentation_poll_ns;
      bench_stats[index].preparation_ns = status.preparation_ns;
      bench_stats[index].traversal_ns = status.traversal_ns;
      bench_stats[index].flush_ns = status.flush_ns;
      bench_stats[index].wait_ns = status.completion_wait_ns;
      bench_stats[index].max_preparation_ns = status.max_preparation_ns;
      bench_stats[index].max_traversal_ns = status.max_traversal_ns;
      bench_stats[index].max_flush_ns = status.max_flush_ns;
      bench_stats[index].max_wait_ns = status.max_completion_wait_ns;
      bench_stats[index].retained_hits = status.cache.hits;
      bench_stats[index].retained_builds = status.cache.builds;
      bench_stats[index].stroke_hits = status.cache.stroke_hits;
      bench_stats[index].stroke_builds = status.cache.stroke_builds;
      bench_stats[index].group_saves = status.cache.group_saves;
      bench_stats[index].stroke_raster_ns = status.cache.stroke_raster_ns;
      bench_stats[index].max_deferred_gpu_frames = status.cache.max_deferred_gpu_frames;
      bench_stats[index].max_gap_ns = status.max_scene_present_gap_ns;
      bench_stats[index].gaps_over_20ms = status.scene_gaps_over_20ms;
      bench_stats[index].gaps_over_25ms = status.scene_gaps_over_25ms;
      native_stats[index].import_ns = status.import_ns;
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
      if (const auto input = outputs->inputs[index]) {
        const auto v = input->stats();
        auto& s = native_stats[index];
        s.submitted = v.submitted;
        s.decoded = v.decoded;
        s.presented = v.presented;
        s.late = v.late;
        s.superseded = v.superseded;
        s.discarded = v.discarded;
        s.clock_dropped = v.clock_dropped;
        s.max_staged = v.max_staged;
        if (status.scene_compositions > 0 && phase == script::UiWorker::DisplayPhase::frame) {
          const auto visible = [&](auto&& self,
                                   const std::shared_ptr<const graphics::Layer>& layer) -> bool {
            if (!layer || layer->hidden || !layer->opacity) return false;
            if (layer->video && layer->bounds.size.width > 0 && layer->bounds.size.height > 0 &&
                layer->video->native())
              return true;
            for (const auto& child : layer->children)
              if (self(self, child)) return true;
            return false;
          };
          if (visible(visible, runtime.scene_root())) ++s.visible_draws;
        }
      }
#else
      (void)runtime;
#endif
      return result;
    };
    outputs.reset();  // GPU owners must die with worker callbacks on that thread.
  }
#else
  CHECK(!gpu);
#endif
  script::UiWorker worker(7, std::move(config));
  const auto began = std::chrono::steady_clock::now();
  unsigned bench_seconds = 8;
  if (benchmark)
    if (const auto* duration = std::getenv("HEADUNIT_THEME_PROBE_BENCH_SECONDS")) {
      const std::string_view value(duration);
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), bench_seconds);
      if (error != std::errc{} || end != value.data() + value.size() || bench_seconds < 1 ||
          bench_seconds > 120)
        throw std::invalid_argument("invalid benchmark duration");
    }
  const auto run_for = std::chrono::seconds(benchmark ? bench_seconds : 1);
  std::size_t polls = 0, messages = 0;
  std::optional<std::chrono::steady_clock::time_point> running;
  std::map<std::string, std::int64_t> synthetic_sequences;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    CHECK(now - began < run_for + std::chrono::seconds(30));
    const auto status = worker.status();
    ++polls;
    if (status.state == script::UiWorker::State::faulted)
      throw std::runtime_error("private worker diagnostic: " + worker.failure_reason());
    if (status.state == script::UiWorker::State::running && !running) running = now;
    std::vector<headunit::DataMessage> replies;
    while (worker.send_pending([&](const headunit::DataMessage& message) {
      ++messages;
      if (with_video) {
        // Local peer acknowledgements only; no emulated navigation/UI payloads.
        auto envelope = headunit::decode_ultra_ui_envelope(message.payload, ids);
        auto value = headunit::decode_binary_plist(envelope.binary_plist);
        auto& dictionary = std::get<headunit::PlistValue::Dictionary>(value.value);
        const auto type = std::get<std::int64_t>(dictionary.at("type").value);
        if (type == 0 || type == 2) {
          auto& sequence = synthetic_sequences[envelope.display_uuid];
          if (type == 0) sequence = 0;
          dictionary["type"] = headunit::PlistValue{std::int64_t(type == 0 ? 1 : 3)};
          dictionary["asn"] = dictionary.at("psn");
          dictionary["psn"] = headunit::PlistValue{++sequence};
          dictionary.erase("data");
          dictionary.erase("noAck");
          envelope.binary_plist = headunit::encode_binary_plist(value);
          headunit::DataMessage reply;
          reply.kind = headunit::DataMessageKind::asynchronous;
          reply.type = 0x636f6d6d;
          reply.payload = headunit::encode_ultra_ui_envelope(envelope);
          replies.push_back(std::move(reply));
        }
      }
      return true;
    })) {
    }
    for (const auto& reply : replies)
      CHECK(worker.receive(7, reply));
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
    if (replay && running)
      replay->pump(
          std::uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()),
          video_origin);
#endif
    if (running && now - *running >= run_for &&
        (!gpu ||
         ((!included[0] || scene_counts[0] >= 2) && (!included[1] || scene_counts[1] >= 2))))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(with_video ? 2 : 10));
  }
  worker.request_stop();
  while (!worker.finished()) {
    CHECK(std::chrono::steady_clock::now() - began < run_for + std::chrono::seconds(30));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(worker.status().state == script::UiWorker::State::stopped);
  if (with_video)
    for (const auto& stats : native_stats) {
      std::cerr << "syntheticVideoQualification: submitted=" << stats.submitted
                << " decoded=" << stats.decoded << " presented=" << stats.presented
                << " visibleTurns=" << stats.visible_draws << " importNs=" << stats.import_ns
                << " late=" << stats.late << " clockDropped=" << stats.clock_dropped << '\n';
      CHECK(stats.decoded >= 60 && stats.presented >= 30 && stats.visible_draws >= 30 &&
            stats.import_ns > 0);
    }
  std::cout << "{\"workerFullEntryPoints\":" << int(included[0]) + int(included[1])
            << ",\"hostPollsDuringStartupAndRun\":" << polls << ",\"outgoingMessages\":" << messages
            << ",\"verificationDeclarations\":"
            << declarations.load() + report_counts[0].load() + report_counts[1].load()
            << ",\"simulatedPhoneClock\":true,\"livePhoneTested\":false,\"actualWaylandFeedback\":"
            << (gpu ? "true" : "false") << ",\"scenePresentations\":[" << scene_counts[0].load()
            << ',' << scene_counts[1].load() << "]"
            << ",\"workerPhaseCosts\":{\"turns\":" << worker_turns << ",\"ownerMeanMs\":"
            << (worker_turns ? worker_owner_ns.load() / double(worker_turns.load()) / 1e6 : 0)
            << ",\"displayMeanMs\":[";
  for (unsigned i = 0; i < 2; ++i) {
    if (i) std::cout << ',';
    std::cout << (worker_turns
                      ? worker_presentation_ns[i].load() / double(worker_turns.load()) / 1e6
                      : 0);
  }
  std::cout << "],\"scriptMeanMs\":[";
  for (unsigned i = 0; i < 2; ++i) {
    if (i) std::cout << ',';
    std::cout << (worker_turns ? worker_task_ns[i].load() / double(worker_turns.load()) / 1e6 : 0);
  }
  std::cout << "]}"
            << ",\"benchPanels\":[";
  for (unsigned i = 0; i < 2; ++i) {
    if (i) std::cout << ',';
    const auto composed = bench_stats[i].compositions.load(),
               receipts = bench_stats[i].receipts.load();
    std::cout << "{\"compositions\":" << composed << ",\"composeMeanMs\":"
              << (composed ? bench_stats[i].compose_ns.load() / double(composed) / 1e6 : 0)
              << ",\"composeMaxMs\":" << bench_stats[i].max_compose_ns.load() / 1e6
              << ",\"preparationMeanMs\":"
              << (composed ? bench_stats[i].preparation_ns.load() / double(composed) / 1e6 : 0)
              << ",\"traversalMeanMs\":"
              << (composed ? bench_stats[i].traversal_ns.load() / double(composed) / 1e6 : 0)
              << ",\"flushMeanMs\":"
              << (composed ? bench_stats[i].flush_ns.load() / double(composed) / 1e6 : 0)
              << ",\"completionWaitMeanMs\":"
              << (composed ? bench_stats[i].wait_ns.load() / double(composed) / 1e6 : 0)
              << ",\"preparationMaxMs\":" << bench_stats[i].max_preparation_ns.load() / 1e6
              << ",\"traversalMaxMs\":" << bench_stats[i].max_traversal_ns.load() / 1e6
              << ",\"flushMaxMs\":" << bench_stats[i].max_flush_ns.load() / 1e6
              << ",\"completionWaitMaxMs\":" << bench_stats[i].max_wait_ns.load() / 1e6
              << ",\"retainedHits\":" << bench_stats[i].retained_hits.load()
              << ",\"retainedBuilds\":" << bench_stats[i].retained_builds.load()
              << ",\"strokeHits\":" << bench_stats[i].stroke_hits.load()
              << ",\"strokeBuilds\":" << bench_stats[i].stroke_builds.load()
              << ",\"strokeRasterMeanMs\":"
              << (composed ? bench_stats[i].stroke_raster_ns.load() / double(composed) / 1e6 : 0)
              << ",\"maxPresentGapMs\":" << bench_stats[i].max_gap_ns.load() / 1e6
              << ",\"gapsOver20Ms\":" << bench_stats[i].gaps_over_20ms.load()
              << ",\"gapsOver25Ms\":" << bench_stats[i].gaps_over_25ms.load()
              << ",\"groupSaves\":" << bench_stats[i].group_saves.load()
              << ",\"maxDeferredGpuFrames\":" << bench_stats[i].max_deferred_gpu_frames.load()
              << ",\"zeroCopySceneReceipts\":" << bench_stats[i].zero_copy_presentations.load()
              << ",\"pendingTurns\":" << bench_stats[i].pending_turns.load()
              << ",\"submitToPresentMeanMs\":"
              << (receipts ? bench_stats[i].present_ns.load() / double(receipts) / 1e6 : 0)
              << ",\"presentToPollMeanMs\":"
              << (receipts ? bench_stats[i].poll_ns.load() / double(receipts) / 1e6 : 0) << '}';
  }
  std::cout << "],\"syntheticNativeVideo\":" << (with_video ? "true" : "false")
            << ",\"syntheticUiPeer\":" << (with_video ? "true" : "false")
            << ",\"syntheticVideoBackdrop\":" << (with_video ? "true" : "false")
            << ",\"assetVideoNegotiationTested\":false,\"receiverSocketsTested\":false,"
               "\"nativeVideoPanels\":[";
  for (unsigned i = 0; i < 2; ++i) {
    if (i) std::cout << ',';
    const auto& s = native_stats[i];
    std::cout << "{\"submitted\":" << s.submitted << ",\"decoded\":" << s.decoded
              << ",\"presented\":" << s.presented << ",\"late\":" << s.late
              << ",\"superseded\":" << s.superseded << ",\"discarded\":" << s.discarded
              << ",\"clockDropped\":" << s.clock_dropped << ",\"maxStaged\":" << s.max_staged
              << ",\"visibleVideoTurns\":" << s.visible_draws
              << ",\"importMs\":" << s.import_ns.load() / 1e6;
#ifdef HEADUNIT_THEME_HAVE_NATIVE_VIDEO
    if (replay)
      std::cout << ",\"produced\":" << replay->sent[i]
                << ",\"backpressurePolls\":" << replay->backpressure[i];
#endif
    std::cout << '}';
  }
  std::cout << ']'
#ifdef HEADUNIT_THEME_HAVE_SKIA
            << ",\"slowFrames\":[";
  for (unsigned panel = 0; panel < 2; ++panel) {
    if (panel) std::cout << ',';
    auto& traces = slow_frames[panel];
    std::sort(traces.begin(), traces.end(),
              [](const auto& a, const auto& b) { return a.compose_ns > b.compose_ns; });
    std::cout << '[';
    for (std::size_t index = 0; index < traces.size(); ++index) {
      if (index) std::cout << ',';
      const auto& t = traces[index];
      std::cout << "{\"ordinal\":" << t.ordinal << ",\"composeMs\":" << t.compose_ns / 1e6
                << ",\"validationMs\":" << t.validation_ns / 1e6
                << ",\"preparationMs\":" << t.preparation_ns / 1e6
                << ",\"traversalMs\":" << t.traversal_ns / 1e6
                << ",\"flushMs\":" << t.flush_ns / 1e6
                << ",\"completionWaitMs\":" << t.completion_wait_ns / 1e6
                << ",\"importMs\":" << t.import_ns / 1e6
                << ",\"retainedBuilds\":" << t.retained_builds
                << ",\"strokeBuilds\":" << t.stroke_builds
                << ",\"strokeRasterMs\":" << t.stroke_raster_ns / 1e6
                << ",\"groupSaves\":" << t.group_saves << ",\"gpuCacheBytes\":" << t.gpu_cache_bytes
                << '}';
    }
    std::cout << ']';
  }
  std::cout << "],\"slowGaps\":[";
  for (unsigned panel = 0; panel < 2; ++panel) {
    if (panel) std::cout << ',';
    auto& traces = slow_gaps[panel];
    std::sort(traces.begin(), traces.end(),
              [](const auto& a, const auto& b) { return a.gap_ns > b.gap_ns; });
    std::cout << '[';
    for (std::size_t index = 0; index < traces.size(); ++index) {
      if (index) std::cout << ',';
      const auto& t = traces[index];
      std::cout << "{\"sceneOrdinal\":" << t.scene_ordinal << ",\"drawOrdinal\":" << t.draw_ordinal
                << ",\"gapMs\":" << t.gap_ns / 1e6
                << ",\"submitToPresentMs\":" << t.submit_to_present_ns / 1e6
                << ",\"presentToPollMs\":" << t.present_to_poll_ns / 1e6
                << ",\"refreshMs\":" << t.refresh_ns / 1e6
                << ",\"queuedBehind\":" << t.queued_behind << '}';
    }
    std::cout << ']';
  }
  std::cout << ']'
#endif
            << ",\"workerMaxTurnMs\":" << worker_max_ns.load() / 1e6
            << ",\"workerMissedSlots\":" << worker_missed_slots.load()
            << ",\"clusterRenderWidth\":" << cluster_render_width
            << ",\"targetRunSeconds\":" << run_for.count() << "}\n";
}
void scene_requirements(const std::shared_ptr<const graphics::Layer>& root, const char* display) {
  std::array<std::size_t, 6> kinds{};
  std::array<std::size_t, 19> blends{};
  std::array<std::size_t, 3> gradients{};
  std::size_t masks = 0, clips = 0, filters = 0, groups = 0, depth = 0, stops = 0, count = 0;
  const auto visit = [&](auto&& self, const std::shared_ptr<const graphics::Layer>& layer,
                         unsigned level, double inherited) -> void {
    if (!layer || layer->hidden || layer->opacity == 0) return;
    CHECK(level <= 64 && ++count <= 4096);
    depth = std::max(depth, std::size_t(level));
    ++kinds.at(std::size_t(layer->kind));
    ++blends.at(std::size_t(layer->blend));
    clips += layer->clip;
    for (const auto& f : layer->filters)
      filters += f && f->enabled && f->radius > 0;
    if (layer->kind == graphics::Kind::gradient) {
      ++gradients.at(std::size_t(layer->gradient_kind));
      stops = std::max(stops, layer->stops.size());
    }
    const auto alpha = inherited * layer->opacity;
    const bool group = layer->needs_group(alpha);
    groups += group;
    for (const auto& child : layer->children)
      self(self, child, level + 1, group && layer->group_blending ? 1 : alpha);
    if (layer->mask) {
      ++masks;
      self(self, layer->mask, level + 1, 1);
    }
  };
  visit(visit, root, 0, 1);
  std::size_t gradient_index = 0;
  const auto describe_masks = [&](auto&& self, const std::shared_ptr<const graphics::Layer>& layer,
                                  unsigned level) -> void {
    if (!layer || layer->hidden || layer->opacity == 0) return;
    CHECK(level <= 64);
    if (layer->kind == graphics::Kind::gradient && layer->mask) {
      const auto index = ++gradient_index;
      const auto describe = [&](auto&& walk, const std::shared_ptr<const graphics::Layer>& node,
                                unsigned depth) -> void {
        CHECK(node && depth <= 64);
        std::cout << "{\"gradientMask\":\"" << display << "\",\"index\":" << index
                  << ",\"depth\":" << depth << ",\"kind\":" << unsigned(node->kind)
                  << ",\"children\":" << node->children.size()
                  << ",\"hasMask\":" << (node->mask ? "true" : "false")
                  << ",\"opacity\":" << node->opacity
                  << ",\"backgroundAlpha\":" << node->background.a
                  << ",\"fillAlpha\":" << node->fill.a << ",\"strokeAlpha\":" << node->stroke.a
                  << ",\"lineWidth\":" << node->line_width
                  << ",\"borderWidth\":" << node->border_width
                  << ",\"segments\":" << (node->path ? node->path->segments.size() : 0)
                  << ",\"clip\":" << (node->clip ? "true" : "false") << "}\n";
        for (const auto& child : node->children)
          walk(walk, child, depth + 1);
        if (node->mask) walk(walk, node->mask, depth + 1);
      };
      describe(describe, layer->mask, 0);
    }
    for (const auto& child : layer->children)
      self(self, child, level + 1);
  };
  describe_masks(describe_masks, root, 0);
  const auto array = [](const auto& values) {
    for (std::size_t i = 0; i < values.size(); ++i)
      std::cout << (i ? "," : "") << values[i];
  };
  std::cout << "{\"sceneRequirements\":\"" << display << "\",\"kinds\":[";
  array(kinds);
  std::cout << "],\"blends\":[";
  array(blends);
  std::cout << "],\"gradients\":[";
  array(gradients);
  std::cout << "],\"masks\":" << masks << ",\"clips\":" << clips
            << ",\"enabledFilters\":" << filters << ",\"groups\":" << groups
            << ",\"maxDepth\":" << depth << ",\"maxGradientStops\":" << stops
            << ",\"simulatedStateOnly\":true}\n";
}
Packet next(script::Channel& channel, script::UiTransport& transport, const std::string& display) {
  auto& r = channel.runtime(display);
  for (int i = 0; i < 8; ++i) {
    if (const auto* pending = transport.pending()) {
      const std::array ids{std::string("aabbccdd-0000-0000-0000-000000000001")};
      const auto message = headunit::decode_ultra_ui_envelope(pending->payload, ids);
      transport.accepted();
      return decode(message.binary_plist);
    }
    r.tick();
  }
  throw std::runtime_error("asset session did not produce expected message");
}
script::Stats probe(const script::ResourceReader& asset, const std::string& display) {
  script::Channel channel(asset, {{display}});
  const std::string wire_id = "aabbccdd-0000-0000-0000-000000000001";
  script::UiTransport transport(channel, 7, {{wire_id, display}});
  transport.start();
  auto& r = channel.runtime(display);
  auto receive = [&](const Packet& packet) {
    headunit::DataMessage message;
    message.kind = headunit::DataMessageKind::asynchronous;
    message.type = 0x636f6d6d;
    message.payload = headunit::encode_ultra_ui_envelope({wire_id, encode(packet)});
    return transport.receive(7, message) == script::TransportDelivery::accepted;
  };
  r.evaluate(R"(
    session = false; received = 0; resets = 0; ready = 0
    local Session = require 'APL.ClusterControlSession'
    session = Session.new('livi-offline-peer')
    session:setDataHandler(function(bytes)
      local command = CP.parsePlist(bytes)
      assert(command.type == 17 and command.data.probe == 'livi')
      received += 1
    end)
    session:setResetHandler(function() resets += 1 end)
    session:setReadyHandler(function(id)
      assert(id == 'livi-offline-peer'); ready += 1
    end)
    session:start()
  )",
             "offline-asset-probe");
  const auto reset = next(channel, transport, display);
  CHECK(reset.type == MessageType::reset && reset.session == 1 && reset.sequence == 1);
  CHECK(reset.version == 2 && reset.peer_id == "livi-offline-peer");
  Packet peer;
  peer.type = MessageType::reset_complete;
  peer.session = reset.session;
  peer.sequence = 1;
  peer.acknowledgement = reset.sequence;
  peer.version = 2;
  peer.peer_id = "different-peer";
  CHECK(receive(peer));
  r.tick();
  r.evaluate("assert(not session:getIsReady() and ready == 0)");
  CHECK(!transport.pending());
  peer.peer_id = "livi-offline-peer";
  CHECK(receive(peer));
  const auto complete = next(channel, transport, display);
  CHECK(complete.type == MessageType::reset_complete && complete.sequence == 2);
  CHECK(complete.version == 2 && complete.peer_id == peer.peer_id);
  r.evaluate(R"(
    assert(session:getIsReady() and ready == 1)
    session:sendCommand({type=17, data={probe='livi'}})
  )");
  const auto command = next(channel, transport, display);
  CHECK(command.type == MessageType::command && command.sequence == 3 && !command.data.empty());
  peer.type = MessageType::command;
  peer.sequence = 2;
  peer.acknowledgement = command.sequence;
  peer.version.reset();
  peer.peer_id.reset();
  peer.data = command.data;
  CHECK(receive(peer));
  const auto ack = next(channel, transport, display);
  CHECK(ack.type == MessageType::ack && ack.sequence == 4 && ack.acknowledgement == 2);
  r.evaluate("assert(received == 1)");
  CHECK(receive(peer));
  r.tick();  // Duplicate must not be delivered twice.
  r.evaluate("assert(received == 1)");
  CHECK(!transport.pending());
  peer.sequence = 4;
  peer.acknowledgement = ack.sequence;
  CHECK(receive(peer));
  const auto restarted = next(channel, transport, display);
  CHECK(restarted.type == MessageType::reset && restarted.session == 2 && restarted.sequence == 1);
  r.evaluate("assert(resets == 1 and not session:getIsReady()); session:destroy()");
  r.tick();
  CHECK(r.stats().active_tasks == 0 && !r.stats().faulted);
  return r.stats();
}
void draw_probe(const script::ResourceReader& asset, const char* output) {
  script::Runtime r(asset, {"Center_Display", 640, 360});
  r.evaluate(R"(
    local Layer=require 'APL.Layer'
    local Shape=require 'APL.ShapeLayer'
    local Gradient=require 'APL.GradientLayer'
    local Path=require 'APL.Path'
    local Asset=require 'APL.Asset'
    local Text=require 'APL.TextLayer'
    local Rect=require 'APL.Rect'
    local AttributedString=require 'APL.AttributedString'
    Asset.setManifest('AssetManifestData','r/Center_Display')
    local root=Layer.new():setFrame(0,0,640,360):setBackgroundColor('#080D18FF')
    local function arc(x,y,r,finish,color,width)
      local path=Path.new():arcTo(x,y,r,math.pi*.75,math.pi*finish,true)
      return Shape.new():setPath(path):setFillColor(nil):setStrokeColor(color)
        :setLineWidth(width):setLineCap(Shape.LINECAP.ROUND)
    end
    root:addSublayer(arc(168,182,112,2.25,'#243247FF',13))
    root:addSublayer(arc(168,182,112,1.92,'#45C9FFFF',13))
    root:addSublayer(arc(466,182,94,2.25,'#243247FF',10))
    local gradient=Gradient.new():setFrame(355,72,222,222):setStartPoint(0,0):setEndPoint(1,1)
      :setColors({'#49F2BCFF','#428AFFFF'})
    gradient:setMask(arc(111,110,94,1.65,'#FFFFFFFF',10))
    root:addSublayer(gradient)
    for i=0,12 do
      local angle=math.pi*(.75+1.5*i/12)
      local path=Path.new():moveTo(168+90*math.cos(angle),182+90*math.sin(angle))
        :lineTo(168+99*math.cos(angle),182+99*math.sin(angle))
      root:addSublayer(Shape.new():setPath(path):setFillColor(nil):setStrokeColor('#70859FFF'):setLineWidth(2))
    end
    local needle=Path.new():moveTo(162,182):lineTo(218,119):lineTo(174,187):closeSubpath()
    root:addSublayer(Shape.new():setPath(needle):setFillColor('#F2F8FFFF'))
    local panel=Layer.new():setFrame(281,140,86,84):setBackgroundColor('#ABCFFF26')
      :setCornerRadius(18):setBorderWidth(1):setBorderColor('#D1E5FF40')
    root:addSublayer(panel)
    local label=Text.new():setFrame(Rect.new(86,200,164,96))
      :setAlignment(AttributedString.ALIGNMENT.CENTER):setText('72'):setForegroundColor('#F2F8FFFF')
    root:addSublayer(label)
    local second=Text.new():setFrame(Rect.new(375,155,182,100))
      :setAlignment(AttributedString.ALIGNMENT.CENTER):setText('3.2'):setForegroundColor('#49F2BCFF')
    root:addSublayer(second)
    CP.setRootLayer('Center_Display',root:getNativeLayer())
    assert(root:isSublayer(panel))
    local transform=panel:getNativeLayer():getTransformTo(root:getNativeLayer())
    local rect=CP.DPRect.withSize(panel:getNativeLayer().bounds.size):applyTransform(transform)
    assert(rect.origin.x==281 and rect.origin.y==140)
  )",
             "original-graphics-helper-probe");
  auto frame = r.render();
  CHECK(frame.pixels[0] == 0xff080d18);
  CHECK(frame.pixels[182 * 640 + 466] != 0);  // Frame has an opaque background throughout.
  std::size_t white_digits = 0, green_digits = 0;
  for (unsigned y = 220; y < 284; ++y)
    for (unsigned x = 100; x < 235; ++x) {
      const auto px = frame.pixels[y * 640 + x];
      if (((px >> 16) & 255) > 200 && ((px >> 8) & 255) > 200 && (px & 255) > 200) ++white_digits;
    }
  for (unsigned y = 170; y < 240; ++y)
    for (unsigned x = 405; x < 525; ++x) {
      const auto px = frame.pixels[y * 640 + x];
      if (((px >> 8) & 255) > 190 && ((px >> 16) & 255) < 140) ++green_digits;
    }
  CHECK(white_digits > 500 && green_digits > 500);
  graphics::write_png(frame, output);
#ifdef HEADUNIT_THEME_HAVE_WAYLAND
  if (const char* flag = std::getenv("HEADUNIT_THEME_GRAPHICS_WAYLAND");
      flag && std::string_view(flag) == "1") {
    const auto presented = graphics::present_wayland(frame, "Headunit Ultra graphics bench");
    CHECK(presented.readback.pixels.size() == frame.pixels.size());
    unsigned max_error = 0;
    for (std::size_t i = 0; i < frame.pixels.size(); ++i)
      for (unsigned shift = 0; shift < 32; shift += 8)
        max_error = std::max(
            max_error, unsigned(std::abs(int((frame.pixels[i] >> shift) & 255) -
                                         int((presented.readback.pixels[i] >> shift) & 255))));
    graphics::write_png(presented.readback, std::string(output) + ".gpu.png");
    std::cerr << "Wayland GLES renderer: " << presented.renderer << '\n';
    std::cout << "{\"waylandFrames\":" << presented.frames
              << ",\"gpuMaxChannelError\":" << max_error << "}\n";
    CHECK(max_error <= 1);
  }
#endif
  std::cout
      << "{\"drawingHelpersExecuted\":true,\"modules\":" << r.stats().modules
      << ",\"width\":640,\"height\":360,\"rasterTextTested\":true,\"fullAssetEntryPoint\":false}\n";
}
void vehicle_probe(const script::ResourceReader& asset) {
  using opack::Array;
  using opack::Dictionary;
  using opack::Value;
  auto text = [](const char* s) {
    return Value{std::string(s)};
  };
  auto number = [](std::int64_t n) {
    return Value{n};
  };
  auto dictionary = [](std::initializer_list<opack::DictionaryEntry> v) {
    return Value{Dictionary(v)};
  };
  auto array = [](std::initializer_list<Value> v) {
    return Value{Array(v)};
  };
  // Deliberately synthetic local IIDs/type IDs. This model is never advertised
  // to a phone and does not stand in for a vehicle integration or sensor input.
  auto characteristic = [&](int id) {
    return dictionary({{text("iid"), number(id)},
                       {text("type"), number(id)},
                       {text("supportsInvalid"), Value{true}}});
  };
  auto profile = dictionary(
      {{text("accessories"),
        array({dictionary(
            {{text("iid"), number(1)},
             {text("services"),
              array({dictionary({{text("iid"), number(2)},
                                 {text("characteristics"),
                                  array({characteristic(3), characteristic(4)})}})})}})})}});
  auto source = std::make_shared<vehicle::ReadOnlyService>(
      std::move(profile),
      std::vector<vehicle::SignalPolicy>{{3, vehicle::ValueKind::real, 100, 0.0, 10000.0},
                                         {4, vehicle::ValueKind::boolean, 100, {}, {}}});
  std::uint64_t now = 1000;
  source->publish(3, Value{0.0}, now, now);
  source->publish(4, Value{false}, now, now);
  script::Runtime r(asset, {"Cluster_Display", 640, 360});
  r.bind_vehicle(source,
                 {{3, {"internalCombustionEngine", "engineRPM", "rotationalSpeed"}, {}},
                  {4, {"uiControl", "uiAppearance", "nightMode"}, {}}},
                 [&] { return now; });
  r.evaluate(R"(
    vehicleUpdater=false;vehicleIds=false
    local Updater=require 'VehicleDataUpdater'
    vehicleIds=Updater.DATAIDENTIFIERS
    vehicleUpdater=Updater.new(CP.getVehicleDataConfig())
    assert(vehicleUpdater:isSubscribable(vehicleIds.engineRPM,vehicleIds.nightMode))
    local data=vehicleUpdater:getData({vehicleIds.engineRPM,vehicleIds.nightMode})
    assert(data[vehicleIds.engineRPM]==0 and data[vehicleIds.nightMode]==false)
    vehicleUpdater:start()
  )",
             "original-vehicle-updater-probe");
  r.tick();
  ++now;
  source->publish(3, Value{3200.0}, now, now);
  source->publish(4, Value{true}, now, now);
  r.tick();
  r.evaluate(R"(
    local data=vehicleUpdater:getData({vehicleIds.engineRPM,vehicleIds.nightMode})
    assert(data[vehicleIds.engineRPM]==3200 and data[vehicleIds.nightMode]==true)
  )",
             "original-vehicle-update");
  now += 100;
  r.tick();
  r.evaluate(R"(
    local data=vehicleUpdater:getData({vehicleIds.engineRPM,vehicleIds.nightMode})
    assert(data[vehicleIds.engineRPM]==CP.INVALIDVALUE and data[vehicleIds.nightMode]==CP.INVALIDVALUE)
    vehicleUpdater:destroy()
  )",
             "original-vehicle-expiration");
  r.tick();
  CHECK(!r.stats().faulted && r.stats().active_tasks == 0);
  std::cout << "{\"originalVehicleUpdater\":true,\"syntheticSignals\":2,\"typedUpdates\":true,"
               "\"expiry\":true,\"modules\":"
            << r.stats().modules << ",\"realSensors\":false,\"phoneAdvertised\":false}\n";
}
void controls_probe(const script::ResourceReader& asset) {
  using opack::Array;
  using opack::Dictionary;
  using opack::Value;
  auto text = [](const char* value) {
    return Value{std::string(value)};
  };
  auto integer = [](std::int64_t value) {
    return Value{value};
  };
  Array characteristics;
  std::vector<vehicle::SignalPolicy> policies;
  std::vector<vehicle::ScriptBinding> bindings;
  std::map<std::int64_t, Value> fixed, initial;
  std::int64_t next_id = 3;
  const auto add = [&](const char* instance, std::array<std::string, 3> path,
                       vehicle::ValueKind kind, std::optional<Value> immutable = {}) {
    const auto id = next_id++;
    characteristics.push_back(
        Value{Dictionary{{text("iid"), integer(id)}, {text("supportsInvalid"), Value{true}}}});
    policies.push_back({id, kind, 100, {}, {}});
    bindings.push_back({id, std::move(path), {}, std::string(instance)});
    if (immutable) fixed.emplace(id, std::move(*immutable));
    return id;
  };
  // Synthetic LOCAL controls exercise the unmodified asset's actual updater.
  // No phone-facing type IDs, remote requests, sensor readings or actuation.
  const auto select =
      add("select", {"uiControl", "uiInputDeviceButton", "uiInputDeviceButtonEvent"},
          vehicle::ValueKind::integer);
  add("select", {"uiControl", "uiInputDeviceButton", "uiInputDevicePurpose"},
      vehicle::ValueKind::integer, integer(0));
  const auto back = add("back", {"uiControl", "uiInputDeviceButton", "uiInputDeviceButtonEvent"},
                        vehicle::ValueKind::integer);
  add("back", {"uiControl", "uiInputDeviceButton", "uiInputDevicePurpose"},
      vehicle::ValueKind::integer, integer(1));
  initial.emplace(select, integer(0));
  initial.emplace(back, integer(0));
  std::array<std::int64_t, 2> on_ids{};
  for (std::size_t i = 0; i < 2; ++i) {
    const auto* zone = i ? "zone2" : "zone1";
    const auto url = add(zone, {"automakerRequestContent", "requestContent", "contentURL"},
                         vehicle::ValueKind::string);
    on_ids[i] =
        add(zone, {"automakerRequestContent", "requestContent", "on"}, vehicle::ValueKind::boolean);
    add(zone, {"automakerRequestContent", "requestContent", "userDismissible"},
        vehicle::ValueKind::boolean, Value{false});
    add(zone, {"automakerRequestContent", "requestContent", "displayPanelIdentifier"},
        vehicle::ValueKind::string, text("Cluster_Display"));
    add(zone, {"automakerRequestContent", "requestContent", "displayZoneIdentifier"},
        vehicle::ValueKind::string, text(zone));
    initial.emplace(url, text(i ? "livi-test://two" : "livi-test://one"));
    initial.emplace(on_ids[i], Value{false});
  }
  auto service = Value{Dictionary{{text("iid"), integer(2)},
                                  {text("characteristics"), Value{std::move(characteristics)}}}};
  auto accessory = Value{
      Dictionary{{text("iid"), integer(1)}, {text("services"), Value{Array{std::move(service)}}}}};
  auto source = std::make_shared<vehicle::ReadOnlyService>(
      Value{Dictionary{{text("accessories"), Value{Array{std::move(accessory)}}}}},
      std::move(policies), std::move(fixed));
  std::uint64_t now = 1000;
  for (const auto& [id, value] : initial)
    source->publish(id, value, now, now);
  script::Runtime runtime(asset, {"Cluster_Display", 640, 360});
  runtime.bind_vehicle(source, std::move(bindings), [&] { return now; });
  runtime.evaluate(R"(
    controlIds=false;controlUpdater=false;controlDispatcher=false;contentControllers=false;pressEvents=false
    local Updater=require 'VehicleDataUpdater'
    local Dispatcher=require 'InputEventDispatcher'
    local Controller=require 'RequestContentController'
    controlIds=Updater.DATAIDENTIFIERS
    local config=CP.getVehicleDataConfig()
    assert(#config.uiControl.uiInputDeviceButton==2)
    assert(#config.automakerRequestContent.requestContent==2)
    controlUpdater=Updater.new(config)
    assert(controlUpdater:isSubscribable(controlIds.buttonInput,controlIds.requestContent))
    pressEvents={}
    controlDispatcher=Dispatcher.new(controlUpdater,function(events)
      for _, event in ipairs(events) do
        pressEvents[#pressEvents+1]={kind=event:getType(),phase=event:getPhase()}
      end
    end)
    contentControllers={Controller.new('zone1','Cluster_Display'),Controller.new('zone2','Cluster_Display')}
    for _, controller in ipairs(contentControllers) do assert(controller:setVehicleDataUpdater(controlUpdater)) end
    controlUpdater:start()
  )",
                   "original-repeated-controls");
  runtime.tick();
  runtime.evaluate(R"(
    assert(#pressEvents==0)
    local states=controlUpdater:getData({controlIds.requestContent})[controlIds.requestContent].Cluster_Display
    assert(states.zone1.contentURL=='livi-test://one' and states.zone1.on==false)
    assert(states.zone2.contentURL=='livi-test://two' and states.zone2.on==false)
    for _, controller in ipairs(contentControllers) do assert(controller:isZoneSupported()) end
  )",
                   "original-control-initial-state");
  ++now;
  source->publish(select, integer(1), now, now);
  source->publish(on_ids[0], Value{true}, now, now);
  runtime.tick();
  runtime.evaluate(R"(
    assert(#pressEvents==1 and pressEvents[1].kind==0 and pressEvents[1].phase==0)
    local states=controlUpdater:getData({controlIds.requestContent})[controlIds.requestContent].Cluster_Display
    assert(states.zone1.on==true and states.zone2.on==false)
  )",
                   "original-select-and-zone-update");
  ++now;
  source->publish(back, integer(1), now, now);
  runtime.tick();
  runtime.evaluate(
      "assert(#pressEvents==2 and pressEvents[2].kind==1 and pressEvents[2].phase==0)");
  ++now;
  source->publish(select, integer(0), now, now);
  runtime.tick();
  runtime.evaluate(
      "assert(#pressEvents==3 and pressEvents[3].kind==0 and pressEvents[3].phase==1)");
  // Expired button data must not remain pressed. This is a local simulation;
  // production policy still needs a real button/sensor provider and its clock.
  now += 100;
  runtime.tick();
  runtime.evaluate(R"(
    assert(#pressEvents==4 and pressEvents[4].kind==1 and pressEvents[4].phase==1)
    controlDispatcher:destroy()
    for _, controller in ipairs(contentControllers) do controller:destroy() end
    controlUpdater:destroy()
  )",
                   "original-button-expiration");
  runtime.tick();
  CHECK(!runtime.stats().faulted && runtime.stats().active_tasks == 0);
  std::cout << "{\"originalControlScripts\":true,\"buttons\":2,\"contentZones\":2,"
               "\"independentUpdates\":true,\"buttonExpiry\":true,\"syntheticLocalInputs\":true,"
               "\"phoneAdvertised\":false,\"realSensors\":false}\n";
}
std::optional<std::string> probe_palette(const script::ResourceReader& asset) {
  const auto* requested = std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_PALETTE");
  if (!requested) return {};
  const std::string palette = requested;
  if (palette.empty() || palette.size() > 64 ||
      std::any_of(palette.begin(), palette.end(), [](unsigned char c) {
        return !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
               c != '_' && c != '-';
      }))
    throw std::invalid_argument("invalid offline palette identifier");
  const auto* selected = std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT");
  if (!selected) throw std::invalid_argument("offline palette selection requires a cluster layout");
  const std::string layout = selected;
  script::Runtime metadata(asset, {"Cluster_Display"});
  metadata.select_startup_layout(layout);  // Validate UUID syntax before interpolation.
  // Read immutable signed metadata in a bounded VM. Reject a missing palette
  // or layout before any entry-point/handshake probe runs. This executes data
  // modules only, not the display controller or a phone session.
  metadata.evaluate(
      "local palette=require('r.ThemeData').palettes['" + palette +
          "']; "
          "assert(type(palette)=='table' and palette.id=='" +
          palette +
          "', 'unknown offline palette'); "
          "local found=false; for _,display in ipairs(require('r.PackageData').displays) do "
          "if display.id=='Cluster_Display' then for _,layout in ipairs(display.layouts) do "
          "if layout.id=='" +
          layout +
          "' and layout.layoutType=='primary' then found=true end end end end; "
          "assert(found, 'unknown offline palette layout')",
      "offline-palette-metadata");
  return palette;
}
// Explicit offline dual-VM fixture. CPU uptime is the theme's normal animation
// clock; all PTS/foreground/visibility inputs here are simulated, never receipts.
void greeting_probe(const script::ResourceReader& asset, const char* local_profile,
                    const char* output) {
  auto profile = vehicle::decode_local_profile(read_root(local_profile));
  CHECK(profile.scene_inputs);
  auto launch = script::SceneLifecycle::Launch::startup;
  if (const auto* mode = std::getenv("HEADUNIT_THEME_PROBE_GREETING")) {
    const std::string_view name(mode);
    if (name == "first-pairing")
      launch = script::SceneLifecycle::Launch::first_pairing;
    else if (name == "resume")
      launch = script::SceneLifecycle::Launch::resume;
    else
      CHECK(name == "startup");
  }
  script::Limits limits;
  limits.startup_ms = 10000;
  const bool production_budget = std::getenv("HEADUNIT_THEME_PROBE_GREETING_PRODUCTION") != nullptr;
  const bool delayed_foreground =
      std::getenv("HEADUNIT_THEME_PROBE_GREETING_DELAY_FOREGROUND") != nullptr;
  limits.frame_ms = production_budget ? 20 : 1000;
  unsigned slow_reads = 0;
  const auto measured_reader = [&](std::string_view path) {
    const auto began = std::chrono::steady_clock::now();
    auto bytes = asset(path);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();
    if (production_budget && elapsed >= 1000 && slow_reads++ < 32)
      std::clog << "Greeting resource path=" << path << " readWallUs=" << elapsed << '\n';
    return bytes;
  };
  script::Channel channel(measured_reader,
                          {{"Center_Display", 1920, 720}, {"Cluster_Display", 1920, 720}}, limits);
  auto& cluster = channel.runtime("Cluster_Display");
  cluster.enable_scene_observation();
  script::SceneLifecycle scene(profile, launch);
  std::uint64_t now = 1000;
  scene.pump(now, script::Runtime::ScenePhase::unavailable, false);
  for (const auto* panel : {"Center_Display", "Cluster_Display"}) {
    auto& runtime = channel.runtime(panel);
    runtime.bind_vehicle(profile.source, profile.bindings, [&] { return now; });
    if (std::string_view(panel) == "Center_Display") runtime.enable_scene_observation();
    (void)runtime.add_video_input({"iPhoneVideo", 1920, 720});
    runtime.bind_verification_reporter([](const auto&) {});
    runtime.tick(0.0);
    runtime.evaluate("require 'main'", "offline-greeting");
    runtime.set_foregrounded(!delayed_foreground);
  }
  for (const auto* panel : {"Center_Display", "Cluster_Display"})
    channel.runtime(panel).start_tasks(0.0);
  const auto began = std::chrono::steady_clock::now();
  bool logos = false, captured = false;
  const bool brand = std::getenv("HEADUNIT_THEME_PROBE_BRAND_GREETING") != nullptr;
  graphics::BrandGreeting decoration;
  std::shared_ptr<const graphics::Layer> decoration_scene;
#ifdef HEADUNIT_THEME_HAVE_SKIA
  std::unique_ptr<graphics::GpuCompositor> capture_gpu;
#endif
  const auto capture = [&](script::Runtime& runtime, const std::string& suffix) {
    auto root = std::shared_ptr<const graphics::Layer>(runtime.scene_root());
    const auto original = root;
    if (&runtime == &cluster && decoration_scene) root = decoration_scene;
    const bool compare_apple = brand && &runtime == &cluster && suffix == "-logos";
    const auto check_apple = [&](const graphics::Image& before, const graphics::Image& after) {
      unsigned changed = 0;
      for (unsigned y = 0; y < 720; ++y)
        for (unsigned x = 0; x < 1920; ++x) {
          const auto index = std::size_t(y) * 1920 + x;
          if (before.pixels[index] == after.pixels[index]) continue;
          CHECK(x < 960);
          ++changed;
        }
      CHECK(changed > 1000);
      std::cout << "{\"coBrandApplePixelsUnchanged\":true,\"manufacturerChanged\":true}\n";
    };
#ifdef HEADUNIT_THEME_HAVE_SKIA
    if (const auto* node = std::getenv("HEADUNIT_THEME_PROBE_GPU_ONLY")) {
      if (!capture_gpu) capture_gpu = std::make_unique<graphics::GpuCompositor>(1920, 720, node);
      graphics::Image before;
      if (compare_apple) {
        (void)capture_gpu->compose_scene(original, 2000000000);
        before = capture_gpu->readback();
      }
      (void)capture_gpu->compose_scene(root, 2000000000);
      const auto after = capture_gpu->readback();
      if (compare_apple) check_apple(before, after);
      graphics::write_png(after, std::string(output) + suffix + ".png");
      return;
    }
#endif
    const auto after = graphics::render(std::const_pointer_cast<graphics::Layer>(root), 1920, 720);
    if (compare_apple)
      check_apple(graphics::render(std::const_pointer_cast<graphics::Layer>(original), 1920, 720),
                  after);
    graphics::write_png(after, std::string(output) + suffix + ".png");
  };
  // Explicit offline export only. Retain actual sampling times; PNG readback
  // does not establish live refresh rate or phone/compositor presentation.
  std::ofstream frame_times;
  if (std::getenv("HEADUNIT_THEME_PROBE_GREETING_FRAMES")) {
    frame_times.open(std::string(output) + "-frame-times.txt");
    if (!frame_times) throw std::runtime_error("cannot open greeting frame receipts");
  }
  // Let the original signed animator complete; never dispatch its finish action.
  for (unsigned frame = 0; frame < 240; ++frame) {
    std::this_thread::sleep_until(began + std::chrono::milliseconds(frame * 1000 / 30));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();
    now = 1001 + std::uint64_t(elapsed);
    if (delayed_foreground && frame == 2)
      for (const auto* panel : {"Center_Display", "Cluster_Display"})
        channel.runtime(panel).set_foregrounded(true);
    const auto phase = cluster.scene_phase();
    logos = logos || phase == script::Runtime::ScenePhase::logos;
    // Foreground/visibility and PTS here are fixture inputs, never live receipts.
    scene.pump(now, phase, !delayed_foreground || frame >= 2);
    for (const auto* panel : {"Center_Display", "Cluster_Display"}) {
      const auto before = std::chrono::steady_clock::now();
      channel.runtime(panel).tick(double(elapsed) / 1000);
      const auto task_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - before)
                               .count();
      if (production_budget && task_us >= 5000)
        std::clog << "Greeting task panel=" << panel << " frame=" << frame << " wallUs=" << task_us
                  << '\n';
    }
    if (brand)
      decoration_scene = decoration.scene(
          cluster.scene_root(), cluster.scene_phase() == script::Runtime::ScenePhase::logos,
          now * 1000000);
    if (logos && !captured && elapsed >= 3000 &&
        cluster.scene_phase() == script::Runtime::ScenePhase::logos) {
      CHECK(!brand || decoration_scene != cluster.scene_root());
      if (std::getenv("HEADUNIT_THEME_PROBE_GREETING_AUDIT")) {
        unsigned count = 0;
        const auto walk = [&](auto&& self, const std::shared_ptr<const graphics::Layer>& node,
                              unsigned depth) -> void {
          if (!node || count++ >= 256 || depth > 32) return;
          std::clog << "Greeting layer depth=" << depth << " name=" << node->name
                    << " kind=" << int(node->kind) << " opacity=" << node->opacity
                    << " position=" << node->position.x << ',' << node->position.y
                    << " bounds=" << node->bounds.size.width << ',' << node->bounds.size.height
                    << " transform=" << node->transform.a << ',' << node->transform.d << ','
                    << node->transform.tx << ',' << node->transform.ty << '\n';
          for (const auto& child : node->children)
            self(self, child, depth + 1);
        };
        walk(walk, cluster.scene_root(), 0);
      }
      capture(cluster, "-logos");
      captured = true;
    }
    if (frame_times.is_open() && frame < 180) {
      const auto sampled = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - began)
                               .count();
      capture(cluster, "-frame-" + std::to_string(frame));
      frame_times << frame << ' ' << sampled << '\n';
      if (!frame_times) throw std::runtime_error("cannot write greeting frame receipts");
    }
  }
  CHECK(scene.driving());
  CHECK(!brand || decoration_scene == cluster.scene_root());
  CHECK(cluster.scene_phase() == script::Runtime::ScenePhase::drive);
  CHECK(channel.runtime("Center_Display").scene_phase() == script::Runtime::ScenePhase::drive);
  CHECK(launch == script::SceneLifecycle::Launch::resume ? !logos : logos && captured);
  for (const auto* panel : {"Center_Display", "Cluster_Display"})
    capture(channel.runtime(panel), std::string("-") + panel + "-drive");
  scene.retire(++now);
  const auto invalid = profile.source->snapshot(
      std::array{profile.scene_inputs->state, profile.scene_inputs->options}, now);
  CHECK(
      std::holds_alternative<std::monostate>(invalid.at(profile.scene_inputs->state).value->data));
  CHECK(std::holds_alternative<std::monostate>(
      invalid.at(profile.scene_inputs->options).value->data));
  std::cout << "{\"greetingTested\":true,\"frameBudgetMs\":" << limits.frame_ms
            << ",\"brandDecoration\":" << (brand ? "true" : "false")
            << ",\"delayedForeground\":" << (delayed_foreground ? "true" : "false")
            << ",\"logosObserved\":" << (logos ? "true" : "false")
            << ",\"retiredUnavailable\":true,\"displays\":2,\"livePhoneTested\":false}\n";
}
void layout_probe(const script::ResourceReader& asset, const char* output,
                  const char* local_profile = nullptr,
                  const std::optional<std::string>& palette = {}) {
  using opack::Array;
  using opack::Dictionary;
  using opack::Value;
  auto text = [](const char* s) {
    return Value{std::string(s)};
  };
  auto number = [](std::int64_t n) {
    return Value{n};
  };
  struct Sample {
    std::array<std::string, 3> path;
    Value value;
    std::optional<std::int64_t> unit;
  };
  // An isolated local simulation, not a vehicle profile. The only purpose of
  // these fixture readings is to exercise the original asset's whole layout.
  // Synthetic type/IID numbers never leave this process or enter the live app.
  const std::vector<Sample> samples{
      {{"vehicleMotion", "displayedSpeed", "speed"}, Value{72000.0}, 1034},
      {{"vehicleMotion", "displayedSpeed", "speedKMH"}, Value{72.0}, 1032},
      {{"vehicleMotion", "displayedSpeed", "speedMPH"}, Value{44.7387}, 1033},
      {{"vehicleMotion", "displayedSpeed", "speedMaxKMH"}, Value{260.0}, 1032},
      {{"vehicleMotion", "displayedSpeed", "speedMaxMPH"}, Value{160.0}, 1033},
      {{"vehicleUnits", "displayUnits", "speedUnit"}, number(1032), {}},
      {{"internalCombustionEngine", "engineRPM", "rotationalSpeed"}, Value{3200.0}, {}},
      {{"internalCombustionEngine", "engineRPM", "rotationalSpeedMax"}, Value{8000.0}, {}},
      {{"internalCombustionEngine", "engineRPM", "rotationalSpeedMarkerRedline"},
       Value{6500.0},
       {}},
      {{"internalCombustionEngine", "engineRPM", "rotationalSpeedState"}, number(0), {}},
      {{"driverAssistance", "targetSpeed", "speedKMH"}, Value{80.0}, 1032},
      {{"driverAssistance", "targetSpeed", "speedMPH"}, Value{49.7097}, 1033},
      {{"driverAssistance", "targetSpeed", "targetSpeedState"}, number(0), {}},
      {{"driveState", "transmissionStatus", "transmissionMode"}, number(68), {}},
      {{"driveState", "transmissionStatus", "gearPosition"}, number(3), {}},
      {{"uiControl", "uiState", "uiSceneState"}, number(2), {}},
      {{"uiControl", "uiAppearance", "nightMode"}, Value{false}, {}}};
  Array characteristics;
  std::vector<vehicle::SignalPolicy> policies;
  std::vector<vehicle::ScriptBinding> bindings;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const auto id = std::int64_t(i + 3);
    const auto& sample = samples[i];
    Dictionary metadata{{text("iid"), number(id)},
                        {text("type"), number(id)},
                        {text("supportsInvalid"), Value{true}}};
    if (sample.unit) metadata.emplace_back(text("units"), number(*sample.unit));
    characteristics.push_back(Value{std::move(metadata)});
    bindings.push_back({id, sample.path, {}});
    const auto kind = std::holds_alternative<double>(sample.value.data) ? vehicle::ValueKind::real
                      : std::holds_alternative<bool>(sample.value.data)
                          ? vehicle::ValueKind::boolean
                          : vehicle::ValueKind::integer;
    policies.push_back({id, kind, 10000, {}, {}});
  }
  Value service{Dictionary{{text("iid"), number(2)},
                           {text("characteristics"), Value{std::move(characteristics)}}}};
  Value accessory{
      Dictionary{{text("iid"), number(1)}, {text("services"), Value{Array{std::move(service)}}}}};
  auto source = std::make_shared<vehicle::ReadOnlyService>(
      Value{Dictionary{{text("accessories"), Value{Array{std::move(accessory)}}}}},
      std::move(policies));
  std::uint64_t now = 1000;
  std::optional<vehicle::LocalProfile> decoded_profile;
  if (local_profile) {
    auto profile = vehicle::decode_local_profile(read_root(local_profile));
    source = profile.source;
    bindings = profile.bindings;
    decoded_profile = std::move(profile);
  } else
    for (std::size_t i = 0; i < samples.size(); ++i)
      source->publish(i + 3, samples[i].value, now, now);
  const bool synthetic_gauges = std::getenv("HEADUNIT_THEME_PROBE_GAUGES") != nullptr;
  CHECK(!synthetic_gauges || local_profile);
  const bool animated_gauges = std::getenv("HEADUNIT_THEME_PROBE_ANIMATED") != nullptr;
  CHECK(!animated_gauges || synthetic_gauges);
  const auto* transmission_fixture = std::getenv("HEADUNIT_THEME_PROBE_TRANSMISSION");
  CHECK(!transmission_fixture || synthetic_gauges);
  std::optional<headunit::TransmissionState> fixture_transmission;
  if (transmission_fixture && std::string_view(transmission_fixture) != "unavailable") {
    fixture_transmission = headunit::parse_transmission_state_key(transmission_fixture);
    CHECK(fixture_transmission);
  }
  const auto* mode_catalog_fixture = std::getenv("HEADUNIT_THEME_PROBE_DRIVE_MODES");
  const auto* mode_fixture = std::getenv("HEADUNIT_THEME_PROBE_DRIVE_MODE");
  CHECK(!mode_catalog_fixture || synthetic_gauges);
  CHECK(!mode_fixture || mode_catalog_fixture);
  std::optional<headunit::DriveModeCatalog> fixture_modes;
  std::optional<headunit::DriveModeIdentifier> fixture_mode;
  if (mode_catalog_fixture) {
    fixture_modes = headunit::parse_drive_mode_catalog_key(mode_catalog_fixture);
    CHECK(fixture_modes);
    if (mode_fixture && std::string_view(mode_fixture) != "unavailable") {
      fixture_mode = headunit::parse_drive_mode_identifier(mode_fixture);
      CHECK(fixture_mode && headunit::contains_drive_mode(*fixture_modes, *fixture_mode));
    }
  }
  const auto* trip_fixture = std::getenv("HEADUNIT_THEME_PROBE_TRIP");
  const auto* heading_fixture = std::getenv("HEADUNIT_THEME_PROBE_HEADING");
  CHECK((!trip_fixture && !heading_fixture) || synthetic_gauges);
  std::optional<headunit::TripState> fixture_trip;
  std::optional<headunit::HeadingState> fixture_heading;
  if (trip_fixture && std::string_view(trip_fixture) != "unavailable") {
    fixture_trip = headunit::parse_trip_state_key(trip_fixture);
    CHECK(fixture_trip);
  }
  if (heading_fixture && std::string_view(heading_fixture) != "unavailable") {
    fixture_heading = headunit::parse_heading_state_key(heading_fixture);
    CHECK(fixture_heading);
  }
  std::array<std::int64_t, 5> gauge_ids{};
  if (synthetic_gauges) {
    const std::array<std::array<std::string, 3>, 5> paths{
        {{"vehicleMotion", "displayedSpeed", "speed"},
         {"vehicleMotion", "displayedSpeed", "speedKMH"},
         {"vehicleMotion", "displayedSpeed", "speedMPH"},
         {"internalCombustionEngine", "engineRPM", "rotationalSpeed"},
         {"internalCombustionEngine", "engineRPM", "rotationalSpeedState"}}};
    for (std::size_t i = 0; i < paths.size(); ++i) {
      unsigned matches = 0;
      for (const auto& binding : bindings)
        if (binding.path == paths[i]) {
          gauge_ids[i] = binding.iid;
          ++matches;
        }
      CHECK(matches == 1 && source->timed(gauge_ids[i]));
    }
  }
  // Exercise the same profile/source bridge used by the live host, including
  // optional fuel/temperature bindings. All values here are explicit fixtures.
  class FixtureVehicle final : public headunit::VehicleSource {
    bool animated_;
    std::optional<headunit::TransmissionState> transmission_;
    std::optional<headunit::DriveModeCatalog> modes_;
    std::optional<headunit::DriveModeIdentifier> mode_;
    std::optional<headunit::TripState> trip_;
    std::optional<headunit::HeadingState> heading_;

   public:
    FixtureVehicle(bool animated, std::optional<headunit::TransmissionState> transmission,
                   std::optional<headunit::DriveModeCatalog> modes,
                   std::optional<headunit::DriveModeIdentifier> mode,
                   std::optional<headunit::TripState> trip,
                   std::optional<headunit::HeadingState> heading)
        : animated_(animated),
          transmission_(transmission),
          modes_(modes),
          mode_(mode),
          trip_(trip),
          heading_(heading) {}
    headunit::VehicleSample sample(headunit::Clock::time_point at) override {
      const auto ms = std::chrono::duration<double, std::milli>(at.time_since_epoch()).count();
      headunit::VehicleSample value;
      value.sampled_at = at;
      value.simulated = true;
      value.speed_kmh = animated_ ? 72 + 20 * std::sin(ms / 1000.) : 72;
      value.engine_rpm = animated_ ? 3200 + 1000 * std::sin(ms / 700.) : 3200;
      value.fuel_percent = 62;
      value.coolant_c = 92;
      value.ambient_c = 21;
      value.range_km = 340;
      value.odometer_km = 1250.2;
      value.transmission = transmission_;
      value.drive_mode_catalog = modes_;
      value.drive_mode = mode_;
      value.trip = trip_;
      value.heading = heading_;
      return value;
    }
  };
  std::unique_ptr<vehicle::GaugeInput> gauge_input;
  if (synthetic_gauges)
    gauge_input = std::make_unique<vehicle::GaugeInput>(
        *decoded_profile,
        std::make_unique<FixtureVehicle>(animated_gauges, fixture_transmission, fixture_modes,
                                         fixture_mode, fixture_trip, fixture_heading));
  const auto publish_gauges = [&] {
    if (gauge_input) gauge_input->pump(headunit::Clock::time_point(std::chrono::milliseconds(now)));
  };
  for (const auto* display : {"Center_Display", "Cluster_Display"}) {
    if (synthetic_gauges) now += 16;  // Keep samples strictly ordered across displays.
    script::Limits limits;
    limits.startup_ms = 10000;
    limits.frame_ms = 1000;
    // Explicit offline diagnostic budgets, including ASan overhead. Report
    // measured tick cost; do not silently weaken the live runtime's 20 ms limit.
    script::Runtime r(asset, {display, 1920, 720}, limits);
    if (std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_OPTIONS") &&
        std::string_view(display) == "Cluster_Display")
      r.enable_live_layout_selection();
    // Exercise an alternate layout from the installed signed pack without
    // changing its contents or the live phone-facing configuration.
    if (std::string_view(display) == "Cluster_Display") {
      if (const auto* layout = std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT")) {
        r.select_remote_map_component();
        r.select_startup_layout(layout);
      }
    }
    r.bind_vehicle(source, bindings, [&] { return now; });
    publish_gauges();
    std::size_t reports = 0, regions = 0;
    r.bind_verification_reporter([&](const script::VerificationReport& report) {
      CHECK(report.display_identifier == display);
      ++reports;
      regions = report.regions.size();
      // This probe records declarations only. No monitor has verified pixels.
    });
    r.tick(0.0);  // Explicit simulated timeline, never used by the live service.
    if (palette && std::string_view(display) == "Cluster_Display") {
      const std::string layout = std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_LAYOUT");
      // Receiver-owned diagnostic startup configuration, not an edit to the
      // signed bundle or an unacknowledged live UI-Sync palette transition.
      r.evaluate("require('APL.Config').applyOverrides({STARTUP={LAYOUT={ID='" + layout +
                     "',PALETTE_ID='" + *palette +
                     "'}}}, false); "
                     // The reference controller reads its saved selections before applying
                     // a startup override. Seed that normal local-state path first so its
                     // initial layout objects receive the requested diagnostic palette.
                     "require('State').setPaletteSelections({['" +
                     layout + "']='" + *palette + "'})",
                 "offline-palette-startup");
    }
    const auto began = std::chrono::steady_clock::now();
    try {
      r.evaluate("require 'main'", "isolated-layout-simulation");
    } catch (...) {
      std::cerr << "layoutSimulation=" << display
                << " startupFailed=true modules=" << r.stats().modules
                << " memoryBytes=" << r.stats().memory_bytes << '\n';
      throw;
    }
    const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - began)
                                .count();
    std::cerr << "layoutSimulation=" << display
              << " entryPointReturned=true modules=" << r.stats().modules
              << " startupMs=" << startup_ms << '\n';
    r.set_foregrounded(true);  // Explicit simulated foreground, not a compositor assertion.
    std::int64_t max_tick_us = 0;
    const bool realtime = std::getenv("HEADUNIT_THEME_PROBE_REALTIME") != nullptr;
    auto next_frame = std::chrono::steady_clock::now();
    auto tick = [&](unsigned frame) {
      if (realtime) {
        next_frame += std::chrono::nanoseconds(16666667);
        std::this_thread::sleep_until(next_frame);
      }
      now += 16;
      publish_gauges();
      const auto start = std::chrono::steady_clock::now();
      r.tick(double(frame) / 60);
      max_tick_us =
          std::max(max_tick_us, std::int64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - start)
                                                 .count()));
    };
    for (unsigned frame = 1; frame <= 60; ++frame)
      tick(frame);
    if (std::getenv("HEADUNIT_THEME_PROBE_CLUSTER_OPTIONS") &&
        std::string_view(display) == "Cluster_Display") {
      const auto options = r.cluster_selection();
      std::cerr << "clusterOptions=layouts:" << options.choices.size()
                << ";palettes:" << options.palettes.size()
                << ";contents:" << options.contents.size()
                << ";activePalette:" << options.active_palette << '\n';
      CHECK(!options.choices.empty() && !options.palettes.empty());
      for (const auto& choice : options.contents)
        std::cerr << "clusterContent=" << choice.name << ";active:" << choice.active << '\n';
    }
    if (std::getenv("HEADUNIT_THEME_PROBE_SPEED_LABEL") &&
        std::string_view(display) == "Cluster_Display") {
      r.bind_log_sink([](int, std::string_view tag, std::string_view message) {
        if (tag == "speedBoundary") std::cerr << tag << '=' << message << '\n';
      });
      r.evaluate(R"(
        local Number=require 'APL.StableNumberLayer'
        local Font=require 'APL.RasterFont'
        local Rect=require 'APL.Rect'
        local Text=require 'APL.AttributedString'
        local font=Font.new('Stable-SoftNumeric-Medium-170')
        local layer=Number.new(font,'speed-boundary-diagnostic'):setFrame(Rect.new(0,0,400,170))
        layer:setAlignment(Text.ALIGNMENT.LEFT)
        for _, value in ipairs({19,19.6,20,20.4,21,20,19.6,19,20}) do
          layer:setNumber(value)
          local bounds=layer:getBoundingRect()
          local actual=layer:getAttributedString():getBoundingRect()
          CP.log(2,'speedBoundary',tostring(value)..': reserved='..tostring(bounds._width)..', actual='..tostring(actual._width))
          assert(bounds._width>=actual._width, 'stable number exceeds reserved width')
        end
        layer:destroy()
      )",
                 "offline-speed-boundary");
    }
    scene_requirements(r.scene_root(), display);
    if (std::getenv("HEADUNIT_THEME_PROBE_SCENE_AUDIT")) {
      const auto audit = [&](auto&& self, const std::shared_ptr<const graphics::Layer>& layer,
                             unsigned depth) -> void {
        if (!layer) return;
        const auto frame = layer->frame();
        std::cerr << "sceneAudit=" << display << ";depth=" << depth
                  << ";name=" << std::quoted(layer->name) << ";kind=" << unsigned(layer->kind)
                  << ";hidden=" << layer->hidden << ";opacity=" << layer->opacity
                  << ";frame=" << frame.origin.x << ',' << frame.origin.y << ',' << frame.size.width
                  << ',' << frame.size.height << ";bounds=" << layer->bounds.origin.x << ','
                  << layer->bounds.origin.y << ',' << layer->bounds.size.width << ','
                  << layer->bounds.size.height << ";blend=" << unsigned(layer->blend)
                  << ";groupBlending=" << layer->group_blending << ";clip=" << layer->clip
                  << ";masked=" << bool(layer->mask) << ";fill=" << layer->fill.r << ','
                  << layer->fill.g << ',' << layer->fill.b << ',' << layer->fill.a
                  << ";stroke=" << layer->stroke.r << ',' << layer->stroke.g << ','
                  << layer->stroke.b << ',' << layer->stroke.a << ";tint=" << layer->tint.r << ','
                  << layer->tint.g << ',' << layer->tint.b << ',' << layer->tint.a
                  << ";background=" << layer->background.r << ',' << layer->background.g << ','
                  << layer->background.b << ',' << layer->background.a
                  << ";lineCap=" << unsigned(layer->cap) << ";lineJoin=" << unsigned(layer->join)
                  << ";lineWidth=" << layer->line_width << ";trim=" << layer->stroke_start << ','
                  << layer->stroke_end << '\n';
        if (layer->path)
          for (const auto& segment : layer->path->segments)
            std::cerr << "pathAudit=" << std::quoted(layer->name)
                      << ";type=" << unsigned(segment.type) << ";point=" << segment.p.x << ','
                      << segment.p.y << ";radius=" << segment.radius << ";angles=" << segment.start
                      << ',' << segment.end << ";clockwise=" << segment.clockwise << '\n';
        if (layer->mask) {
          std::cerr << "maskOf=" << std::quoted(layer->name) << '\n';
          self(self, layer->mask, depth + 1);
        }
        for (const auto& child : layer->children)
          self(self, child, depth + 1);
      };
      audit(audit, r.scene_root(), 0);
    }
    if (std::getenv("HEADUNIT_THEME_PROBE_MASK_AUDIT")) {
      const auto walk = [&](auto&& self, const std::shared_ptr<const graphics::Layer>& node,
                            std::string path) -> void {
        if (!node) return;
        if (node->mask) {
          const auto& mask = *node->mask;
          std::cerr << "maskAudit=" << display << ";path=" << path
                    << ";kind=" << unsigned(node->kind) << ";maskKind=" << unsigned(mask.kind)
                    << ";hidden=" << node->hidden << ";maskHidden=" << mask.hidden
                    << ";childHidden=" << (node->children.size() == 1 && node->children[0]->hidden)
                    << ";children=" << node->children.size()
                    << ";backgroundAlpha=" << node->background.a << ";childKind="
                    << (node->children.size() == 1 ? int(node->children[0]->kind) : -1)
                    << ";childChildren="
                    << (node->children.size() == 1 ? node->children[0]->children.size() : 0)
                    << ";childMask="
                    << (node->children.size() == 1 && bool(node->children[0]->mask))
                    << ";childBackgroundAlpha="
                    << (node->children.size() == 1 ? node->children[0]->background.a : 0)
                    << ";childBorderAlpha="
                    << (node->children.size() == 1 ? node->children[0]->border.a : 0)
                    << ";childClip=" << (node->children.size() == 1 && node->children[0]->clip)
                    << ";childGroupBlending="
                    << (node->children.size() == 1 && node->children[0]->group_blending)
                    << ";opacity=" << node->opacity << ";blend=" << unsigned(node->blend)
                    << ";groupBlending=" << node->group_blending << ";maskFillAlpha=" << mask.fill.a
                    << ";maskClip=" << mask.clip << ";maskBackgroundAlpha=" << mask.background.a
                    << ";maskChildren=" << mask.children.size()
                    << ";maskPathSegments=" << (mask.path ? mask.path->segments.size() : 0)
                    << ";maskImage=" << bool(mask.image)
                    << ";maskStroke=" << (mask.line_width > 0 && mask.stroke.a > 0)
                    << ";width=" << node->bounds.size.width
                    << ";height=" << node->bounds.size.height << '\n';
        }
        for (std::size_t index = 0; index < node->children.size(); ++index)
          self(self, node->children[index], path + "/" + std::to_string(index));
        if (node->mask) self(self, node->mask, path + "/mask");
      };
      walk(walk, r.scene_root(), "root");
    }
    if (std::getenv("HEADUNIT_THEME_PROBE_BACKDROP_AUDIT")) {
      const auto walk = [&](auto&& self, const std::shared_ptr<const graphics::Layer>& node,
                            std::string path, unsigned groups, double inherited) -> void {
        if (!node || node->hidden || node->opacity == 0) return;
        const auto alpha = inherited * node->opacity;
        const bool group = node->needs_group(alpha);
        if (group)
          std::cerr << "groupAudit=" << display << ";path=" << path << ";ancestorGroups=" << groups
                    << ";alpha=" << alpha << ";localOpacity=" << node->opacity
                    << ";blend=" << unsigned(node->blend) << ";mask=" << bool(node->mask)
                    << ";clip=" << node->clip << '\n';
        if (node->kind == graphics::Kind::backdrop && !node->filters.empty())
          std::cerr << "backdropAudit=" << display << ";path=" << path << ";groups=" << groups
                    << ";alpha=" << alpha << ";localOpacity=" << node->opacity
                    << ";blend=" << unsigned(node->blend) << ";clip=" << node->clip
                    << ";cornerRadius=" << node->corner_radius
                    << ";backgroundAlpha=" << node->background.a
                    << ";filters=" << node->filters.size() << '\n';
        for (std::size_t i = 0; i < node->children.size(); ++i)
          self(self, node->children[i], path + "/" + std::to_string(i), groups + group,
               group && node->group_blending ? 1 : alpha);
        if (node->mask) self(self, node->mask, path + "/mask", groups + group, 1);
      };
      walk(walk, r.scene_root(), "root", 0, 1);
    }
#ifdef HEADUNIT_THEME_HAVE_SKIA
    if (const auto* node = std::getenv("HEADUNIT_THEME_PROBE_GPU_ONLY")) {
      graphics::GpuCompositor gpu(1920, 720, node);
      std::vector<std::uint64_t> warm, traversal, submit, completion;
      std::uint64_t cold = 0;
      graphics::GpuTimings phases;
      for (unsigned frame = 0; frame < 63; ++frame) {
        if (animated_gauges) tick(frame + 61);
        const auto began = std::chrono::steady_clock::now();
        phases =
            gpu.compose_scene(r.scene_root(), 2000000000);  // Explicit offline completion limit.
        const auto ns = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - began)
                                          .count());
        if (!frame) cold = ns;
        if (frame >= 3) {
          warm.push_back(ns);
          const auto sample = gpu.scene_cache_stats();
          traversal.push_back(sample.traversal_ns);
          submit.push_back(sample.submit_ns);
          completion.push_back(phases.completion_wait_ns);
        }
      }
      std::sort(warm.begin(), warm.end());
      const auto cache = gpu.scene_cache_stats();
      const auto percentile = [](auto values, unsigned percent) {
        std::sort(values.begin(), values.end());
        return values[(values.size() * percent + 99) / 100 - 1];
      };
      graphics::write_png(gpu.readback(), std::string(output) + "-" + display + "-gpu-only.png");
      std::cout << "{\"gpuOnlyLayout\":\"" << display << "\",\"coldWallNs\":" << cold
                << ",\"warmMedianWallNs\":" << warm[warm.size() / 2]
                << ",\"warmP95WallNs\":" << warm[(warm.size() * 95 + 99) / 100 - 1]
                << ",\"warmMaxWallNs\":" << warm.back()
                << ",\"warmTraversalMedianNs\":" << percentile(traversal, 50)
                << ",\"warmTraversalP95Ns\":" << percentile(traversal, 95)
                << ",\"warmSubmitMedianNs\":" << percentile(submit, 50)
                << ",\"warmSubmitP95Ns\":" << percentile(submit, 95)
                << ",\"warmCompletionMedianNs\":" << percentile(completion, 50)
                << ",\"warmCompletionP95Ns\":" << percentile(completion, 95)
                << ",\"gpuCacheBytes\":" << cache.gpu_cache_bytes << ",\"renderer\":\""
                << gpu.renderer() << "\""
                << ",\"lastTraversalNs\":" << cache.traversal_ns
                << ",\"lastSubmitNs\":" << cache.submit_ns << ",\"cacheBuilds\":" << cache.builds
                << ",\"cacheHits\":" << cache.hits
                << ",\"simpleMaskGroups\":" << cache.simple_mask_groups
                << ",\"strokeBuilds\":" << cache.stroke_builds
                << ",\"strokeHits\":" << cache.stroke_hits
                << ",\"strokeRasterNs\":" << cache.stroke_raster_ns
                << ",\"groupSaves\":" << cache.group_saves
                << ",\"additiveMaskFusions\":" << cache.additive_mask_fusions
                << ",\"simpleFillMaskGroups\":" << cache.simple_fill_mask_groups
                << ",\"backdropOutputClips\":" << cache.backdrop_output_clips
                << ",\"backdropMaskReplacementClips\":" << cache.backdrop_mask_replacement_clips
                << ",\"backdropGroupRegions\":" << cache.backdrop_group_regions
                << ",\"backdropGroupFusions\":" << cache.backdrop_group_fusions
                << ",\"backdropSnapshotRegions\":" << cache.backdrop_snapshot_regions
                << ",\"backdropSnapshotSavedPixels\":" << cache.backdrop_snapshot_saved_pixels
                << ",\"maxDeferredGpuFrames\":" << cache.max_deferred_gpu_frames
                << ",\"backdropMaskShaders\":" << cache.backdrop_mask_shaders
                << ",\"lastValidationNs\":" << phases.validation_ns
                << ",\"lastCompletionWaitNs\":" << phases.completion_wait_ns
                << ",\"animatedGauges\":" << (animated_gauges ? "true" : "false")
                << ",\"realTimeCadence\":" << (realtime ? "true" : "false")
                << ",\"cpuReferenceUsed\":false,\"livePhoneTested\":false}\n";
      continue;
    }
#endif
    const auto render_began = std::chrono::steady_clock::now();
    auto image = r.render();
    const auto render_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - render_began)
                               .count();
    graphics::write_png(image, std::string(output) + "-" + display + ".png");
#ifdef HEADUNIT_THEME_HAVE_SKIA
    if (const auto* node = std::getenv("HEADUNIT_THEME_SKIA_ASSET_PROBE")) {
      // Explicit isolated benchmark only. The live UI service does not switch
      // renderer or negotiate DPManaged merely because this is compiled in.
      graphics::GpuCompositor gpu(1920, 720, node);
      std::vector<std::uint64_t> costs, wall_costs;
      std::uint64_t cold = 0;
      for (unsigned frame = 0; frame < 33; ++frame) {
        const auto began = std::chrono::steady_clock::now();
        const auto cost = gpu.compose_scene(r.scene_root(), 2000000000).draw_ns;
        const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - began)
                              .count();
        if (!frame) cold = cost;
        if (frame >= 3) {
          costs.push_back(cost);
          wall_costs.push_back(wall);
        }
      }
      const auto actual = gpu.readback();
      graphics::write_png(actual, std::string(output) + "-" + display + "-gpu.png");
      unsigned error = 0;
      std::uint64_t sum = 0, changed = 0, over8 = 0;
      for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        unsigned pixel_error = 0;
        for (unsigned c = 0; c < 4; ++c) {
          const auto delta = unsigned(std::abs(int((image.pixels[i] >> (8 * c)) & 255) -
                                               int((actual.pixels[i] >> (8 * c)) & 255)));
          error = std::max(error, delta);
          pixel_error = std::max(pixel_error, delta);
          sum += delta;
        }
        changed += image.pixels[i] != actual.pixels[i];
        over8 += pixel_error > 8;
      }
      std::sort(costs.begin(), costs.end());
      std::sort(wall_costs.begin(), wall_costs.end());
      const auto cache = gpu.scene_cache_stats();
      std::cout << "{\"gpuSceneAsset\":\"" << display << "\",\"coldNs\":" << cold
                << ",\"medianNs\":" << costs[costs.size() / 2]
                << ",\"p95Ns\":" << costs[costs.size() * 95 / 100] << ",\"maxError\":" << error
                << ",\"meanError\":" << double(sum) / (image.pixels.size() * 4)
                << ",\"changedPixels\":" << changed << ",\"pixelsOver8\":" << over8
                << ",\"medianWallNs\":" << wall_costs[wall_costs.size() / 2]
                << ",\"p95WallNs\":" << wall_costs[wall_costs.size() * 95 / 100]
                << ",\"retainedHits\":" << cache.hits << ",\"retainedBuilds\":" << cache.builds
                << ",\"retainedBytes\":" << cache.bytes << ",\"imageHits\":" << cache.image_hits
                << ",\"imageBuilds\":" << cache.image_builds
                << ",\"imageBytes\":" << cache.image_bytes
                << ",\"gpuCacheBytes\":" << cache.gpu_cache_bytes
                << ",\"lastTraversalNs\":" << cache.traversal_ns
                << ",\"lastSubmitNs\":" << cache.submit_ns
                << ",\"simulatedStateOnly\":true,\"livePhoneTested\":false}\n";
    }
#endif
    const auto nonblack = std::count_if(image.pixels.begin(), image.pixels.end(),
                                        [](auto p) { return (p & 0x00ffffff) != 0; });
    std::cout
        << "{\"layoutSimulation\":\"" << display
        << "\",\"entryPoint\":true,\"ticks\":60,\"modules\":" << r.stats().modules
        << ",\"offlinePalette\":"
        << (palette && std::string_view(display) == "Cluster_Display" ? "\"" + *palette + "\""
                                                                      : "null")
        << ",\"syntheticSignals\":"
        << (local_profile ? (synthetic_gauges ? 5 + decoded_profile->vehicle_inputs.size() : 0)
                          : samples.size())
        << ",\"localProfile\":" << (local_profile ? "true" : "false")
        << ",\"regionReports\":" << reports << ",\"declaredRegions\":" << regions
        << ",\"nonblackPixels\":" << nonblack << ",\"startupMs\":" << startup_ms
        << ",\"renderUs\":" << render_us << ",\"maxTickUs\":" << max_tick_us
        << ",\"startupBudgetMs\":10000,\"tickBudgetMs\":1000,\"independentlyVerified\":false,"
           "\"simulatedClock\":true,\"simulatedForeground\":true,\"livePhoneTested\":false}\n";
    if (local_profile) {
      CHECK(!r.stats().faulted);
      continue;
    }
    // Exercise a real subscription update through the original asset, rather
    // than changing its background or palettes from the test host. The final
    // fixture characteristic is explicitly the synthetic night-mode signal.
    CHECK(samples.back().path[2] == "nightMode");
    source->publish(samples.size() + 2, Value{true}, now, now);
    for (unsigned frame = 61; frame <= 120; ++frame)
      tick(frame);
    auto night = r.render();
    graphics::write_png(night, std::string(output) + "-" + display + "-night.png");
    const auto changed = std::inner_product(image.pixels.begin(), image.pixels.end(),
                                            night.pixels.begin(), std::size_t{0}, std::plus<>{},
                                            [](auto a, auto b) { return std::size_t(a != b); });
    CHECK(changed > 0);
    CHECK(!r.stats().faulted);
    std::cout << "{\"layoutSimulation\":\"" << display
              << "\",\"nightModeSubscriptionUpdate\":true,\"changedPixels\":" << changed
              << ",\"maxTickUs\":" << max_tick_us
              << ",\"tickBudgetMs\":1000,\"livePhoneTested\":false}\n";
    source->publish(samples.size() + 2, Value{false}, now, now);
  }
}
void describe_layouts(const script::ResourceReader& asset) {
  // Read only selected metadata through the same constrained, signature-checked
  // module loader. Never dump instrument code, images or full asset tables.
  script::Runtime runtime(asset, {"Cluster_Display"});
  std::size_t records = 0, bytes = 0;
  runtime.bind_log_sink([&](int, std::string_view tag, std::string_view message) {
    CHECK(tag == "layoutMetadata" && ++records <= 1024 && (bytes += message.size()) <= 128 * 1024);
    std::cout << message << '\n';
  });
  runtime.evaluate(R"(
    local function emit(...)
      local fields = {...}
      for i, value in ipairs(fields) do
        assert(type(value) == 'string' or type(value) == 'number')
        local text = tostring(value)
        assert(#text <= 128)
        fields[i] = string.gsub(text, '[%c|]', ' ')
      end
      CP.log(2, 'layoutMetadata', table.concat(fields, '|'))
    end
    local theme = require('r.ThemeData')
    local paletteIDs = {}
    for id, palette in pairs(theme.palettes) do
      assert(type(id) == 'string' and type(palette) == 'table' and palette.id == id)
      assert(#paletteIDs < 64)
      paletteIDs[#paletteIDs + 1] = id
    end
    table.sort(paletteIDs)
    for _, id in ipairs(paletteIDs) do
      emit('palette', id, theme.palettes[id].displayName or '')
    end
    local displays = require('r.PackageData').displays
    assert(type(displays) == 'table' and #displays <= 8)
    for _, display in ipairs(displays) do
      assert(#display.layouts <= 64)
      emit('display', display.id, #display.layouts)
      for _, layout in ipairs(display.layouts) do
        emit('layout', display.id, layout.id, layout.displayName or '', layout.layoutType or '', layout.layoutStyleID or '')
        assert(#layout.zones <= 128)
        for _, zone in ipairs(layout.zones) do
          local kind = zone.type.dca and 'dca' or zone.type.single and 'single' or 'other'
          emit('zone', display.id, layout.id, zone.id, zone.displayName or '', kind, zone.zoneRegion or '', #zone.components)
          assert(#zone.components <= 128)
          for _, component in ipairs(zone.components) do
            emit('component', display.id, layout.id, zone.id, component.id, component.name or '', component.type or '', #component.slots)
          end
        end
      end
    end
  )",
                   "offline-layout-metadata");
  CHECK(records > 0 && !runtime.stats().faulted);
}
int main(int argc, char** argv) {
  try {
    const bool allow_expired =
        argc > 1 && std::string_view(argv[argc - 1]) == "--allow-expired-leaf";
    if (allow_expired) --argc;
    if (argc != 5 &&
        !(argc == 7 &&
          (std::string_view(argv[5]) == "--worker" || std::string_view(argv[5]) == "--worker-gpu" ||
           std::string_view(argv[5]) == "--draw" || std::string_view(argv[5]) == "--layout" ||
           std::string_view(argv[5]) == "--drive-modes")) &&
        !(argc == 6 && (std::string_view(argv[5]) == "--vehicle" ||
                        std::string_view(argv[5]) == "--describe-layouts" ||
                        std::string_view(argv[5]) == "--controls")) &&
        !(argc == 8 && (std::string_view(argv[5]) == "--layout-local" ||
                        std::string_view(argv[5]) == "--greeting")))
      throw std::runtime_error(
          "usage: headunit-theme-probe ASSET_DIRECTORY ASSET_ID VERSION LOCAL_REFERENCE_OR_DASH "
          "[--draw PNG | --vehicle | --controls | --describe-layouts | --layout OUTPUT_PREFIX | "
          "--layout-local PROFILE OUTPUT_PREFIX | --greeting PROFILE OUTPUT_PREFIX | --drive-modes "
          "PROFILE] [--allow-expired-leaf]");
    const std::filesystem::path directory(argv[1]);
    const std::string version(argv[3]);
    std::uint64_t number = 0;
    const auto [end, error] =
        std::from_chars(version.data(), version.data() + version.size(), number);
    if (error != std::errc{} || end != version.data() + version.size())
      throw std::invalid_argument("invalid version");
    headunit::AssetBundlePolicy policy{argv[2], number, {}};
    if (allow_expired)
      policy.certificate_time_policy = headunit::AssetCertificateTimePolicy::allow_expired_leaf;
    if (std::string_view(argv[4]) != "-")
      policy.previously_accepted = headunit::read_asset_reference(argv[4]);
    const auto bundle = headunit::AssetBundle::open(
        (directory / "archive.zip").string(), (directory / "signature.der").string(),
        (directory / "certificate.pem").string(), (directory / "trusted-root.pem").string(),
        policy);
    if (bundle.info().expired_signer_exception)
      std::clog << "Asset trust: expiredSigningLeafExceptionUsed=true; chain and archive signature "
                   "verified\n";
    const auto reader = script::cache_resources([&](std::string_view path) {
      auto bytes = bundle.resource(std::string(path));
      return std::string(bytes.begin(), bytes.end());
    });
    if (argc == 8 && std::string_view(argv[5]) == "--greeting") {
      greeting_probe(reader, argv[6], argv[7]);
      return 0;
    }
    const auto palette = probe_palette(reader);
    if (palette && !((argc == 7 && std::string_view(argv[5]) == "--layout") || argc == 8))
      throw std::invalid_argument("offline palette selection is supported only by --layout");
    if (argc == 7 &&
        (std::string_view(argv[5]) == "--worker" || std::string_view(argv[5]) == "--worker-gpu")) {
      worker_probe(reader, argv[6], std::string_view(argv[5]) == "--worker-gpu");
      return 0;
    }
    if (argc == 6 && std::string_view(argv[5]) == "--describe-layouts") {
      describe_layouts(reader);
      return 0;
    }
    if (argc == 6 && std::string_view(argv[5]) == "--controls") {
      controls_probe(reader);
      return 0;
    }
    if (argc == 7 && std::string_view(argv[5]) == "--drive-modes") {
      bench::drive_mode_probe(reader, read_root(argv[6]));
      return 0;
    }
    const auto main = probe(reader, "Center_Display");
    const auto dash = probe(reader, "Cluster_Display");
    std::cout << "{\"verifiedAssetVersion\":" << bundle.info().version
              << ",\"displays\":2,\"handshake\":true,\"nestedCommand\":true,\"ack\":true,"
                 "\"duplicateRejected\":true,\"gapReset\":true"
              << ",\"modulesPerDisplay\":" << main.modules << ",\"memoryBytes\":["
              << main.memory_bytes << ',' << dash.memory_bytes
              << "],\"graphicsTested\":false,\"livePhoneTested\":false}\n";
    if (argc == 7 && std::string_view(argv[5]) == "--draw") draw_probe(reader, argv[6]);
    if (argc == 7 && std::string_view(argv[5]) == "--layout")
      layout_probe(reader, argv[6], nullptr, palette);
    if (argc == 8) layout_probe(reader, argv[7], argv[6], palette);
    if (argc == 6) vehicle_probe(reader);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
