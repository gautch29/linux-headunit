// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/cluster_input.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <utility>
#include <vector>

namespace {
void check(bool condition) {
  if (!condition) throw std::runtime_error("cluster input test failed");
}
}  // namespace
int main() {
  char folder[] = "/tmp/headunit-cluster-input-XXXXXX";
  check(mkdtemp(folder));
  const auto path = std::filesystem::path(folder) / "layout.input";
  const std::string nonce(32, 'a');
  using Control = headunit::ClusterInputControl;
  auto now = Control::Clock::time_point(std::chrono::seconds(10));
  const auto write = [&](unsigned sequence, const std::string& action = "next",
                         std::uint64_t date = 10000000000ULL,
                         const std::string& publisher = std::string(32, 'a')) {
    std::ofstream out(path);
    out << "HUI1\n" << publisher << "\n1\n" << sequence << '\n' << date << '\n' << action << '\n';
    out.close();
    check(!chmod(path.c_str(), 0600));
  };
  std::vector<unsigned> reports;
  std::vector<headunit::HidKeyTarget> targets;
  bool accept = true;
  const auto send = [&](headunit::HidKeyTarget target, std::uint8_t report) {
    reports.push_back(report);
    targets.push_back(target);
    return accept;
  };
  Control control(path, nonce, 1);
  control.poll(true, send, now);
  check(reports.empty());
  write(1);
  now += std::chrono::milliseconds(100);
  control.poll(true, send, now);
  check(reports == std::vector<unsigned>{16});
  // The request arriving during a held key is consumed, not replayed later.
  write(2, "previous");
  now += std::chrono::milliseconds(100);
  control.poll(true, send, now);
  control.poll(true, send, now + std::chrono::milliseconds(100));
  check(reports == std::vector<unsigned>({16, 0}));
  now += std::chrono::milliseconds(200);
  write(3, "select");
  control.poll(true, send, now);
  control.poll(false, send, now + std::chrono::milliseconds(1));
  check(reports == std::vector<unsigned>({16, 0, 1, 0}));
  // Old/future dates, not-ready input and rejected presses cannot revive.
  for (unsigned sequence = 4; sequence <= 8; ++sequence) {
    now += std::chrono::milliseconds(100);
    write(sequence, sequence == 8 ? "invalid" : "next",
          sequence == 4   ? 1
          : sequence == 5 ? 20000000000ULL
                          : 10000000000ULL);
    accept = sequence != 7;
    const auto before = reports.size();
    control.poll(sequence != 6, send, now);
    check(reports.size() == before + (sequence == 7 ? 1 : 0));
    accept = true;
    control.poll(true, send, now + std::chrono::milliseconds(100));
    check(reports.size() == before + (sequence == 7 ? 1 : 0));
    now += std::chrono::milliseconds(100);
  }
  now += std::chrono::milliseconds(100);
  write(9, "back", 10000000000ULL, std::string(32, 'b'));
  control.poll(true, send, now);
  const auto before = reports.size();
  // File mode/links and publisher identity cannot authorize input.
  write(10);
  check(!chmod(path.c_str(), 0644));
  now += std::chrono::milliseconds(100);
  control.poll(true, send, now);
  check(reports.size() == before);
  write(11);
  std::filesystem::create_hard_link(path, path.string() + ".hard");
  now += std::chrono::milliseconds(100);
  control.poll(true, send, now);
  check(reports.size() == before);
  std::filesystem::remove(path.string() + ".hard");
  std::filesystem::remove(path);
  std::filesystem::create_symlink("missing", path);
  now += std::chrono::milliseconds(100);
  control.poll(true, send, now);
  check(reports.size() == before);
  std::filesystem::remove(path);
  // A failed release is retried even with malformed input or readiness loss.
  now = Control::Clock::time_point(std::chrono::seconds(12));
  write(12, "down", 12000000000ULL);
  control.poll(true, send, now);
  accept = false;
  std::ofstream(path) << "bad\n";
  control.poll(false, send, now + std::chrono::milliseconds(100));
  accept = true;
  control.poll(false, send, now + std::chrono::milliseconds(101));
  check(reports.size() == before + 3 && reports[before] == 4 && reports[before + 1] == 0 &&
        reports[before + 2] == 0);
  check(targets.front() == headunit::HidKeyTarget::cluster);
  // Media keys press and release the separate media device.
  now = Control::Clock::time_point(std::chrono::seconds(20));
  unsigned media_sequence = 100;
  for (const auto& [action, bit] : {std::pair<const char*, unsigned>{"play_pause", 1},
                                    {"next_track", 2},
                                    {"previous_track", 4}}) {
    now += std::chrono::milliseconds(200);
    write(media_sequence++, action, std::uint64_t(now.time_since_epoch().count()));
    const auto start = reports.size();
    control.poll(true, send, now);
    control.poll(true, send, now + std::chrono::milliseconds(100));
    check(reports.size() == start + 2 && reports[start] == bit && reports[start + 1] == 0 &&
          targets[start] == headunit::HidKeyTarget::media &&
          targets[start + 1] == headunit::HidKeyTarget::media);
  }
  // App shortcuts call the opener once and never press a HID key.
  std::vector<std::string> opened;
  const auto opener = [&](const std::string& url) {
    opened.push_back(url);
    return true;
  };
  for (const auto* action : {"open_maps", "open_music", "open_phone"}) {
    now += std::chrono::milliseconds(200);
    write(media_sequence++, action, std::uint64_t(now.time_since_epoch().count()));
    const auto before_open = reports.size();
    control.poll(true, send, now, opener);
    control.poll(true, send, now + std::chrono::milliseconds(100), opener);
    check(reports.size() == before_open);
  }
  check(opened == std::vector<std::string>({"maps:", "music:", "mobilephone:"}));
  std::filesystem::remove_all(folder);
}
