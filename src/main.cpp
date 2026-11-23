// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/controller.hpp>
#include <headunit/vehicle.hpp>

#include <chrono>
#include <iostream>

int main() {
  const auto now = headunit::Clock::now();
  headunit::Controller controller;
  headunit::SimulatedVehicle vehicle(now);
  const auto sample = vehicle.sample(now);
  const auto displays = controller.update({}, now);
  std::cout << "main="
            << (displays.main == headunit::MainSurface::standalone ? "standalone" : "carplay")
            << " cluster="
            << (displays.cluster == headunit::ClusterSurface::backup ? "backup" : "carplay")
            << " speed_kmh=" << *sample.speed_kmh << " rpm=" << *sample.engine_rpm << '\n';
}
