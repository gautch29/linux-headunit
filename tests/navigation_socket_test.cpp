// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/navigation_socket.hpp>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace headunit;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
std::uint64_t ns(Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}
NavigationSnapshot navigation(Clock::time_point at) {
  NavigationSnapshot n;
  n.available = true;
  n.route_state = 1;
  n.source = "Apple Maps";
  n.current_road = "Rue des Lilas";
  n.destination = "Home";
  n.distance_m = 300;
  n.remaining_m = 4000;
  n.remaining_seconds = 420;
  n.arrival_unix_seconds = 1800000000;
  n.maneuver = NavigationManeuver{7, 2, "Turn right onto Rue des Lilas", "Rue des Lilas", -90};
  n.updated_at = at;
  return n;
}
void send(const std::string& path, std::span<const std::uint8_t> bytes) {
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  check(fd >= 0, "test socket unavailable");
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  const auto count =
      sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
  const auto error = errno;
  close(fd);
  if (count < 0 && error == EMSGSIZE && bytes.size() > maximum_navigation_datagram) return;
  check(count >= 0 && std::size_t(count) == bytes.size(), "test datagram send failed");
}
ControlMessage route() {
  return {0x5201, encode_iap2_parameters(std::array<Iap2Parameter, 4>{
                      {{0, {0, 42}}, {1, {1}}, {10, {0, 0, 1, 44}}, {13, {0, 7}}}})};
}
}  // namespace
int main() try {
  const auto at = Clock::now() + 1s;
  auto nav = navigation(at);
  const NavigationPublication p{ns(at - 1s), 7, at, nav};
  const auto wire = encode_navigation_datagram(p);
  auto parsed = parse_navigation_datagram(wire, at + 1ms);
  check(parsed && parsed->navigation.updated_at == at &&
            parsed->navigation.maneuver->exit_angle == -90 &&
            parsed->navigation.destination == "Home",
        "exact complete packet failed roundtrip");
  check(!parse_navigation_datagram(wire, at - 1ns) && !parse_navigation_datagram(wire, at + 2s),
        "future or exactly expired heartbeat admitted");
  check(parse_navigation_datagram(wire, at + 2s - 1ns).has_value(), "lease expired early");
  for (std::size_t length = 0; length < wire.size(); ++length)
    check(!parse_navigation_datagram(std::span(wire.data(), length), at),
          "truncated packet admitted");
  auto malformed = wire;
  malformed.push_back(0);
  check(!parse_navigation_datagram(malformed, at), "trailing bytes admitted");
  malformed = wire;
  malformed[28] = 2;
  check(!parse_navigation_datagram(malformed, at), "unknown availability flag admitted");
  malformed = wire;
  malformed[0] = 'X';
  check(!parse_navigation_datagram(malformed, at), "unknown version admitted");
  malformed.assign(maximum_navigation_datagram + 1, 0);
  check(!parse_navigation_datagram(malformed, at), "oversized packet admitted");
  auto maximum = p;
  maximum.navigation.source = std::string(256, 'S');
  maximum.navigation.current_road = std::string(256, 'R');
  maximum.navigation.destination = std::string(256, 'D');
  maximum.navigation.maneuver->instruction = std::string(256, 'I');
  maximum.navigation.maneuver->road = std::string(256, 'M');
  check(parse_navigation_datagram(encode_navigation_datagram(maximum), at).has_value(),
        "maximum text snapshot rejected");
  bool rejected = false;
  maximum.navigation.source = std::string(257, 'S');
  try {
    encode_navigation_datagram(maximum);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "oversized text encoded");
  rejected = false;
  maximum = p;
  maximum.navigation.source = "\xc0\xaf";
  try {
    encode_navigation_datagram(maximum);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid UTF-8 encoded");
  rejected = false;
  maximum = p;
  maximum.navigation.updated_at = at + 1ms;
  try {
    encode_navigation_datagram(maximum);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "future source date encoded");
  {
    RouteGuidance model({});
    check(model.receive(route(), at), "source route rejected");
    const auto received = model.snapshot(at);
    check(received.updated_at == at && model.snapshot(at + 1h).updated_at == at,
          "polling renewed original source-change date");
    const auto maneuver = ControlMessage{
        0x5202, encode_iap2_parameters(
                    std::array<Iap2Parameter, 3>{{{0, {0, 42}}, {1, {0, 7}}, {3, {2}}}})};
    check(model.receive(maneuver, at + 1s) && !model.snapshot(at + 500ms).available &&
              model.snapshot(at + 1s).updated_at == at + 1s,
          "maneuver admission date missing or future-dated source visible");
    auto broken = route();
    broken.parameters.push_back(0);
    check(!model.receive(broken, at + 2s) && model.snapshot(at + 2s).updated_at == at + 1s,
          "malformed input refreshed source-change date");
    model.clear();
    check(!model.snapshot(at + 3s).available && !model.snapshot(at + 3s).updated_at,
          "disconnect retained source date");
  }
  char directory[] = "/tmp/headunit-navigation-XXXXXX";
  check(mkdtemp(directory) != nullptr, "test directory unavailable");
  const auto root = std::filesystem::canonical(directory);
  const auto path = (root / "input.sock").string();
  {
    SocketNavigation input(path);
    check(!input.sample(at).available, "empty socket invented navigation");
    struct stat st{};
    check(lstat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "endpoint is not private");
    bool duplicate = false;
    try {
      SocketNavigation other(path);
    } catch (const std::runtime_error&) {
      duplicate = true;
    }
    check(duplicate, "second endpoint owner admitted");
    send(path, wire);
    check(input.sample(at).maneuver->instruction == nav.maneuver->instruction,
          "valid datagram not received");
    auto heartbeat = p;
    heartbeat.sent_at = at + 1s;
    send(path, encode_navigation_datagram(heartbeat));
    check(input.sample(at + 1s).updated_at == at && input.sample(at + 2500ms).available,
          "stationary heartbeat renewed source date or lost live route");
    auto rewrite = heartbeat;
    rewrite.sent_at = at + 1500ms;
    rewrite.navigation.distance_m = 1;
    send(path, encode_navigation_datagram(rewrite));
    check(input.sample(at + 1500ms + 1s).distance_m == 300, "same-date payload rewrite admitted");
    send(path, malformed);
    check(!input.sample(at + 3s).available, "malformed input renewed owner lease");
    // Lease expiry is not source withdrawal: a healthy owner's unchanged route
    // can recover with a new heartbeat, still carrying its original change date.
    heartbeat.sent_at = at + 3s;
    send(path, encode_navigation_datagram(heartbeat));
    check(input.sample(at + 3s).available, "healthy stationary route failed recovery");
    auto withdrawn = heartbeat;
    withdrawn.sent_at = at + 3250ms;
    withdrawn.navigation = {};
    send(path, encode_navigation_datagram(withdrawn));
    check(!input.sample(at + 3250ms).available, "disconnect withdrawal ignored");
    heartbeat.sent_at = at + 3500ms;
    send(path, encode_navigation_datagram(heartbeat));
    check(!input.sample(at + 3500ms).available,
          "same-date route revived after explicit withdrawal");
    heartbeat.sent_at = at + 3750ms;
    heartbeat.navigation.updated_at = at + 3600ms;
    send(path, encode_navigation_datagram(heartbeat));
    check(input.sample(at + 3750ms).available, "new source update failed recovery");
    auto replay = heartbeat;
    replay.sent_at = at + 4s;
    replay.session_generation = 6;
    send(path, encode_navigation_datagram(replay));
    check(input.sample(at + 4s).updated_at == at + 3600ms, "older session changed source");
    auto restart = heartbeat;
    restart.owner_epoch = ns(at + 4s);
    restart.session_generation = 1;
    restart.sent_at = at + 4250ms;
    restart.navigation.updated_at = at + 4100ms;
    send(path, encode_navigation_datagram(restart));
    check(input.sample(at + 4250ms).updated_at == at + 4100ms,
          "publisher restart failed fresh session recovery");
    heartbeat.sent_at = at + 4500ms;
    send(path, encode_navigation_datagram(heartbeat));
    check(input.sample(at + 4500ms).updated_at == at + 4100ms, "old publisher epoch revived");
    rejected = false;
    try {
      input.sample(at + 4s);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "reversed reader clock admitted");
  }
  check(!std::filesystem::exists(path), "endpoint left behind at retirement");
  {
    SocketNavigation input(path);
    NavigationPublisher publisher(path);
    const auto now = Clock::now() + 1s;
    auto current = navigation(now);
    check(publisher.publish(current, 1, now) && input.sample(now).available,
          "production publisher failed delivery");
    check(!publisher.publish({}, 1, now + 249ms), "publisher exceeded four Hz");
    check(publisher.publish({}, 1, now + 250ms) && !input.sample(now + 250ms).available,
          "bounded disconnect publication failed");
    rejected = false;
    try {
      NavigationPublisher duplicate(path);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "second publisher owner admitted");
  }
  {
    // Killed reader leaves a dead path and stale kernel queue; the replacement
    // holds the same owner lock and recovers without replaying that queue.
    int ready[2];
    check(pipe(ready) == 0, "readiness pipe failed");
    const auto pid = fork();
    check(pid >= 0, "fork failed");
    if (pid == 0) {
      close(ready[0]);
      try {
        SocketNavigation input(path);
        if (write(ready[1], "R", 1) != 1) _exit(2);
        close(ready[1]);
        for (;;)
          pause();
      } catch (...) {
        _exit(2);
      }
    }
    close(ready[1]);
    char signal = 0;
    const auto size = read(ready[0], &signal, 1);
    close(ready[0]);
    check(size == 1 && signal == 'R', "child endpoint did not start");
    send(path, wire);
    check(kill(pid, SIGKILL) == 0, "could not stop child endpoint");
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    check(WIFSIGNALED(status), "child did not retire");
    SocketNavigation recovered(path);
    check(!recovered.sample(at).available, "recovered endpoint replayed dead queue");
    send(path, wire);
    check(recovered.sample(at).available, "recovered endpoint rejected fresh publication");
  }
  {
    const auto unsafe = (root / "unsafe").string();
    std::ofstream(unsafe) << "unrelated";
    rejected = false;
    try {
      SocketNavigation input(unsafe);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "regular file replaced by endpoint");
    std::filesystem::create_symlink(unsafe, root / "link");
    rejected = false;
    try {
      SocketNavigation input((root / "link").string());
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "symlink replaced by endpoint");
    const auto public_dir = root / "public";
    std::filesystem::create_directory(public_dir);
    chmod(public_dir.c_str(), 0755);
    rejected = false;
    try {
      SocketNavigation input((public_dir / "input").string());
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "public directory admitted");
  }
  {
    const auto target = (root / "actual.sock").string();
    SocketNavigation input(target);
    const auto alias = (root / "redirect.sock").string();
    std::filesystem::create_symlink(target, alias);
    NavigationPublisher publisher(alias);
    const auto now = Clock::now() + 1s;
    rejected = false;
    try {
      publisher.publish(navigation(now), 1, now);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected && !input.sample(now).available, "publisher followed socket symlink");
    std::filesystem::remove(alias);
  }
  {
    NavigationPublisher publisher(path);
    const auto now = Clock::now() + 1s;
    check(!publisher.publish(navigation(now), 1, now), "missing consumer reported publication");
    SocketNavigation input(path);
    chmod(path.c_str(), 0666);
    rejected = false;
    try {
      publisher.publish(navigation(now), 1, now + 250ms);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected && !input.sample(now + 250ms).available, "publisher delivered to public socket");
    chmod(path.c_str(), 0600);
    check(publisher.publish(navigation(now), 1, now + 500ms) && input.sample(now + 500ms).available,
          "safe replacement consumer failed delivery");
  }
  std::filesystem::remove_all(root);
  std::cout << "Navigation codec, original dates, stationary lease, replay/withdrawal, publisher "
               "bounds and endpoint retirement passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
