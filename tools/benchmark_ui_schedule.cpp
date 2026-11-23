// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_worker.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace headunit::theme::script;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
int main() try {
  const std::array costs{std::array{0ms, 0ms}, std::array{10ms, 0ms}, std::array{10ms, 12ms},
                         std::array{25ms, 0ms}};
  for (const auto& cost : costs) {
    std::vector<Clock::time_point> starts;
    UiWorker::Configuration config;
    config.displays = {{"Center_Display"}, {"Cluster_Display"}};
    config.routes = {{"aabbccdd-0000-0000-0000-000000000001", "Center_Display"},
                     {"aabbccdd-0000-0000-0000-000000000002", "Cluster_Display"}};
    config.resources = [](std::string_view) -> std::string {
      return "return {}";
    };
    config.presentation = [&](Runtime&, const Display& display, UiWorker::DisplayPhase phase) {
      if (phase == UiWorker::DisplayPhase::frame) {
        const auto panel = display.identifier == "Center_Display" ? 0U : 1U;
        if (panel == 0) starts.push_back(Clock::now());
        std::this_thread::sleep_for(cost[panel]);
      }
      return UiWorker::DisplayTurn{{}, false};
    };
    // Permit bootstrap without inventing a phone PTS. The trivial script has no
    // timed tasks; synthetic cost is applied only to ordinary worker turns.
    const auto hook = config.presentation;
    config.presentation = [hook](Runtime& r, const Display& d, UiWorker::DisplayPhase phase) {
      if (phase == UiWorker::DisplayPhase::bootstrap) return UiWorker::DisplayTurn{{}, true};
      return hook(r, d, phase);
    };
    {
      UiWorker worker(7, std::move(config));
      std::this_thread::sleep_for(2100ms);
      worker.request_stop();
      while (!worker.finished())
        std::this_thread::sleep_for(1ms);
      if (!worker.failure_reason().empty()) throw std::runtime_error(worker.failure_reason());
    }
    if (starts.size() < 5) throw std::runtime_error("insufficient worker samples");
    std::vector<double> intervals;
    for (std::size_t i = 2; i < starts.size(); ++i)
      intervals.push_back(
          std::chrono::duration<double, std::milli>(starts[i] - starts[i - 1]).count());
    std::sort(intervals.begin(), intervals.end());
    std::cout << "{\"syntheticMainWorkMs\":" << cost[0].count()
              << ",\"syntheticClusterWorkMs\":" << cost[1].count()
              << ",\"phoneFrames\":0,\"presentations\":0"
              << ",\"intervalSamples\":" << intervals.size()
              << ",\"medianIntervalMs\":" << intervals[intervals.size() / 2]
              << ",\"p95IntervalMs\":"
              << intervals[std::min(intervals.size() - 1, intervals.size() * 95 / 100)]
              << ",\"minimumIntervalMs\":" << intervals.front()
              << ",\"maximumIntervalMs\":" << intervals.back() << "}\n";
  }
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
