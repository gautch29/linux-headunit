// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Explicit private Linux fixture; no display, phone or physical input.
#include <headunit/dated_simulated_vehicle.hpp>
#include <headunit/prephone_publication.hpp>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <time.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}
class Source final : public headunit::VehicleSource {
 public:
  explicit Source(bool dated) : dated_(dated) { begin(); }
  void begin() {
    const auto started = headunit::Clock::now();
    ++generation_;
    if (dated_)
      source_ = std::make_unique<headunit::DatedSimulatedVehicle>(started, generation_);
    else
      source_ = std::make_unique<headunit::SimulatedVehicle>(started);
    paused = false;
  }
  void retire() {
    if (dated_)
      static_cast<headunit::DatedSimulatedVehicle&>(*source_).retire(headunit::Clock::now());
  }
  headunit::VehicleSample sample(headunit::Clock::time_point now) override {
    if (paused) return headunit::current_vehicle_sample(held_, now);
    held_ = source_->sample(now);
    return held_;
  }
  bool paused = false;

 private:
  const bool dated_;
  std::uint64_t generation_ = 0;
  std::unique_ptr<headunit::VehicleSource> source_;
  headunit::VehicleSample held_;
};
}  // namespace
int main(int argc, char** argv) try {
  if (argc != 3 || (std::string_view(argv[2]) != "dated" && std::string_view(argv[2]) != "legacy"))
    return 2;
  // Independent runtime check of the actual standard-library/kernel domain.
  timespec before{}, after{};
  if (clock_gettime(CLOCK_MONOTONIC, &before) != 0) return 2;
  const auto steady = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          headunit::Clock::now().time_since_epoch())
                          .count();
  if (clock_gettime(CLOCK_MONOTONIC, &after) != 0) return 2;
  const auto first = before.tv_sec * 1'000'000'000LL + before.tv_nsec;
  const auto last = after.tv_sec * 1'000'000'000LL + after.tv_nsec;
  if (steady < first || steady > last) return 2;
  std::cout << first << ' ' << steady << ' ' << last << '\n' << std::flush;
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  std::signal(SIGHUP, stop);
  auto source = std::make_unique<Source>(std::string_view(argv[2]) == "dated");
  auto* control = source.get();
  headunit::PublishingVehicle vehicle(std::move(source), argv[1]);
  bool stalled = false;
  while (!stopping) {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if (poll(&input, 1, 10) > 0 && input.revents & POLLIN) {
      char command = 0;
      if (read(STDIN_FILENO, &command, 1) != 1) break;
      if (command == 'p') control->paused = true;
      if (command == 'r') control->paused = false;
      if (command == 'w') control->retire();
      if (command == 'n') control->begin();
      if (command == 's') stalled = true;
      if (command == 'q') break;
      std::cout << command << '\n' << std::flush;
    }
    if (!stalled) vehicle.sample(headunit::Clock::now());
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
