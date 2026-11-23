// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_display.hpp>
#include <headunit/theme/native_input.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <atomic>
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
std::uint64_t ntp(std::uint64_t elapsed) {
  return (std::uint64_t{900} << 32) + (elapsed / 1000000000 << 32) +
         ((elapsed % 1000000000) << 32) / 1000000000;
}
template <class F>
void until(F test) {
  const auto until = ns() + 5000000000ULL;
  while (!test()) {
    CHECK(ns() < until);
    std::this_thread::sleep_for(2ms);
  }
}
int main() try {
  // Queue bounds apply before decoder allocation, include wire-header overhead,
  // and leave a rejected caller packet unchanged.
  {
    auto stream = std::make_shared<video::Stream>(video::Input{"queue", 64, 48},
                                                  std::make_shared<video::Memory>());
    auto queue = std::make_shared<video::NativeInputQueue>();
    video::NativeInput input(stream, queue);
    for (unsigned i = 0; i < video::NativeInputQueue::max_packets; ++i) {
      AirplayVideoPacket p{};
      p.kind = AirplayVideoPacket::Kind::config;
      CHECK(queue->enqueue(p));
    }
    AirplayVideoPacket held{};
    held.kind = AirplayVideoPacket::Kind::config;
    held.data = {1, 2, 3};
    CHECK(!queue->enqueue(held) && held.data == Bytes({1, 2, 3}));
    input.pump(nullptr);
    input.pump(nullptr);
    AirplayVideoPacket empty{};
    empty.kind = AirplayVideoPacket::Kind::config;
    CHECK(queue->enqueue(empty));
    input.pump(nullptr);
    for (unsigned i = 0; i < 3; ++i) {
      AirplayVideoPacket p{};
      p.kind = AirplayVideoPacket::Kind::config;
      p.data.resize(video::NativeInputQueue::max_packet_bytes);
      CHECK(queue->enqueue(p));
    }
    AirplayVideoPacket full{};
    full.kind = AirplayVideoPacket::Kind::config;
    full.data.resize(video::NativeInputQueue::max_packet_bytes);
    CHECK(!queue->enqueue(full) && full.data.size() == video::NativeInputQueue::max_packet_bytes);
    queue->close();
    bool failed = false;
    try {
      input.pump(nullptr);
    } catch (const std::exception&) {
      failed = true;
    }
    CHECK(failed);
    failed = false;
    try {
      queue->enqueue(held);
    } catch (const std::exception&) {
      failed = true;
    }
    CHECK(failed);
  }
  {
    auto queue = std::make_shared<video::NativeInputQueue>();
    {
      auto stream = std::make_shared<video::Stream>(video::Input{"threaded", 64, 48},
                                                    std::make_shared<video::Memory>());
      video::NativeInput input(stream, queue);
      std::atomic<bool> done = false;
      std::exception_ptr error;
      std::jthread producer([&] {
        try {
          const auto deadline = ns() + 3000000000ULL;
          for (unsigned i = 0; i < 512; ++i) {
            AirplayVideoPacket p{};
            p.kind = AirplayVideoPacket::Kind::config;
            while (!queue->enqueue(p)) {
              CHECK(ns() < deadline);
              std::this_thread::yield();
            }
          }
        } catch (...) {
          error = std::current_exception();
        }
        done = true;
      });
      while (!done) {
        input.pump(nullptr);
        std::this_thread::yield();
      }
      producer.join();
      if (error) std::rethrow_exception(error);
    }
    AirplayVideoPacket p{};
    p.kind = AirplayVideoPacket::Kind::config;
    bool failed = false;
    try {
      queue->enqueue(p);
    } catch (const std::exception&) {
      failed = true;
    }
    CHECK(failed);
  }
  std::array<std::uint8_t, 32> key{};
  std::cin.read(reinterpret_cast<char*>(key.data()), key.size());
  CHECK(std::cin);
  AirplayVideoStream parser(key);
  std::vector<AirplayVideoPacket> packets;
  std::array<std::uint8_t, 4096> bytes{};
  while (std::cin.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || std::cin.gcount())
    for (auto& p : parser.feed(std::span(bytes).first(std::cin.gcount()))) {
      CHECK(packets.size() < 32);
      packets.push_back(std::move(p));
    }
  CHECK(!parser.partial() && packets.size() >= 10);
  const auto burst_fixture = packets;
  {
    // Real hardware decoder, intentionally full native queue, then a pause
    // longer than the stall deadline as can occur during signed module loading.
    auto memory = std::make_shared<video::Memory>();
    auto stream = std::make_shared<video::Stream>(video::Input{"startup-gap", 1280, 720}, memory);
    auto queue = std::make_shared<video::NativeInputQueue>();
    video::NativeInput input(stream, queue, "v4l2slh265dec");
    auto config = burst_fixture.front();
    CHECK(queue->enqueue(config));
    const auto fill = [&](std::uint64_t base) {
      for (unsigned i = 0; i < 48; ++i) {
        auto packet = burst_fixture[1 + i % (burst_fixture.size() - 1)];
        CHECK(packet.kind == AirplayVideoPacket::Kind::frame);
        packet.sender_ntp = packet.timestamp = ntp(base + 10000000ULL + i * 1000000ULL);
        CHECK(queue->enqueue(packet));
      }
    };
    fill(0);
    const auto fresh_clock = [] {
      video::MediaClock clock;
      const auto at = ns();
      clock.update({true, ntp(0) + (std::uint64_t{2208988800} << 32), at, at, at, 1000});
      return clock;
    };
    const auto settle = [&] {
      auto last_submitted = input.stats().submitted;
      auto last_progress = ns();
      until([&] {
        const auto clock = fresh_clock();
        input.pump(&clock);
        if (input.stats().submitted != last_submitted) {
          last_submitted = input.stats().submitted;
          last_progress = ns();
        }
        return !stream->can_offer_native() && input.stats().backpressure > 0 &&
               ns() - last_progress >= 250000000;
      });
    };
    settle();
    const auto submitted = input.stats().submitted;
    const auto decoded = input.stats().decoded;
    std::this_thread::sleep_for(3100ms);
    input.suspend();  // Output suspension genuinely releases native leases.
    input.pump(nullptr);
    CHECK(!queue->closed() && input.stats().decoded > decoded);
    until([&] {
      input.pump(nullptr);
      return input.stats().decoded == 48;
    });
    CHECK(input.stats().submitted > submitted);
    CHECK(memory->native_frames == 0 && input.stats().clock_dropped > 0);
    fill(100000000);
    settle();
    std::this_thread::sleep_for(3100ms);
    bool retired = false;
    try {
      const auto clock = fresh_clock();
      input.pump(&clock);  // No release/progress: original stall limit still retires.
    } catch (const std::runtime_error& error) {
      CHECK(std::string_view(error.what()) == "native decoder stalled");
      retired = true;
    }
    CHECK(retired && queue->closed() && memory->native_frames == 0);
  }
  const auto origin = ns();
  bool locked = true;
  auto source = [&]() -> std::optional<AirplayClockSample> {
    if (!locked) return {};
    const auto at = ns();
    return AirplayClockSample{ntp(at - origin) + (std::uint64_t{2208988800} << 32), at, at, 1000};
  };
  script::Display spec{"native-fixture", 1280, 720};
  script::Runtime runtime(
      [](std::string_view) -> std::string { throw std::runtime_error("no resource"); }, spec);
  auto stream = runtime.add_video_input({"phone", 1280, 720});
  auto queue = std::make_shared<video::NativeInputQueue>();
  auto input = std::make_shared<video::NativeInput>(stream, queue, "v4l2slh265dec");
  graphics::GpuDisplayOptions options;
  options.inputs = {input};
  if (std::getenv("HEADUNIT_THEME_TEST_TWO_PENDING")) options.window.max_pending_presentations = 2;
  if (std::getenv("HEADUNIT_THEME_TEST_DEFER_FENCE")) options.window.defer_scene_fence = true;
  graphics::GpuDisplay display(runtime, spec, source, options);
  script::UiWorker::DisplayTurn turn;
  until([&] {
    turn = display.turn();
    return turn.allow_tasks;
  });
  runtime.tick(turn.pts);
  runtime.evaluate(R"(
    movie=CP.DPVideoLayer.create('phone');movie.frame=CP.DPRect(0,0,1280,720);movie:subscribe()
    CP.setRootLayer('native-fixture',movie)
  )");
  runtime.start_tasks(turn.pts);
  auto tick = [&] {
    turn = display.turn();
    if (turn.allow_tasks) runtime.tick(turn.pts);
  };
  CHECK(packets[0].kind == AirplayVideoPacket::Kind::config && queue->enqueue(packets[0]));
  std::uint64_t frame_count = 0;
  for (std::size_t i = 1; i < packets.size(); ++i) {
    std::cerr << "fixture frame " << i << " decoded=" << input->stats().decoded
              << " presented=" << input->stats().presented << "\n";
    auto& packet = packets[i];
    if (packet.kind != AirplayVideoPacket::Kind::frame) continue;
    // The encoded fixture is synthetic, so explicitly re-time each access unit
    // to our synthetic network clock. Real production packets are never retimed.
    const auto due = ns() + 120000000;
    const auto stamp = ntp(due - origin);
    packet.timestamp = stamp;
    packet.sender_ntp = stamp;
    const auto before = stream->current_pts();
    const auto decoded = input->stats().decoded;
    auto extra = packet;
    if (i == 3) runtime.evaluate("movie.hidden=true");
    CHECK(queue->enqueue(packet));
    until([&] {
      tick();
      return input->stats().decoded > decoded;
    });
    if (i == 3) {
      const auto until = ns() + 180000000;
      while (ns() < until) {
        tick();
        CHECK(stream->current_pts() == before);
        std::this_thread::sleep_for(2ms);
      }
      runtime.evaluate("movie.hidden=false");
    }
    until([&] {
      tick();
      return input->stats().presented > frame_count;
    });
    ++frame_count;
    CHECK(stream->current_pts() &&
          *stream->current_pts() ==
              double(stamp >> 32) + double(stamp & 0xffffffffULL) / 4294967296.);
    CHECK(ns() >= due);  // Future video must not be presented early.
    if (i == 2) {
      CHECK(queue->enqueue(extra));
      until([&] {
        tick();
        return input->stats().decoded > decoded + 1;
      });
      CHECK(input->stats().presented == frame_count && input->stats().non_monotonic == 1);
    }
    if (i == 4) {
      locked = false;
      tick();
      CHECK(!turn.allow_tasks && !stream->current_pts() && !stream->native());
      extra.timestamp = stamp + 1;
      extra.sender_ntp = stamp + 1;
      CHECK(queue->enqueue(extra));
      until([&] {
        tick();
        return input->stats().clock_dropped == 1;
      });
      CHECK(!stream->current_pts() && !stream->native());
      locked = true;
      until([&] {
        tick();
        return turn.allow_tasks;
      });
    }
  }
  CHECK(frame_count >= 9 && input->stats().submitted == frame_count + 2 &&
        input->stats().decoded == frame_count + 2);
  CHECK(input->stats().late == 0 && input->stats().clock_dropped == 1 &&
        input->stats().non_monotonic == 1);
  bool fresh_burst = false;
  if (std::getenv("HEADUNIT_THEME_TEST_TWO_PENDING")) {
    // Replay at 60 Hz while two Wayland receipts may remain pending. A single
    // staged source used by alternating scenes loses roughly half this burst.
    // This checks actual hardware decoding and source presentation, not draws.
    const auto base = ns() + 150000000, before = input->stats().presented,
               discarded = input->stats().discarded;
    unsigned count = 0;
    for (auto packet : burst_fixture) {
      if (packet.kind != AirplayVideoPacket::Kind::frame) continue;
      const auto stamp = ntp(base - origin + count * 16666667ULL);
      packet.timestamp = stamp;
      packet.sender_ntp = stamp;
      CHECK(queue->enqueue(packet));
      ++count;
    }
    CHECK(count >= 10);
    until([&] {
      tick();
      return ns() > base + count * 16666667ULL + 150000000;
    });
    // The headless compositor may discard commits; those source completions
    // are valid rejections, not evidence of displayed fresh frames. This tests
    // two distinct leases and completion accounting, not a refresh-rate SLA.
    CHECK(input->stats().max_staged == 2);
    CHECK(input->stats().presented - before + input->stats().discarded - discarded >= count - 2);
    CHECK(!display.stats().faulted && input->stats().late == 0 &&
          input->stats().clock_dropped == 1);
    fresh_burst = true;
  }
  queue->close();
  bool failed = false;
  try {
    tick();
  } catch (const std::exception&) {
    failed = true;
  }
  CHECK(failed && display.stats().faulted && !stream->current_pts() && !stream->native());
  std::cout << "{\"nativePresented\":" << frame_count
            << ",\"freshBurst\":" << (fresh_burst ? "true" : "false")
            << ",\"actualWaylandFeedback\":true,\"syntheticClock\":true,\"queueBounds\":true,"
               "\"hiddenLayerHeld\":true,\"clockRecovery\":true,\"disconnectWithdrawn\":true}\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
