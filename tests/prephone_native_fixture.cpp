// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Explicit simulated private process fixture, never a display/hardware owner.
#include <headunit/prephone_publication.hpp>
#include <headunit/prephone_indicator_publication.hpp>
#include <headunit/socket_vehicle.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/vehicle_observation_wire.hpp>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>

namespace {
volatile std::sig_atomic_t stopping = 0;
bool blocked_send = false;
bool future_copy = false;
bool blocked_indicators = false;
char malformed_indicator = 0;
headunit::PrephoneIndicatorPacket last_indicator{};
std::uint64_t published_packets = 0;
headunit::PrephonePacket last_sent{};
void stop(int) {
  stopping = 1;
}
}  // namespace
// Test-only syscall fault injection at the real production sender. No queue
// policy or deadline is changed; acquisitions and source acceptance continue.
extern "C" ssize_t __real_send(int, const void*, std::size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* data, std::size_t size, int flags) {
  if (blocked_send || (blocked_indicators && size == headunit::prephone_indicator_packet_size)) {
    errno = EAGAIN;
    return -1;
  }
  headunit::PrephonePacket invalid{};
  const void* sent_data = data;
  if (future_copy && size == invalid.size()) {
    std::memcpy(invalid.data(), data, invalid.size());
    std::uint64_t copied = 0;
    for (unsigned i = 0; i < 8; ++i)
      copied |= std::uint64_t(std::to_integer<unsigned>(invalid[48 + i])) << (i * 8);
    copied += 60'000'000'000;  // Explicit malformed-wire clock injection only.
    for (unsigned i = 0; i < 8; ++i)
      invalid[48 + i] = std::byte(copied >> (i * 8));
    sent_data = invalid.data();
  }
  std::array<std::byte, headunit::prephone_indicator_packet_size + 1> indicator_bytes{};
  auto sent_size = size;
  if (size == headunit::prephone_indicator_packet_size && malformed_indicator) {
    std::memcpy(indicator_bytes.data(), data, size);
    if (malformed_indicator == 'x') indicator_bytes[104 + 23 * 32 + 4] = std::byte{1};
    if (malformed_indicator == 'y') --sent_size;
    if (malformed_indicator == 'z') ++sent_size;
    if (malformed_indicator == 'j')
      std::memcpy(indicator_bytes.data(), last_indicator.data(), size);
    sent_data = indicator_bytes.data();
  }
  const auto sent = __real_send(fd, sent_data, sent_size, flags);
  if (sent == static_cast<ssize_t>(size) &&
      (size == headunit::prephone_packet_size || size == 2560 ||
       size == headunit::prephone_indicator_packet_size))
    ++published_packets;
  if (sent == static_cast<ssize_t>(headunit::prephone_packet_size))
    std::memcpy(last_sent.data(), data, last_sent.size());
  if (sent == static_cast<ssize_t>(headunit::prephone_indicator_packet_size))
    std::memcpy(last_indicator.data(), data, last_indicator.size());
  // The test changed wire length. Keep the real parent/socket live so the
  // receiver, rather than the sender's length check/HUP, rejects that record.
  if (size == headunit::prephone_indicator_packet_size &&
      (malformed_indicator == 'y' || malformed_indicator == 'z') &&
      sent == static_cast<ssize_t>(sent_size))
    return static_cast<ssize_t>(size);
  return sent;
}
int main(int argc, char** argv) try {
  if (argc < 3 || argc > 6) return 2;
  bool ambient_enabled = false;
  bool indicators_enabled = false;
  bool legacy = false;
  for (int i = 3; i < argc; ++i) {
    const std::string_view option(argv[i]);
    if (option == "--ambient" && !ambient_enabled)
      ambient_enabled = true;
    else if (option == "--prephone-indicators" && !indicators_enabled)
      indicators_enabled = true;
    else if (option == "--legacy" && !legacy)
      legacy = true;
    else
      return 2;
  }
  using namespace headunit;
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  std::signal(SIGHUP, stop);
  // Test-only real kernel parent-death retirement; no deployment policy change.
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() == 1) return 2;
  std::unique_ptr<VehicleSource> source;
  if (legacy)
    source = std::make_unique<SimulatedVehicle>(Clock::now());
  else
    source = std::make_unique<SocketVehicle>(argv[2]);
  PublishingVehicle vehicle(std::move(source), argv[1], ambient_enabled, indicators_enabled);
  VehicleAgePolicy ages;
  ages.readings.fill(std::chrono::seconds(5));
  ages.readings[2] = std::chrono::milliseconds(500);
  if (indicators_enabled) {
    ages.indicators.fill(std::chrono::seconds(10));
    ages.indicators[0] = std::chrono::milliseconds(500);
    ages.indicators[4] = std::chrono::milliseconds(500);
  }
  VehicleObservationCache producer(
      ages, ambient_enabled ? parse_drive_mode_catalog_key("comfort,sport") : std::nullopt);
  auto generation = std::uint64_t{1};
  auto sequence = std::uint64_t{0};
  const auto started = Clock::now();
  producer.begin_generation(generation, started, started);
  const int sender = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (sender < 0) return 2;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (std::strlen(argv[2]) >= sizeof(address.sun_path)) return 2;
  std::strcpy(address.sun_path, argv[2]);
  bool stalled = false;
  while (!stopping) {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if (poll(&input, 1, 10) > 0 && input.revents & POLLIN) {
      char command = 0;
      if (read(STDIN_FILENO, &command, 1) != 1) break;
      const auto now = Clock::now();
      if (command == 'i') {
        // Stop legitimate copies so a missing credential check cannot fail
        // accidentally on the parent's lower sequence after the child writes.
        stalled = true;
        const auto child = fork();
        if (child == 0) {
          for (int fd = 3; fd < 64; ++fd) {
            int type = 0;
            socklen_t size = sizeof(type);
            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_SEQPACKET) {
              // Genuine inherited nonce/model. Only sequence advances.
              auto packet = last_sent;
              for (std::size_t i = 40; i < 48; ++i) {
                const auto value = std::to_integer<unsigned>(packet[i]);
                packet[i] = std::byte(value + 1);
                if (value != 255) break;
              }
              __real_send(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            }
          }
          _exit(0);
        }
        if (child < 0) return 2;
        int status = 0;
        while (waitpid(child, &status, 0) < 0) {
          if (errno != EINTR) return 2;
        }
      }
      if (command == 'p') blocked_indicators = true;
      if (command == 'o') blocked_indicators = false;
      if (command == 'x' || command == 'y' || command == 'z' || command == 'j')
        malformed_indicator = command;
      if (command == 't') future_copy = true;
      if (command == 'b') blocked_send = true;
      if (command == 'a') blocked_send = false;
      if (command == 's') {
        stalled = true;
        std::cout << "stalled\n" << std::flush;
      }
      if (command == 'r') stalled = false;
      // Test-only progress barrier: count actual successful production sends,
      // without injecting a packet/date or changing the publication cadence.
      if (command == 'q') std::cout << "packets " << published_packets << '\n' << std::flush;
      if (command == 'n') {
        producer.begin_generation(++generation, now, now);
        sequence = 0;
      }
      if (command == 'f') {
        const std::array batch{
            VehicleObservation{"fuel_percent", 0.0, now, VehicleObservationOrigin::simulation},
            VehicleObservation{"ambient_c", -5.0, now, VehicleObservationOrigin::simulation},
            VehicleObservation{"range_km", 42.0, now, VehicleObservationOrigin::calculated}};
        producer.accept(generation, ++sequence, batch, now);
      }
      if (command == 'l') {
        const std::array lamps{VehicleObservation{"left_turn", IndicatorState::flashing, now,
                                                  VehicleObservationOrigin::simulation},
                               VehicleObservation{"right_turn", IndicatorState::off, now,
                                                  VehicleObservationOrigin::simulation},
                               VehicleObservation{"high_beam", IndicatorState::on, now,
                                                  VehicleObservationOrigin::simulation},
                               VehicleObservation{"low_beam", IndicatorState::off, now,
                                                  VehicleObservationOrigin::simulation},
                               VehicleObservation{"front_fog", std::monostate{}, now,
                                                  VehicleObservationOrigin::simulation,
                                                  VehicleAvailability::unavailable}};
        producer.accept(generation, ++sequence, lamps, now);
      }
      if (command == 'm') {
        const std::array batch{
            VehicleObservation{"drive_mode", *parse_drive_mode_identifier("sport"), now,
                               VehicleObservationOrigin::simulation},
            VehicleObservation{"low_beam", IndicatorState::on, now,
                               VehicleObservationOrigin::simulation}};
        producer.accept(generation, ++sequence, batch, now);
      }
      if (command == 'w') producer.withdraw(now);
      if (command == 'f' || command == 'w' || command == 'n' || command == 'm' || command == 'l' ||
          command == 'h') {
        const auto sample = producer.sample(now);
        const auto raw = encode_vehicle_observation_datagram(
            {sample, ++sequence, VehicleObservationWireVersion::hvo4});
        if (sendto(sender, raw.data(), raw.size(), 0, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != static_cast<ssize_t>(raw.size()))
          return 2;
        std::cout << "source " << generation << ' '
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(
                         sample.sampled_at.time_since_epoch())
                         .count()
                  << '\n'
                  << std::flush;
      }
    }
    if (!stalled) vehicle.sample(Clock::now());
  }
  close(sender);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
