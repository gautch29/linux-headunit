// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/backup_cluster.hpp>
#include <headunit/vehicle.hpp>
#include <headunit/vehicle_wire.hpp>

#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) try {
  if (argc != 2 && argc != 4)
    throw std::invalid_argument("usage: headunit-cluster-snapshot OUTPUT.ppm [--sample HUV2_FILE]");
  const auto now = headunit::Clock::now();
  headunit::SimulatedVehicle source(now);
  auto sample = source.sample(now);
  if (argc == 4) {
    if (std::string_view(argv[2]) != "--sample")
      throw std::invalid_argument("unknown snapshot option");
    std::ifstream input(argv[3]);
    if (!input) throw std::runtime_error("cannot open snapshot input");
    std::string data(headunit::maximum_vehicle_datagram + 1, '\0');
    input.read(data.data(), data.size());
    data.resize(input.gcount());
    const auto parsed = headunit::parse_vehicle_datagram(data, now);
    if (!parsed) throw std::invalid_argument("invalid vehicle snapshot");
    sample = *parsed;
  }
  std::ofstream output(argv[1], std::ios::binary);
  if (!output) throw std::runtime_error("cannot open cluster output");
  headunit::write_ppm(headunit::render_backup_cluster(sample, now), output);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
