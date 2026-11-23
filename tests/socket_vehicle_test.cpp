// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/socket_vehicle.hpp>
#include <headunit/prephone_vehicle_summary.hpp>
#include <headunit/vehicle.hpp>
#include <headunit/vehicle_observation.hpp>
#include <headunit/vehicle_observation_wire.hpp>

#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int private_socket() {
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  check(fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0, "cannot create private test socket");
  return fd;
}

void send_packet(int socket, const std::string& path, const char* payload,
                 bool allow_oversize_rejection = false) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (sendto(socket, payload, std::strlen(payload), 0, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0) {
    // Some Unix kernels reject this malformed oversized packet before it can
    // reach recv(). The codec separately checks the exact 8192/8193 boundary.
    if (allow_oversize_rejection && errno == EMSGSIZE &&
        std::strlen(payload) > headunit::maximum_vehicle_observation_datagram)
      return;
    throw std::runtime_error("could not send a vehicle sample (" +
                             std::to_string(std::strlen(payload)) +
                             " bytes): " + std::strerror(errno));
  }
}

int bind_endpoint(const std::string& path) {
  const int endpoint = private_socket();
  check(endpoint >= 0, "cannot create independent vehicle endpoint");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  check(bind(endpoint, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "cannot bind independent vehicle endpoint");
  check(chmod(path.c_str(), 0600) == 0, "cannot restrict independent vehicle endpoint");
  return endpoint;
}

struct Child {
  pid_t pid;
  ~Child() {
    if (pid > 0) {
      kill(pid, SIGKILL);
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
};
void typed_receiver_retirement(const std::string& path) {
  // Actual Unix ownership/death; all acquisition dates and readings below are simulated.
  using namespace headunit;
  using namespace std::chrono_literals;
  const auto at = [](std::int64_t ms) {
    return Clock::time_point{std::chrono::milliseconds(ms)};
  };
  const auto encode = [](VehicleSample value, std::uint64_t sequence) {
    return encode_vehicle_observation_datagram(
        {std::move(value), sequence, VehicleObservationWireVersion::hvo4});
  };
  VehicleAgePolicy ages;
  ages.readings[2] = 100ms;
  ages.readings[3] = 200ms;
  VehicleObservationCache producer(ages);
  producer.begin_generation(7, at(1000), at(1000));
  const std::array initial{
      VehicleObservation{"speed_kmh", 0., at(1000)},
      VehicleObservation{"fuel_percent", 42., at(1000), VehicleObservationOrigin::calculated},
      VehicleObservation{"coolant_c", 90., at(1000), VehicleObservationOrigin::simulation},
      VehicleObservation{"battery_voltage",
                         {},
                         at(1000),
                         VehicleObservationOrigin::measured,
                         VehicleAvailability::unavailable},
      VehicleObservation{"low_beam", IndicatorState::off, at(1000)}};
  producer.accept(7, 1, initial, at(1000));
  const auto original = encode(producer.sample(at(1005)), 1);
  int ready[2];
  int command[2];
  check(pipe(ready) == 0 && pipe(command) == 0, "typed retirement pipes");
  const auto pid = fork();
  check(pid >= 0, "typed retirement fork");
  if (pid == 0) {
    close(ready[0]);
    close(command[1]);
    try {
      SocketVehicle consumer(path);
      check(write(ready[1], "R", 1) == 1, "typed child readiness");
      char request = 0;
      check(read(command[0], &request, 1) == 1 && request == 'S', "typed child sample request");
      const auto admitted = consumer.sample(at(1010));
      check(admitted.speed_kmh == 0 && admitted.fuel_percent == 42 && admitted.coolant_c == 90 &&
                !admitted.battery_voltage &&
                admitted.indicator(Indicator::low_beam) == IndicatorState::off,
            "typed child did not admit complete mixed-field snapshot");
      const auto bytes = encode(admitted, 1);
      std::size_t written = 0;
      while (written < bytes.size()) {
        const auto count = write(ready[1], bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) continue;
        check(count > 0, "typed child retained snapshot write");
        written += static_cast<std::size_t>(count);
      }
      close(ready[1]);
      close(command[0]);
      for (;;)
        pause();
    } catch (...) {
      _exit(2);
    }
  }
  Child child{pid};
  close(ready[1]);
  close(command[0]);
  char bound = 0;
  check(read(ready[0], &bound, 1) == 1 && bound == 'R', "typed child endpoint not bound");
  const int sender = private_socket();
  const auto healthy_path = std::filesystem::path(path).parent_path().string() + "/healthy.sock";
  {
    SocketVehicle healthy(healthy_path);
    send_packet(sender, path, original.c_str());
    send_packet(sender, healthy_path, original.c_str());
    check(write(command[1], "S", 1) == 1, "typed child sample dispatch");
    close(command[1]);
    std::string retained_bytes;
    std::array<char, maximum_vehicle_observation_datagram> buffer{};
    for (;;) {
      const auto count = read(ready[0], buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      check(count >= 0, "typed admitted snapshot read");
      if (count == 0) break;
      retained_bytes.append(buffer.data(), static_cast<std::size_t>(count));
      check(retained_bytes.size() <= maximum_vehicle_observation_datagram,
            "typed admitted snapshot exceeded wire bound");
    }
    close(ready[0]);
    check(retained_bytes == original, "child receipt changed original source bytes");
    const auto retained = parse_vehicle_observation_datagram(retained_bytes, at(1010));
    check(retained.has_value(), "child retained snapshot was not valid HVO4");
    const auto peer = healthy.sample(at(1010));
    const std::array speed{VehicleObservation{"speed_kmh", 50., at(1020)}};
    producer.accept(7, 2, speed, at(1020));
    const auto queued = encode(producer.sample(at(1020)), 2);
    send_packet(sender, path, queued.c_str());
    check(kill(pid, SIGKILL) == 0, "typed child kill");
    int status = 0;
    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "typed child was not reaped after actual SIGKILL");
    child.pid = -1;
    const auto retired = prephone_vehicle_summary(retained->sample, {7, true}, at(1030));
    check(retired.state == VehicleSummaryState::disconnected && retired.source_generation == 7 &&
              retired.rows[0].acquired_at == at(1000) &&
              retired.rows[0].origin == VehicleObservationOrigin::calculated,
          "explicit retirement lost original retained source identity/date/provenance");
    for (const auto& row : retired.rows)
      check(!row.value, "fresh retained snapshot supplied prephone values after owner retirement");
    const auto foreign = prephone_vehicle_summary(retained->sample, {8}, at(1030));
    check(foreign.state == VehicleSummaryState::generation_mismatch && !foreign.source_generation,
          "old retained snapshot inherited successor admission");
    for (const auto& row : foreign.rows)
      check(!row.value && !row.acquired_at, "foreign generation supplied prephone readings/dates");
    check(access(path.c_str(), F_OK) == 0, "typed killed owner did not leave dead endpoint");
    {
      SocketVehicle successor(path);
      auto current = successor.sample(at(1030));
      check(!current.observation_timing && !current.speed_kmh && !current.fuel_percent &&
                !current.coolant_c && !current.battery_voltage,
            "replacement replayed admitted or kernel-queued old typed data");
      producer.begin_generation(8, at(1030), at(1030));
      const auto empty = encode(producer.sample(at(1030)), 1);
      send_packet(sender, path, empty.c_str());
      current = successor.sample(at(1031));
      check(current.observation_timing->generation == 8 && !current.speed_kmh &&
                !current.observation_timing->readings[2].acquired_at,
            "new empty generation inherited old measurement or acquisition");
      send_packet(sender, path, queued.c_str());
      current = successor.sample(at(1032));
      check(current.observation_timing->generation == 8 && current.sampled_at == at(1030) &&
                !current.speed_kmh && !current.fuel_percent,
            "late old-source packet revived retired generation");
      const std::array recovery{
          VehicleObservation{"speed_kmh", 0., at(1040)},
          VehicleObservation{"fuel_percent", 0., at(1041), VehicleObservationOrigin::calculated}};
      producer.accept(8, 1, recovery, at(1042));
      const auto recovered = producer.sample(at(1042));
      send_packet(sender, path, encode(recovered, 2).c_str());
      current = successor.sample(at(1043));
      check(current.speed_kmh == 0 && current.fuel_percent == 0 && !current.coolant_c &&
                !current.battery_voltage &&
                current.observation_timing->readings[2].acquired_at == at(1041),
            "successor confused known zero with unobserved fields or redated recovery");
      const auto model = prephone_vehicle_summary(current, {8}, at(1043));
      check(model.state == VehicleSummaryState::admitted && model.rows[0].value == 0 &&
                model.rows[0].acquired_at == at(1041) &&
                model.rows[4].availability == VehicleSummaryAvailability::missing &&
                !model.rows[4].value && !model.rows[4].acquired_at,
            "current prephone projection confused missing temperature with known zero fuel");
      const std::array advance{VehicleObservation{"speed_kmh", 12., at(1050)}};
      producer.accept(8, 2, advance, at(1050));
      const auto valid = producer.sample(at(1050));
      auto conflict = valid;
      conflict.fuel_percent = 1.;  // A later field conflicts at its unchanged acquisition.
      send_packet(sender, path, encode(conflict, 3).c_str());
      current = successor.sample(at(1051));
      check(current.speed_kmh == 0 && current.fuel_percent == 0 && current.sampled_at == at(1042) &&
                current.observation_timing->readings[0].acquired_at == at(1040),
            "later equal-date conflict partly committed earlier speed/date");
      send_packet(sender, path, encode(valid, 3).c_str());
      current = successor.sample(at(1052));
      check(current.speed_kmh == 12 && current.fuel_percent == 0 &&
                current.sampled_at == at(1050) &&
                current.observation_timing->readings[2].acquired_at == at(1041) &&
                current.observation_timing->readings[2].origin ==
                    VehicleObservationOrigin::calculated,
            "rejected batch consumed valid sequence or renewed unrelated field");
      check(successor.sample(at(1140)).fuel_percent == 0,
            "successor fuel expired before original TTL");
      current = successor.sample(at(1141));
      check(
          !current.fuel_percent && current.speed_kmh == 12 &&
              current.observation_timing->readings[2].availability == VehicleAvailability::stale &&
              current.observation_timing->readings[2].acquired_at == at(1041),
          "new speed, rejection or receipt renewed original fuel expiry");
    }
    auto current = healthy.sample(at(1099));
    check(current.speed_kmh == peer.speed_kmh && current.fuel_percent == 42 &&
              current.coolant_c == 90 && current.sampled_at == at(1005) &&
              current.observation_timing->generation == 7,
          "other endpoint retirement/replacement changed healthy source");
    current = healthy.sample(at(1100));
    check(!current.fuel_percent && current.coolant_c == 90 &&
              current.observation_timing->readings[2].acquired_at == at(1000),
          "healthy endpoint expiry changed during unrelated replacement");
    current = healthy.sample(at(1200));
    check(!current.coolant_c && current.simulated && !current.battery_voltage &&
              current.observation_timing->readings[3].origin ==
                  VehicleObservationOrigin::simulation &&
              current.observation_timing->readings[3].acquired_at == at(1000),
          "healthy expiry erased historical origin or manufactured missing value");
    check(retained->sample.speed_kmh == 0 && retained->sample.fuel_percent == 42 &&
              retained->sample.coolant_c == 90 && retained->sample.sampled_at == at(1005) &&
              retained->sample.observation_timing->generation == 7 &&
              !current_vehicle_sample(retained->sample, at(1200)).coolant_c &&
              !current_vehicle_sample(retained->sample, at(1500)).speed_kmh,
          "retirement mutated retained value-copy or retained copy bypassed original expiry");
  }
  close(sender);
  check(access(path.c_str(), F_OK) != 0 && access(healthy_path.c_str(), F_OK) != 0,
        "typed successor or healthy endpoint remained after destruction");
  check(unlink((healthy_path + ".lock").c_str()) == 0, "typed healthy lock fixture cleanup");
}
}  // namespace

int main() try {
  char directory[] = "/tmp/headunit-vehicle-XXXXXX";
  check(mkdtemp(directory) != nullptr, "could not make test directory");
  const std::string path = std::filesystem::canonical(directory).string() + "/input.sock";
  {
    headunit::SocketVehicle vehicle(path);
    struct stat info{};
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
          "socket must be accessible only to its owner");
    const int sender = private_socket();
    check(sender >= 0, "could not create test sender");
    const auto now = headunit::Clock::now();
    send_packet(sender, path, "HUV1 57.4 2400");
    auto value = vehicle.sample(now);
    check(value.speed_kmh == 57.4 && value.engine_rpm == 2400, "socket sample was not received");
    send_packet(sender, path, "HUV1 -10 1000");
    check(vehicle.sample(now).speed_kmh == 57.4, "invalid packet changed vehicle state");
    send_packet(sender, path, "HUV1 - 0");
    value = vehicle.sample(now);
    check(!value.speed_kmh && value.engine_rpm == 0, "missing speed must stay unavailable");
    check(!headunit::fresh(value, now + std::chrono::seconds(1), std::chrono::milliseconds(500)),
          "old vehicle data was not marked stale");
    send_packet(sender, path, "HUV2 fuel_percent=18 coolant_c=102 low_fuel=on left_turn=flash");
    value = vehicle.sample(now);
    check(value.fuel_percent == 18 && value.coolant_c == 102 && !value.speed_kmh &&
              value.indicator(headunit::Indicator::low_fuel) == headunit::IndicatorState::on &&
              value.indicator(headunit::Indicator::left_turn) == headunit::IndicatorState::flashing,
          "extended snapshot did not reach vehicle source");
    send_packet(sender, path, "HUV2 fuel_percent=22 coolant_c=999");
    check(vehicle.sample(now).fuel_percent == 18, "invalid extended snapshot partially applied");
    send_packet(sender, path, "HUV1 0 0");
    value = vehicle.sample(now);
    check(!value.fuel_percent && !value.indicator(headunit::Indicator::low_fuel),
          "old warning retained across complete snapshots");
    const auto queued = now + std::chrono::seconds(1);
    const auto stamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(queued.time_since_epoch()).count();
    const auto stamped = "HUF1 " + std::to_string(stamp) + " HUV2 speed_kmh=27 low_fuel=on";
    send_packet(sender, path, stamped.c_str());
    value = vehicle.sample(queued + std::chrono::seconds(2));
    check(value.speed_kmh == 27 && value.sampled_at == queued &&
              !headunit::fresh(value, queued + std::chrono::seconds(2),
                               std::chrono::milliseconds(500)),
          "consumer queue renewed an expired ingress timestamp");
    const auto replay = "HUF1 " + std::to_string(stamp - 1) + " HUV1 99 4000";
    send_packet(sender, path, replay.c_str());
    check(vehicle.sample(queued + std::chrono::seconds(2)).speed_kmh == 27,
          "older broker packet replaced newer input");
    close(sender);
  }
  check(access(path.c_str(), F_OK) != 0, "vehicle socket was not removed");
  {
    using namespace headunit;
    const auto at = [](std::int64_t ms) {
      return Clock::time_point{std::chrono::milliseconds(ms)};
    };
    VehicleAgePolicy ages;
    ages.readings[3] = std::chrono::milliseconds(100);
    ages.indicators[static_cast<std::size_t>(Indicator::check_engine)] =
        std::chrono::milliseconds(100);
    VehicleObservationCache producer(ages);
    producer.begin_generation(1, at(1000), at(1000));
    SocketVehicle consumer(path);
    const int sender = private_socket();
    check(!consumer.sample(at(1000)).observation_timing,
          "empty socket invented observation metadata");
    const std::array initial{VehicleObservation{"speed_kmh", 40., at(1000)},
                             VehicleObservation{"coolant_c", 90., at(1000)},
                             VehicleObservation{"check_engine", IndicatorState::on, at(1000)}};
    producer.accept(1, 1, initial, at(1000));
    const auto first = encode_vehicle_observation_datagram({producer.sample(at(1000)), 1});
    send_packet(sender, path, first.c_str());
    auto value = consumer.sample(at(1001));
    check(value.speed_kmh == 40 && value.coolant_c == 90 && value.sampled_at == at(1000) &&
              value.observation_timing->readings[3].acquired_at == at(1000),
          "socket renewed source publication/acquisition");
    const std::array speed{VehicleObservation{"speed_kmh", 50., at(1099)}};
    producer.accept(1, 2, speed, at(1099));
    const VehicleObservationPacket second{producer.sample(at(1099)), 2};
    const auto bytes = encode_vehicle_observation_datagram(second);
    send_packet(sender, path, bytes.c_str());
    value = consumer.sample(at(1100));
    check(value.speed_kmh == 50 && !value.coolant_c && !value.indicator(Indicator::check_engine) &&
              value.observation_timing->readings[3].availability == VehicleAvailability::stale,
          "socket fresh speed renewed stale temperature or warning");
    send_packet(sender, path, first.c_str());
    send_packet(sender, path, "HUV1 99 4000");
    value = consumer.sample(at(1120));
    check(value.speed_kmh == 50 && value.sampled_at == at(1099),
          "wire replay or format downgrade changed observations");
    producer.withdraw(at(1121));
    const auto lost = encode_vehicle_observation_datagram({producer.sample(at(1121)), 3});
    send_packet(sender, path, lost.c_str());
    check(!consumer.sample(at(1121)).speed_kmh, "source withdrawal did not cross socket");
    auto revival = second;
    revival.sequence = 4;
    revival.sample.sampled_at = at(1122);
    // The source's older coolant/lamp now really are expired; retain their
    // old dates but try to revive only the withdrawn, still-young speed value.
    revival.sample = current_vehicle_sample(revival.sample, at(1122));
    const auto forged = encode_vehicle_observation_datagram(revival);
    send_packet(sender, path, forged.c_str());
    check(!consumer.sample(at(1122)).speed_kmh, "same-date source value revived after withdrawal");
    const std::array recovered{VehicleObservation{"speed_kmh", 55., at(1110)}};
    producer.accept(1, 3, recovered, at(1123));
    const auto recovery = encode_vehicle_observation_datagram({producer.sample(at(1123)), 4});
    send_packet(sender, path, recovery.c_str());
    value = consumer.sample(at(1140));
    check(value.speed_kmh == 55 && value.observation_timing->readings[0].acquired_at == at(1110),
          "delayed socket observation failed recovery or renewed date");
    producer.begin_generation(2, at(1130), at(1131));
    const std::array zero{VehicleObservation{"speed_kmh", 0., at(1131)}};
    producer.accept(2, 1, zero, at(1131));
    const auto restarted = encode_vehicle_observation_datagram({producer.sample(at(1131)), 1});
    send_packet(sender, path, restarted.c_str());
    value = consumer.sample(at(1141));
    check(value.speed_kmh == 0 && value.observation_timing->generation == 2 &&
              value.sampled_at == at(1131),
          "restart begun before a consumer poll rejected a fresh delayed generation");
    send_packet(sender, path, recovery.c_str());
    const auto oversized =
        std::string("HVO1 ") + std::string(maximum_vehicle_observation_datagram, 'x');
    send_packet(sender, path, oversized.c_str(), true);
    value = consumer.sample(at(1142));
    check(value.speed_kmh == 0 && value.observation_timing->generation == 2,
          "old generation or oversized snapshot changed source");
    bool reversed = false;
    try {
      consumer.sample(at(1141));
    } catch (const std::invalid_argument&) {
      reversed = true;
    }
    check(reversed, "socket consumer accepted reversed host clock");
    close(sender);
  }
  {
    // Standard preview opens its endpoint on the control thread, then gives
    // exclusive sampling to its painter. Ownership starts at first sample.
    using namespace headunit;
    SocketVehicle consumer(path);
    const int sender = private_socket();
    const auto now = Clock::now();
    VehicleObservationCache producer;
    producer.begin_generation(1, now, now);
    const std::array batch{VehicleObservation{"speed_kmh", 28., now}};
    producer.accept(1, 1, batch, now);
    const auto packet = encode_vehicle_observation_datagram({producer.sample(now), 1});
    send_packet(sender, path, packet.c_str());
    std::exception_ptr error;
    std::thread painter([&] {
      try {
        const auto value = consumer.sample(now);
        check(value.speed_kmh == 28 && value.sampled_at == now,
              "endpoint construction prevented first sampling-thread handoff");
        check(consumer.sample(now).speed_kmh == 28, "sampling owner could not poll again");
      } catch (...) {
        error = std::current_exception();
      }
    });
    painter.join();
    close(sender);
    if (error) std::rethrow_exception(error);
    bool wrong_owner = false;
    try {
      consumer.sample(now);
    } catch (const std::invalid_argument&) {
      wrong_owner = true;
    }
    check(wrong_owner, "initialized vehicle socket accepted a second sampling owner");
  }
  // A killed receiver does not run its destructor. Recover only that owned dead
  // endpoint; its old kernel queue and source values must not be replayed.
  int ready[2];
  check(pipe(ready) == 0, "cannot create child readiness pipe");
  const auto pid = fork();
  check(pid >= 0, "cannot fork vehicle consumer");
  if (pid == 0) {
    close(ready[0]);
    try {
      headunit::SocketVehicle consumer(path);
      if (write(ready[1], "R", 1) != 1) _exit(2);
      close(ready[1]);
      for (;;)
        pause();
    } catch (...) {
      _exit(2);
    }
  }
  Child child{pid};
  close(ready[1]);
  char started = 0;
  const auto status = read(ready[0], &started, 1);
  close(ready[0]);
  check(status == 1 && started == 'R', "killed-consumer fixture did not bind");
  const int sender = private_socket();
  check(sender >= 0, "cannot create restart fixture sender");
  send_packet(sender, path, "HUV1 99 4000");
  check(kill(pid, SIGKILL) == 0, "could not kill vehicle consumer");
  int exit_status = 0;
  check(waitpid(pid, &exit_status, 0) == pid && WIFSIGNALED(exit_status),
        "vehicle child did not exit");
  child.pid = -1;
  check(access(path.c_str(), F_OK) == 0, "killed consumer did not leave its endpoint fixture");
  {
    headunit::SocketVehicle recovered(path);
    const auto now = headunit::Clock::now();
    check(!recovered.sample(now).speed_kmh, "stale killed-consumer data replayed after restart");
    send_packet(sender, path, "HUV1 18 1000");
    check(recovered.sample(headunit::Clock::now()).speed_kmh == 18,
          "recovered consumer cannot receive new input");
    bool rejected = false;
    try {
      headunit::SocketVehicle duplicate(path);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "duplicate vehicle consumer took an active lock");
  }
  close(sender);
  // Older/live endpoints that do not use our advisory lock remain protected.
  const int live = bind_endpoint(path);
  bool rejected = false;
  try {
    headunit::SocketVehicle duplicate(path);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  check(rejected && access(path.c_str(), F_OK) == 0, "live independent input was reclaimed");
  close(live);
  {
    headunit::SocketVehicle recovered(path);
    const auto moved = std::string(directory) + "/moved.sock";
    check(rename(path.c_str(), moved.c_str()) == 0, "cannot move replacement fixture");
    const int replacement = bind_endpoint(path);
    close(replacement);
  }
  check(access(path.c_str(), F_OK) == 0, "consumer cleanup removed a replacement endpoint");
  check(unlink(path.c_str()) == 0 && unlink((std::string(directory) + "/moved.sock").c_str()) == 0,
        "cannot remove replacement fixture");
  const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  check(file >= 0, "cannot make unsafe path fixture");
  close(file);
  rejected = false;
  try {
    headunit::SocketVehicle unsafe(path);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  check(rejected && access(path.c_str(), F_OK) == 0,
        "ordinary file was removed during endpoint recovery");
  check(unlink(path.c_str()) == 0, "cannot remove unsafe path fixture");
  const int unsafe = bind_endpoint(path);
  check(chmod(path.c_str(), 0666) == 0, "cannot change unsafe endpoint mode");
  rejected = false;
  try {
    headunit::SocketVehicle input(path);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  check(rejected && access(path.c_str(), F_OK) == 0,
        "unsafe consumer permissions were accepted or removed");
  close(unsafe);
  check(unlink(path.c_str()) == 0, "cannot remove unsafe endpoint fixture");
  check(symlink((path + ".lock").c_str(), path.c_str()) == 0,
        "cannot create symlink endpoint fixture");
  rejected = false;
  try {
    headunit::SocketVehicle input(path);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  struct stat alias{};
  check(rejected && lstat(path.c_str(), &alias) == 0 && S_ISLNK(alias.st_mode),
        "symlink endpoint was accepted or removed");
  check(unlink(path.c_str()) == 0, "cannot remove symlink fixture");
  check(chmod(directory, 0755) == 0, "cannot change parent fixture mode");
  rejected = false;
  try {
    headunit::SocketVehicle input(path);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "public vehicle socket parent was accepted");
  check(chmod(directory, 0700) == 0, "cannot restore private parent fixture");
  typed_receiver_retirement(path);
  check(unlink((path + ".lock").c_str()) == 0, "vehicle lock fixture was not removed");
  check(rmdir(directory) == 0, "test directory was not removed");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
