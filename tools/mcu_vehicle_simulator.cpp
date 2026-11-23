// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/socket_vehicle.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/vehicle_observation_wire.hpp>
#include <headunit/sport_simulated_vehicle.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
using namespace headunit;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Sender {
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  Sender() {
    check(fd >= 0, "cannot open simulator socket");
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      close(fd);
      fd = -1;
      throw std::runtime_error("cannot restrict simulator socket");
    }
  }
  void disconnect() {
    if (fd >= 0) close(fd);
    fd = -1;
  }
  ~Sender() { disconnect(); }
  void send(const std::string& path, const std::string& bytes) const {
    sockaddr_un address{};
    check(path.size() < sizeof(address.sun_path), "simulator socket path is too long");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto result = sendto(fd, bytes.data(), bytes.size(), 0,
                               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result != static_cast<ssize_t>(bytes.size())) {
      throw std::runtime_error(std::string("simulator datagram was not delivered: ") +
                               (result < 0 ? std::strerror(errno) : "short send"));
    }
  }
};

struct Directory {
  std::string path;
  Directory() {
    char name[] = "/tmp/headunit-mcu-XXXXXX";
    check(mkdtemp(name) != nullptr, "cannot create private simulator directory");
    path = std::filesystem::canonical(name).string();
  }
  ~Directory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

struct Expected {
  std::optional<double> speed, rpm, fuel, coolant, oil, ambient;
  std::optional<TransmissionState> transmission;
  std::optional<DriveModeIdentifier> mode;
  std::optional<IndicatorState> beam, turn;
  std::uint64_t generation = 1;
  std::chrono::milliseconds speed_acquisition{10};
  VehicleAvailability speed_availability = VehicleAvailability::available;
  std::chrono::milliseconds publication{10};
};

struct Event {
  std::chrono::milliseconds elapsed;
  const char* label;
  std::vector<std::string> packets;
  Expected expected;
  bool disconnect = false;
};

// The timeline is a synthetic, host-clock acquisition fixture, NOT the MCU
// serial protocol or a clock synchronizer. Every recorded origin is simulation.
std::vector<Event> scenario(Clock::time_point base) {
  const auto at = [base](int ms) {
    return base + std::chrono::milliseconds(ms);
  };
  VehicleAgePolicy ages;
  ages.readings[0] = 100ms;
  ages.readings[1] = 100ms;
  for (const auto index : {2, 3, 4, 7})
    ages.readings[index] = 2000ms;
  ages.indicators.fill(100ms);
  ages.transmission = 100ms;
  ages.drive_mode = 100ms;
  const auto catalog = *parse_drive_mode_catalog_key("normal,sport");
  const auto mode = catalog.identifiers[0];
  const TransmissionState gear{GearboxType::automatic, TransmissionSelector::drive,
                               EngagedGear{EngagedGearKind::forward, 3}};
  VehicleObservationCache producer(ages, catalog);
  producer.begin_generation(1, base, base);
  std::vector<Event> events;
  const auto packet = [&](int ms, std::uint64_t sequence) {
    return VehicleObservationPacket{producer.sample(at(ms)), sequence,
                                    VehicleObservationWireVersion::hvo4};
  };
  const auto batch = [&](int ms, std::uint64_t generation, std::uint64_t sequence,
                         std::vector<VehicleObservation> observations) {
    for (auto& observation : observations)
      observation.origin = VehicleObservationOrigin::simulation;
    producer.accept(generation, sequence, observations, at(ms));
  };
  batch(10, 1, 1,
        {{"speed_kmh", 42., at(10)},
         {"engine_rpm", 2200., at(10)},
         {"fuel_percent", 25., at(10)},
         {"coolant_c", 90., at(10)},
         {"oil_c", 95., at(10)},
         {"ambient_c", -2., at(10)},
         {"transmission", gear, at(10)},
         {"drive_mode", mode, at(10)},
         {"low_beam", IndicatorState::off, at(10)},
         {"left_turn", IndicatorState::flashing, at(10)}});
  const auto first = encode_vehicle_observation_datagram(packet(10, 1));
  Expected expected{
      42., 2200., 25., 90., 95., -2., gear, mode, IndicatorState::off, IndicatorState::flashing};
  events.push_back(
      {10ms, "all typed inputs, known off and negative temperature", {first}, expected});
  batch(60, 1, 2, {{"speed_kmh", 43., at(60)}, {"engine_rpm", 2300., at(60)}});
  auto second = packet(60, 2);
  expected.speed = 43.;
  expected.rpm = 2300.;
  expected.speed_acquisition = 60ms;
  expected.publication = 60ms;
  events.push_back({60ms,
                    "fast update preserves slow acquisitions",
                    {encode_vehicle_observation_datagram(second)},
                    expected});
  events.push_back({70ms,
                    "reordered and duplicate snapshots rejected",
                    {first, encode_vehicle_observation_datagram(second)},
                    expected});
  auto conflict = second;
  conflict.sequence = 3;
  conflict.sample.sampled_at = at(80);
  conflict.sample.fuel_percent = 99.;
  conflict.sample.speed_kmh = 99.;
  conflict.sample.observation_timing->readings[0].acquired_at = at(80);
  events.push_back({80ms,
                    "equal-date conflict rejects entire batch",
                    {encode_vehicle_observation_datagram(conflict)},
                    expected});
  expected.publication = 81ms;
  events.push_back({81ms,
                    "rejected packet does not consume snapshot sequence",
                    {encode_vehicle_observation_datagram(packet(81, 3))},
                    expected});
  auto changed_policy = packet(82, 4);
  changed_policy.sample.observation_timing->readings[2].maximum_age = 3000ms;
  events.push_back({82ms,
                    "age policy cannot be enlarged",
                    {encode_vehicle_observation_datagram(changed_policy)},
                    expected});
  expected.transmission.reset();
  expected.mode.reset();
  expected.beam.reset();
  expected.turn.reset();
  events.push_back({110ms, "selector mode and lamps expire independently", {}, expected});
  const auto heartbeat = encode_vehicle_observation_datagram(packet(150, 4));
  expected.speed.reset();
  expected.rpm.reset();
  expected.speed_availability = VehicleAvailability::stale;
  expected.publication = 150ms;
  events.push_back(
      {160ms, "queued heartbeat cannot renew acquisition at exact expiry", {heartbeat}, expected});
  expected.fuel.reset();
  expected.coolant.reset();
  expected.oil.reset();
  expected.ambient.reset();
  events.push_back({2010ms, "slow inputs reach exact expiry", {}, expected});
  batch(2011, 1, 3,
        {{"speed_kmh", 44., at(2011)},
         {"engine_rpm", 2400., at(2011)},
         {"fuel_percent", 24., at(2011)},
         {"coolant_c", 91., at(2011)},
         {"oil_c", 96., at(2011)},
         {"ambient_c", -1., at(2011)},
         {"transmission", gear, at(2011)},
         {"drive_mode", mode, at(2011)},
         {"low_beam", IndicatorState::on, at(2011)},
         {"left_turn", IndicatorState::off, at(2011)}});
  const auto recovery = packet(2011, 5);
  expected = {44., 2400., 24., 91., 96., -1., gear, mode, IndicatorState::on, IndicatorState::off,
              1,   2011ms};
  expected.publication = 2011ms;
  events.push_back({2015ms,
                    "delayed fresh acquisition recovers without redating",
                    {encode_vehicle_observation_datagram(recovery)},
                    expected});
  batch(2020, 1, 4,
        {{"fuel_percent", std::monostate{}, at(2020), VehicleObservationOrigin::simulation,
          VehicleAvailability::unavailable}});
  expected.fuel.reset();
  expected.publication = 2020ms;
  const auto before_withdrawal = packet(2020, 6);
  events.push_back({2020ms,
                    "explicit sensor absence is not zero",
                    {encode_vehicle_observation_datagram(before_withdrawal)},
                    expected});
  producer.withdraw(at(2021));
  expected = {};
  expected.speed_acquisition = 2011ms;
  expected.speed_availability = VehicleAvailability::disconnected;
  expected.publication = 2021ms;
  events.push_back({2021ms,
                    "explicit link withdrawal preserves dates",
                    {encode_vehicle_observation_datagram(packet(2021, 7))},
                    expected});
  auto revival = before_withdrawal;
  revival.sequence = 8;
  revival.sample.sampled_at = at(2022);
  events.push_back({2022ms,
                    "withdrawn acquisitions cannot revive at a new sequence",
                    {encode_vehicle_observation_datagram(revival)},
                    expected});
  batch(2023, 1, 5, {{"speed_kmh", 45., at(2022)}});
  const auto newer = encode_vehicle_observation_datagram(packet(2023, 8));
  expected.speed = 45.;
  expected.speed_acquisition = 2022ms;
  expected.speed_availability = VehicleAvailability::available;
  expected.publication = 2023ms;
  events.push_back({2023ms, "strictly newer delayed acquisition recovers", {newer}, expected});
  producer.begin_generation(2, at(2023), at(2024));
  batch(2024, 2, 1,
        {{"speed_kmh", 0., at(2024)},
         {"engine_rpm", 0., at(2024)},
         {"low_beam", IndicatorState::off, at(2024)}});
  expected = {};
  expected.speed = 0.;
  expected.rpm = 0.;
  expected.beam = IndicatorState::off;
  expected.generation = 2;
  expected.speed_acquisition = 2024ms;
  expected.publication = 2024ms;
  events.push_back({2024ms,
                    "restart permits known zero without guessing missing gear",
                    {encode_vehicle_observation_datagram(packet(2024, 1))},
                    expected});
  events.push_back({2025ms, "retired generation rejected", {newer}, expected, true});
  expected.speed.reset();
  expected.rpm.reset();
  expected.beam.reset();
  expected.speed_availability = VehicleAvailability::stale;
  events.push_back({2124ms, "silent sender death expires rather than reports zero", {}, expected});
  return events;
}

void verify(const VehicleSample& sample, const Expected& expected, Clock::time_point base) {
  check(sample.speed_kmh == expected.speed && sample.engine_rpm == expected.rpm &&
            sample.fuel_percent == expected.fuel && sample.coolant_c == expected.coolant &&
            sample.oil_c == expected.oil && sample.ambient_c == expected.ambient &&
            sample.transmission == expected.transmission && sample.drive_mode == expected.mode &&
            sample.indicator(Indicator::low_beam) == expected.beam &&
            sample.indicator(Indicator::left_turn) == expected.turn,
        "unexpected reading, absence, transmission, mode or lamp");
  check(!sample.range_km && !sample.odometer_km && !sample.oil_pressure_kpa &&
            !sample.battery_voltage && !sample.trip && !sample.heading,
        "unfitted input became available");
  check(sample.observation_timing && sample.simulated &&
            sample.sampled_at == base + expected.publication &&
            sample.observation_timing->generation == expected.generation &&
            sample.drive_mode_catalog == parse_drive_mode_catalog_key("normal,sport"),
        "generation, capability or simulation provenance lost");
  check(sample.observation_timing->readings[2].maximum_age == 2000ms, "slow age policy changed");
  const auto& speed = sample.observation_timing->readings[0];
  check(speed.acquired_at == base + expected.speed_acquisition &&
            speed.availability == expected.speed_availability && speed.maximum_age == 100ms &&
            speed.origin == VehicleObservationOrigin::simulation,
        "acquisition date, availability, age policy or provenance changed");
  if (expected.fuel) {
    const auto acquisition = expected.speed_acquisition < 2000ms ? 10ms : 2011ms;
    check(sample.observation_timing->readings[2].acquired_at == base + acquisition,
          "fast update renewed fuel acquisition");
  }
  for (const auto index : {3, 4, 7}) {
    if (sample.*reading_definitions[index].member) {
      const auto acquisition = expected.speed_acquisition < 2000ms ? 10ms : 2011ms;
      check(sample.observation_timing->readings[index].acquired_at == base + acquisition &&
                sample.observation_timing->readings[index].origin ==
                    VehicleObservationOrigin::simulation,
            "temperature acquisition or provenance changed");
    }
  }
  if (expected.transmission) {
    const auto acquisition = expected.speed_acquisition < 2000ms ? 10ms : 2011ms;
    check(sample.observation_timing->transmission.acquired_at == base + acquisition &&
              sample.observation_timing->drive_mode.acquired_at == base + acquisition,
          "selector or mode acquisition changed");
  }
}

std::size_t run(const std::string& path, SocketVehicle* consumer) {
  const auto base = consumer ? Clock::time_point{1s} : Clock::now() + 20ms;
  const auto events = scenario(base);
  Sender sender;
  if (consumer) check(!consumer->sample(base).speed_kmh, "startup supplied a fabricated zero");
  for (const auto& event : events) {
    if (!consumer) std::this_thread::sleep_until(base + event.elapsed);
    for (const auto& bytes : event.packets)
      sender.send(path, bytes);
    if (event.disconnect) sender.disconnect();
    if (consumer) {
      try {
        verify(consumer->sample(base + event.elapsed), event.expected, base);
      } catch (const std::exception& error) {
        throw std::runtime_error(std::string(event.label) + ": " + error.what());
      }
    }
    std::cout << event.elapsed.count() << " ms: " << event.label << '\n';
  }
  return events.size();
}
void run_sport(const std::string& path, SocketVehicle* consumer) {
  const auto base = consumer ? Clock::time_point{1s} : Clock::now();
  Sender sender;
  for (int index = 0; index <= 500; ++index) {
    const auto now = base + 50ms * index;
    if (!consumer) std::this_thread::sleep_until(now);
    const auto sample = sport_simulation_sample(base, now);
    sender.send(path, encode_vehicle_observation_datagram(
                          {sample, std::uint64_t(index + 1), VehicleObservationWireVersion::hvo6}));
    if (consumer) {
      const auto received = consumer->sample(now);
      check(received.speed_kmh == sample.speed_kmh &&
                received.acceleration == sample.acceleration &&
                received.gps_position == sample.gps_position && received.simulated &&
                received.observation_timing->acceleration.acquired_at == now &&
                received.observation_timing->gps_position.acquired_at == now,
            "sport simulator lost typed acquisition or provenance");
    }
  }
  if (consumer) {
    const auto stale = consumer->sample(base + 25500ms);
    check(!stale.speed_kmh && !stale.acceleration && !stale.gps_position,
          "sport simulator silence did not expire at the original age");
  }
  std::cout << "SIMULATION ONLY: 501 sport HVO6 acquisitions; no hardware qualification\n";
}
}  // namespace

int main(int argc, char** argv) try {
  check(argc == 2 || argc == 3,
        "usage: headunit-mcu-vehicle-simulator --self-test | --sport-self-test | --socket "
        "PRIVATE_SOCKET | --sport-socket PRIVATE_SOCKET");
  if (argc == 2 && std::string_view(argv[1]) == "--sport-self-test") {
    Directory directory;
    const auto path = directory.path + "/sport.sock";
    {
      SocketVehicle consumer(path);
      run_sport(path, &consumer);
    }
    check(!std::filesystem::exists(path), "sport endpoint survived retirement");
  } else if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
    Directory directory;
    const auto path = directory.path + "/vehicle.sock";
    std::size_t checkpoints = 0;
    {
      SocketVehicle consumer(path);
      checkpoints = run(path, &consumer);
    }
    check(!std::filesystem::exists(path), "consumer endpoint survived retirement");
    std::cout << "SIMULATION ONLY: " << checkpoints
              << " socket checkpoints passed; endpoint retired\n";
  } else if (argc == 3 && (std::string_view(argv[1]) == "--socket" ||
                           std::string_view(argv[1]) == "--sport-socket")) {
    const std::filesystem::path path(argv[2]);
    struct stat directory{}, endpoint{};
    check(path.is_absolute() && path.lexically_normal() == path &&
              std::filesystem::canonical(path.parent_path()) == path.parent_path() &&
              lstat(path.parent_path().c_str(), &directory) == 0 && S_ISDIR(directory.st_mode) &&
              directory.st_uid == getuid() && !(directory.st_mode & 0077) &&
              lstat(path.c_str(), &endpoint) == 0 && S_ISSOCK(endpoint.st_mode) &&
              endpoint.st_uid == getuid() && !(endpoint.st_mode & 0077),
          "simulator needs an owned socket in an exact private directory");
    std::cout << "SIMULATION ONLY: use an isolated consumer; no hardware or actuator I/O\n";
    if (std::string_view(argv[1]) == "--sport-socket")
      run_sport(path.string(), nullptr);
    else
      run(path.string(), nullptr);
  } else {
    throw std::invalid_argument(
        "usage: headunit-mcu-vehicle-simulator --self-test | --sport-self-test | --socket "
        "PRIVATE_SOCKET | --sport-socket PRIVATE_SOCKET");
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
