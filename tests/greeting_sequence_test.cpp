// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/greeting_sequence.hpp>
#include <headunit/native_cluster.hpp>
#include <headunit/theme/greeting_overlay.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>

namespace {
void check(bool condition) {
  if (!condition) throw std::runtime_error("greeting check failed");
}
void write(const std::filesystem::path& path, const std::string& data) {
  std::ofstream(path) << data;
  chmod(path.c_str(), 0600);
}
std::uint64_t nanos(headunit::Clock::time_point now) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}
}  // namespace
int main() try {
  using namespace headunit;
  using namespace std::chrono_literals;
  const auto now = Clock::now();
  GreetingSequence sequence;
  check(parse_greeting_variant("brand\n") == GreetingVariant::brand);
  check(!parse_greeting_variant("brand\n\n"));
  check(!parse_greeting_variant("BRAND"));
  sequence.begin(GreetingPhase::startup, GreetingVariant::brand, now);
  const auto frozen = sequence.frame(now + 400ms);
  check(frozen.opacity > .9 && frozen.reveal > 0);
  check(sequence.frame(now + 1800ms).phase == GreetingPhase::inactive);
  check(frozen.phase == GreetingPhase::startup);
  sequence.begin(GreetingPhase::goodbye, GreetingVariant::minimal, now);
  check(sequence.frame(now + 200ms).phase == GreetingPhase::goodbye);
  check(sequence.frame(now + 900ms).phase == GreetingPhase::inactive);
  sequence.begin(GreetingPhase::startup, GreetingVariant::off, now);
  check(sequence.frame(now + 100ms).phase == GreetingPhase::inactive);
  sequence.begin(GreetingPhase::startup, GreetingVariant::brand, now);
  sequence.frame(now + 200ms);
  check(sequence.frame(now + 100ms).phase == GreetingPhase::inactive);

  char folder[] = "/tmp/headunit-greeting-XXXXXX";
  check(mkdtemp(folder));
  const std::filesystem::path root = std::filesystem::canonical(folder);
  const auto path = root / "greeting-sequence";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{root};
  write(path, "brand\n");
  GreetingSequenceFile reader(path, true);
  auto tick = Clock::now();
  check(reader.frame(tick).phase == GreetingPhase::startup);
  write(path, "off\n");
  check(reader.frame(tick + 60ms).phase == GreetingPhase::inactive);
  write(path, "minimal\n");
  check(reader.frame(tick + 120ms).phase == GreetingPhase::inactive);
  const auto acquired = nanos(tick + 120ms);
  write(path.string() + ".event", "goodbye " + std::to_string(acquired) + "\n");
  check(reader.frame(tick + 180ms).phase == GreetingPhase::goodbye);
  check(reader.frame(tick + 1100ms).phase == GreetingPhase::inactive);
  // An old event cannot be renewed by rereading, replacement or a new owner.
  check(reader.frame(tick + 1200ms).phase == GreetingPhase::inactive);
  GreetingSequenceFile successor(path);
  check(successor.frame(tick + 2s).phase == GreetingPhase::inactive);
  write(path.string() + ".event", "goodbye " + std::to_string(nanos(tick + 10s)) + "\n");
  check(reader.frame(tick + 1300ms).phase == GreetingPhase::inactive);
  // Unsafe and special-file settings cannot block or keep decoration alive.
  chmod(path.c_str(), 0644);
  check(reader.frame(tick + 1400ms).phase == GreetingPhase::inactive);
  std::filesystem::remove(path);
  check(mkfifo(path.c_str(), 0600) == 0);
  check(reader.frame(tick + 1500ms).phase == GreetingPhase::inactive);
  std::filesystem::remove(path);
  const auto alternate = root / "alternate";
  write(alternate, "brand\n");
  std::filesystem::create_symlink(alternate, path);
  check(reader.frame(tick + 1600ms).phase == GreetingPhase::inactive);
  // Pixel evidence: decoration leaves actual gauge/warning regions unchanged.
  // Both first-frame and completion frames are exactly the ordinary cluster.
  ClusterContent content;
  content.center = ClusterCenter::media;
  constexpr unsigned width = 1920, height = 720;
  std::vector<std::uint32_t> baseline(width * height), decorated(width * height);
  draw_native_cluster(content, now, baseline.data(), width, height, width * 4);
  content.greeting = frozen;
  draw_native_cluster(content, now, decorated.data(), width, height, width * 4);
  std::size_t changed = 0;
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      if (baseline[y * width + x] != decorated[y * width + x]) {
        ++changed;
        check(x >= 870 && x <= 1050 && y >= 165 && y <= 300);
      }
    }
  }
  check(changed > 100);
  namespace graphics = headunit::theme::graphics;
  const auto overlay = graphics::greeting_overlay(frozen, {1920, 720});
  check(overlay && overlay->background.a == 0 && overlay->children.size() == 1);
  const auto pixels =
      graphics::render(std::const_pointer_cast<graphics::Layer>(overlay), width, height);
  std::size_t painted = 0;
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      if (pixels.pixels[y * width + x] >> 24) {
        ++painted;
        check(x >= 870 && x <= 1050 && y >= 165 && y <= 300);
      }
    }
  }
  check(painted > 100);
  check(!graphics::greeting_overlay({}, {1920, 720}));
  content.greeting = {};
  draw_native_cluster(content, now, decorated.data(), width, height, width * 4);
  check(baseline == decorated);
  std::cout << "greeting lifecycle/private requests and portable pixels pass (simulated timing)\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
