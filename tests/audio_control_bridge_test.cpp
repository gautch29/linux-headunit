// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_control_bridge.hpp>

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

using namespace headunit::audio_control;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
namespace {
struct Folder {
  std::string path;
  Folder() {
    char name[] = "/tmp/headunit-audio-bridge-XXXXXX";
    const auto* created = mkdtemp(name);
    CHECK(created);
    path = std::filesystem::canonical(created).string();
    CHECK(chmod(path.c_str(), 0700) == 0);
  }
  ~Folder() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::string request() const { return path + "/control.json"; }
  std::string state() const { return path + "/state.json"; }
};
std::uint64_t now() {
  return monotonic_time_ns();
}
void write(const std::string& path, std::string_view content) {
  std::ofstream output(path);
  output << content;
  output.close();
  CHECK(output.good());
  CHECK(chmod(path.c_str(), 0600) == 0);
}
std::string command(std::string_view route, std::uint64_t sequence, unsigned percent, bool muted) {
  return "{\"version\":1,\"route\":\"" + std::string(route) + "\",\"sequence\":\"" +
         std::to_string(sequence) + "\",\"volumePercent\":" + std::to_string(percent) +
         ",\"muted\":" + (muted ? "true" : "false") + "}";
}
void requested_is_not_applied_and_replay() {
  Folder files;
  Bridge bridge(files.request(), files.state(), getuid(), [] { return std::string(32, 'a'); });
  auto control = std::make_shared<Mailbox>(Level{25, false});
  control->open();
  const std::array routes{control};
  bridge.poll(routes, now());
  CHECK(bridge.state().control.available && bridge.state().route == std::string(32, 'a') &&
        bridge.state().control.applied == Command(0, {25, false}) &&
        bridge.publication_error().empty());
  write(files.request(), command(bridge.state().route, 1, 50, true));
  bridge.poll(routes, now());
  CHECK(bridge.state().control.requested == Command(1, {50, true}) &&
        bridge.state().control.applied == Command(0, {25, false}));
  const auto lease = control->next();
  CHECK(lease);
  control->applied(*lease);
  bridge.poll(routes, now());
  CHECK(bridge.state().control.applied == Command(1, {50, true}) && !control->next());
  write(files.request(), command(bridge.state().route, 1, 99, false));
  bridge.poll(routes, now());
  CHECK(!bridge.state().error.empty() && control->snapshot().applied == Command(1, {50, true}));
  write(files.request(), command(bridge.state().route, 2, 0, false));
  bridge.poll(routes, now());
  CHECK(bridge.state().error.empty() &&
        bridge.state().control.requested == Command(2, {0, false}) &&
        bridge.state().control.applied == Command(1, {50, true}));
  control->close();
  bridge.poll(routes, now());
  CHECK(!bridge.state().control.available && bridge.state().route.empty());
}
void route_change_blocks_old_controls() {
  Folder files;
  unsigned identities = 0;
  Bridge bridge(files.request(), files.state(), getuid(),
                [&] { return std::string(32, char('a' + identities++)); });
  auto first = std::make_shared<Mailbox>();
  first->open();
  std::array routes{first};
  bridge.poll(routes, now());
  const auto old_route = bridge.state().route;
  write(files.request(), command(old_route, 100, 0, true));
  first->close();
  bridge.poll(routes, now());
  CHECK(!bridge.state().control.available && bridge.state().route.empty());
  auto next = std::make_shared<Mailbox>(Level{40, false});
  next->open();
  routes[0] = next;
  bridge.poll(routes, now());
  CHECK(bridge.state().control.available && bridge.state().route != old_route &&
        bridge.state().control.requested.sequence == 0 && !bridge.state().error.empty());
  write(files.request(), command(bridge.state().route, 1, 70, false));
  bridge.poll(routes, now());
  CHECK(next->snapshot().requested == Command(1, {70, false}) &&
        next->snapshot().applied == Command(0, {40, false}));
  next->close();
}
void malformed_or_unsafe_file_preserves_audio() {
  Folder files;
  Bridge bridge(files.request(), files.state(), getuid(), [] { return std::string(32, 'c'); });
  auto control = std::make_shared<Mailbox>();
  control->open();
  const std::array routes{control};
  bridge.poll(routes, now());
  write(files.request(), "{\"version\":1}");
  bridge.poll(routes, now());
  CHECK(bridge.state().control.available && !bridge.state().error.empty() &&
        control->snapshot().requested.sequence == 0);
  write(files.request(), command(bridge.state().route, 1, 30, false));
  CHECK(chmod(files.request().c_str(), 0644) == 0);
  bridge.poll(routes, now());
  CHECK(bridge.state().control.available && !bridge.state().error.empty() &&
        control->snapshot().requested.sequence == 0);
  CHECK(chmod(files.request().c_str(), 0600) == 0);
  bridge.poll(routes, now());
  CHECK(bridge.state().error.empty() && control->snapshot().requested == Command(1, {30, false}));
  std::filesystem::remove(files.request());
  std::filesystem::remove(files.state());
  CHECK(symlink("other.json", files.state().c_str()) == 0);
  bridge.poll(routes, now());
  CHECK(!bridge.publication_error().empty() && bridge.state().control.available &&
        control->snapshot().applied.sequence == 0 &&
        !std::filesystem::exists(files.path + "/other.json"));
  control->close();
}
void ambiguous_or_faulted_route_withdraws_controls() {
  Folder files;
  Bridge bridge(files.request(), files.state(), getuid(), [] { return std::string(32, 'd'); });
  auto first = std::make_shared<Mailbox>(), second = std::make_shared<Mailbox>();
  first->open();
  second->open();
  const std::array routes{first, second};
  bridge.poll(routes, now());
  CHECK(!bridge.state().control.available && bridge.state().route.empty() &&
        !bridge.state().error.empty());
  first->close();
  second->close();
  bridge.poll(routes, now());
  CHECK(!bridge.state().control.available && bridge.state().error.empty() &&
        bridge.publication_error().empty());
}
void owner_and_time_are_required() {
  Folder files;
  Bridge bridge(files.request(), files.state(), getuid());
  bridge.poll({}, now());
  const auto before = bridge.state();
  bool bad_time = false;
  try {
    bridge.poll({}, 1);
  } catch (const std::invalid_argument&) {
    bad_time = true;
  }
  CHECK(bad_time && bridge.state() == before);
  bool bad_owner = false;
  std::thread other([&] {
    try {
      bridge.poll({}, now());
    } catch (const std::logic_error&) {
      bad_owner = true;
    }
  });
  other.join();
  CHECK(bad_owner && bridge.state() == before);
}
}  // namespace
int main(int argc, char** argv) try {
  if (argc == 3 && std::string_view(argv[1]) == "--interop") {
    const std::string folder = argv[2];
    Bridge bridge(folder + "/audio-control.json", folder + "/audio-state.json", getuid(),
                  [] { return std::string(32, 'e'); });
    auto control = std::make_shared<Mailbox>(Level{25, false});
    control->open();
    const std::array routes{control};
    bridge.poll(routes, now());
    CHECK(bridge.publication_error().empty() && bridge.state().error.empty());
    CHECK(bridge.state().control.requested.sequence > 0 &&
          bridge.state().control.applied.sequence == 0);
    const auto pending = encode_state(bridge.state(), now());
    write(folder + "/pending.json", pending);
    const auto lease = control->next();
    CHECK(lease);
    control->applied(*lease);
    control->observe_output({now(), 42, 1, 2, true});
    bridge.poll(routes, now());
    CHECK(bridge.publication_error().empty() &&
          bridge.state().control.applied == bridge.state().control.requested);
    control->close();
    return 0;
  }
  CHECK(argc == 1);
  requested_is_not_applied_and_replay();
  route_change_blocks_old_controls();
  malformed_or_unsafe_file_preserves_audio();
  ambiguous_or_faulted_route_withdraws_controls();
  owner_and_time_are_required();
  std::cout << "audio bridge: 5 private-file/actual-mailbox scenarios passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
