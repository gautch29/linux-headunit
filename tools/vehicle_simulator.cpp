// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle.hpp>
#include <headunit/vehicle_wire.hpp>
#include <headunit/vehicle_observation_wire.hpp>
#include <headunit/sport_simulated_vehicle.hpp>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
int main(int argc, char** argv) try {
  if (argc != 3 && argc != 4)
    throw std::invalid_argument(
        "usage: headunit-vehicle-simulator SOCKET SECONDS [SNAPSHOT.huv2 | --sport]");
  unsigned seconds = 0;
  const std::string duration = argv[2];
  const auto [end, error] =
      std::from_chars(duration.data(), duration.data() + duration.size(), seconds);
  if (error != std::errc{} || end != duration.data() + duration.size() || seconds < 1 ||
      seconds > 3600)
    throw std::invalid_argument("simulation duration must be 1..3600 seconds");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path = argv[1];
  if (path.empty() || path.size() >= sizeof(address.sun_path))
    throw std::invalid_argument("invalid simulation socket path");
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) throw std::runtime_error("cannot create simulation socket");
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } close_socket{fd};
  const auto start = headunit::Clock::now();
  headunit::SimulatedVehicle vehicle(start);
  std::optional<headunit::VehicleSample> fixture;
  const bool sport = argc == 4 && std::string_view(argv[3]) == "--sport";
  if (argc == 4 && !sport) {
    std::ifstream input(argv[3]);
    if (!input) throw std::runtime_error("cannot open simulation fixture");
    std::string data(headunit::maximum_vehicle_datagram + 1, '\0');
    input.read(data.data(), data.size());
    data.resize(input.gcount());
    fixture = headunit::parse_vehicle_datagram(data, start);
    if (!fixture) throw std::invalid_argument("invalid simulation fixture");
  }
  unsigned delivered = 0, dropped = 0;
  while (headunit::Clock::now() - start < std::chrono::seconds(seconds)) {
    const auto now = headunit::Clock::now();
    auto sample = sport     ? headunit::sport_simulation_sample(start, now)
                  : fixture ? *fixture
                            : vehicle.sample(now);
    sample.sampled_at = now;
    sample.simulated = true;
    // HUV2 has no fields for typed transmission or drive modes.
    sample.transmission.reset();
    sample.drive_mode.reset();
    sample.drive_mode_catalog.reset();
    const auto text = sport ? headunit::encode_vehicle_observation_datagram(
                                  {sample, std::uint64_t(delivered) + dropped + 1,
                                   headunit::VehicleObservationWireVersion::hvo6})
                            : headunit::encode_vehicle_datagram(sample);
    const auto n = sendto(fd, text.data(), text.size(), 0,
                          reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (n == static_cast<ssize_t>(text.size()))
      ++delivered;
    else if (n < 0 && (errno == ENOENT || errno == ECONNREFUSED || errno == EAGAIN ||
                       errno == EWOULDBLOCK || errno == EINTR))
      ++dropped;
    else
      throw std::runtime_error("vehicle simulation send failed");
    std::this_thread::sleep_until(now + std::chrono::milliseconds(50));
  }
  std::cout << "SIMULATION ONLY: snapshots delivered=" << delivered << " dropped=" << dropped
            << '\n';
  return delivered ? 0 : 1;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
