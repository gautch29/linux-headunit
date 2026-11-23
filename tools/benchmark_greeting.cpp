// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/native_cluster.hpp>
#include <algorithm>
#include <iostream>
#include <vector>

int main() {
  using namespace headunit;
  constexpr unsigned width = 1920, height = 720;
  std::vector<std::uint32_t> pixels(width * height);
  ClusterContent content;
  content.center = ClusterCenter::media;
  const auto began = Clock::now();
  draw_native_cluster(content, began, pixels.data(), width, height, width * 4);
  const auto cold = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
  std::vector<double> off, on;
  // Alternate identical unavailable-reading frames after cold font/backdrop
  // preparation. Animation time is an explicit fixture; costs use real clocks.
  for (unsigned i = 0; i < 120; ++i) {
    for (unsigned mode = 0; mode < 2; ++mode) {
      const bool enabled = (mode + i) % 2;
      content.greeting = enabled
                             ? GreetingFrame{GreetingVariant::brand, GreetingPhase::startup, 1, .5}
                             : GreetingFrame{};
      const auto start = Clock::now();
      draw_native_cluster(content, began, pixels.data(), width, height, width * 4);
      const auto cost = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      (enabled ? on : off).push_back(cost);
    }
  }
  std::sort(off.begin(), off.end());
  std::sort(on.begin(), on.end());
  std::cout << "{\"evidence\":\"portable CPU native drawing; no boot, phone, GPU or presentation "
               "measurement\","
            << "\"width\":" << width << ",\"height\":" << height << ",\"samplesPerVariant\":120,"
            << "\"coldNativeDrawMs\":" << cold << ",\"offMedianMs\":" << off[60]
            << ",\"brandMedianMs\":" << on[60] << ",\"offP95Ms\":" << off[113]
            << ",\"brandP95Ms\":" << on[113] << ",\"medianIncrementMs\":" << on[60] - off[60]
            << "}\n";
}
