// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/presentation_status.hpp>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F&& action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "expected rejection");
}
struct Private {
  std::filesystem::path folder;
  Private() {
    char name[] = "/tmp/headunit-presentation-status-XXXXXX";
    auto* p = mkdtemp(name);
    check(p, "mkdtemp failed");
    folder = std::filesystem::canonical(p);
  }
  ~Private() {
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
  }
  std::string path() const { return (folder / "carplay-status.json").string(); }
};
std::string read(const std::string& path) {
  std::ifstream f(path);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
std::string field(const std::string& data, const std::string& key) {
  const auto begin = data.find("\"" + key + "\":\"");
  check(begin != data.npos, "field missing");
  const auto first = begin + key.size() + 4;
  const auto end = data.find('"', first);
  check(end != data.npos, "field unterminated");
  return data.substr(first, end - first);
}
constexpr const char* nonce = "0123456789abcdef0123456789abcdef";
template <class F>
std::string unchanged_update(const std::string& path, const std::string& previous, F&& update) {
  const auto published = std::stoull(field(previous, "updatedMonotonicNs"));
  const auto sequence = std::stoull(field(previous, "sequence"));
  const auto before = headunit::presentation_monotonic_ns();
  update();
  const auto after = headunit::presentation_monotonic_ns();
  const auto current = read(path);
  // Sanitizers and contended runners can pause between identical updates.
  // A genuine heartbeat is allowed, but it must obey the publication dates
  // and advance exactly once. Keep the production CLOCK_MONOTONIC source.
  if (current == previous) {
    check(before - published < 250000000, "due heartbeat was suppressed");
  } else {
    const auto date = std::stoull(field(current, "updatedMonotonicNs"));
    check(date >= before && date <= after && date >= published + 250000000,
          "unchanged status caused unbounded writes");
    check(std::stoull(field(current, "sequence")) == sequence + 1,
          "unchanged update advanced more than one heartbeat");
    auto semantics = current;
    for (const auto* key : {"sequence", "updatedMonotonicNs"}) {
      const auto value = field(semantics, key);
      const auto start =
          semantics.find("\"" + std::string(key) + "\":\"") + std::string(key).size() + 4;
      semantics.replace(start, value.size(), field(previous, key));
    }
    check(semantics == previous, "heartbeat changed publication semantics");
  }
  return current;
}
void heartbeat_preserves_semantics() {
  for (const auto* key : {"returnReady", "negotiatedMode"}) {
    Private files;
    headunit::PresentationStatusPublisher writer(files.path(), nonce);
    writer.update(headunit::PresentationState::ready, 1, true, headunit::CarplayMode::ultra);
    const auto previous = read(files.path());
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    bool diagnosed = false;
    try {
      (void)unchanged_update(files.path(), previous, [&] {
        writer.update(headunit::PresentationState::ready, 1, true, headunit::CarplayMode::ultra);
        // A real due heartbeat supplies the clock/sequence. Change only one
        // semantic field to prove it cannot hide inside that valid renewal.
        auto changed = read(files.path());
        const bool readiness = std::string(key) == "returnReady";
        const std::string from =
            readiness ? "\"returnReady\":true" : "\"negotiatedMode\":\"ultra\"";
        const std::string to =
            readiness ? "\"returnReady\":false" : "\"negotiatedMode\":\"standard\"";
        const auto start = changed.find(from);
        check(start != changed.npos, "semantic fixture field missing");
        changed.replace(start, from.size(), to);
        std::ofstream out(files.path());
        out << changed;
        check(bool(out), "semantic fixture write failed");
      });
    } catch (const std::runtime_error& error) {
      diagnosed = std::string(error.what()) == "heartbeat changed publication semantics";
    }
    check(diagnosed, "due heartbeat hid a semantic change");
  }
}
void receipt_generation() {
  headunit::MainPresentationSnapshot source;
  const auto first = source.begin();
  check(!source.sample().qualified, "startup qualified without receipt");
  source.observe(first, false, true, true);
  check(!source.sample().qualified, "hidden startup qualified without receipt");
  source.observe(first, true, false, true);
  check(source.sample().qualified, "actual scene did not qualify");
  source.observe(first, false, true, true);
  auto retained = source.sample();
  check(retained.qualified && retained.home && retained.healthy,
        "Home lost healthy retained eligibility");
  source.observe(first, false, true, false);
  check(!source.sample().qualified, "clock fault retained qualification");
  source.observe(first, false, true, true);
  check(!source.sample().qualified, "healthy observation revived old receipt");
  source.observe(first, true, false, true);
  check(source.sample().qualified, "fresh scene failed recovery");
  // Each Home return is counted so the control owner can ask for a keyframe.
  source.observe(first, true, false, true, 2);
  check(source.sample().surface_restorations == 2, "main restorations not published");
  const auto second = source.begin();
  check(second > first && !source.sample().qualified, "new generation reused receipt");
  check(source.sample().surface_restorations == 0, "new generation inherited restorations");
  source.observe(first, true, false, true, 5);
  check(source.sample().surface_restorations == 0, "old worker published restorations");
  source.observe(first, true, false, true);
  check(!source.sample().qualified, "old worker updated replacement");
  source.observe(second, true, false, true);
  source.retire(first);
  check(source.sample().qualified, "old worker retired replacement");
  source.retire(second);
  check(!source.sample().qualified, "current worker retirement retained receipt");
  source.observe(second, true, false, true, 9);
  const auto retired = source.sample();
  check(
      !retired.qualified && !retired.healthy && !retired.home && retired.surface_restorations == 0,
      "late observation revived a retired main presentation owner");
  const auto third = source.begin();
  source.observe(third, false, true, true);
  check(!source.sample().qualified, "replacement inherited a retired receipt");
  source.observe(third, true, false, true);
  source.retire(second);
  check(source.sample().qualified, "retired owner cleared its replacement");
}
void snapshot_age_and_threads() {
  headunit::MainPresentationSnapshot source;
  const auto generation = source.begin();
  std::thread owner([&] {
    for (unsigned i = 0; i < 20000; ++i)
      source.observe(generation, true, false, true);
  });
  for (unsigned i = 0; i < 20000; ++i)
    (void)source.sample();
  owner.join();
  check(source.sample().qualified, "concurrent receipt snapshot failed");
  std::this_thread::sleep_for(std::chrono::milliseconds(1020));
  check(!source.sample().qualified, "stalled worker heartbeat remained eligible");
}
void publisher_wire_and_terminal() {
  Private private_files;
  std::string last;
  {
    headunit::PresentationStatusPublisher writer(private_files.path(), nonce);
    auto data = read(private_files.path());
    check(data.size() <= 512 && data.ends_with("}\n"), "wire bound failed");
    check(data.starts_with("{\"version\":3,") &&
              data.find("\"negotiatedMode\":null") != data.npos &&
              field(data, "phoneHomeRequests") == "0",
          "initial mode/version incorrect");
    check(field(data, "nonce") == nonce && field(data, "sequence") == "1" &&
              field(data, "connectionGeneration") == "0" && field(data, "state") == "disconnected",
          "initial wire incorrect");
    const auto before = headunit::presentation_monotonic_ns();
    writer.update(headunit::PresentationState::ready, 3, true);
    data = read(private_files.path());
    const auto date = std::stoull(field(data, "updatedMonotonicNs"));
    check(date >= before && date <= headunit::presentation_monotonic_ns(),
          "wire observation not actual CLOCK_MONOTONIC");
    check(field(data, "sequence") == "2" && field(data, "connectionGeneration") == "3" &&
              data.find("\"returnReady\":true") != data.npos,
          "ready wire incorrect");
    writer.update(headunit::PresentationState::ready, 3, true, std::nullopt, 1);
    data = read(private_files.path());
    check(field(data, "sequence") == "3" && field(data, "phoneHomeRequests") == "1",
          "phone Home request did not publish immediately");
    writer.update(headunit::PresentationState::ready, 3, true);
    data = read(private_files.path());
    check(field(data, "phoneHomeRequests") == "0", "phone Home count not replaced");
    data = unchanged_update(private_files.path(), data,
                            [&] { writer.update(headunit::PresentationState::ready, 3, true); });
    const auto sequence = std::stoull(field(data, "sequence"));
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    data = unchanged_update(private_files.path(), data,
                            [&] { writer.update(headunit::PresentationState::ready, 3, true); });
    check(std::stoull(field(data, "sequence")) == sequence + 1,
          "real heartbeat did not advance sequence");
    writer.update(headunit::PresentationState::home, 3, true);
    writer.update(headunit::PresentationState::disconnected, 3, false);
    rejects([&] { writer.update(headunit::PresentationState::ready, 2, true); });
    rejects([&] { writer.update(headunit::PresentationState::ready, 3, false); });
    rejects([&] { writer.update(headunit::PresentationState::starting, 3, true); });
    last = read(private_files.path());
    struct stat file{};
    check(lstat(private_files.path().c_str(), &file) == 0 && (file.st_mode & 07777) == 0600 &&
              file.st_nlink == 1,
          "publication not private");
  }
  const auto stopped = read(private_files.path());
  check(field(stopped, "state") == "stopped" && field(stopped, "connectionGeneration") == "3" &&
            stopped.find("\"returnReady\":false") != stopped.npos,
        "destructor failed terminal publication");
  check(stopped.find("\"negotiatedMode\":null") != stopped.npos, "stopped retained old mode");
  check(std::stoull(field(stopped, "sequence")) > std::stoull(field(last, "sequence")),
        "terminal sequence did not advance");
}
void publication_write_failure() {
  Private files;
  headunit::PresentationStatusPublisher writer(files.path(), nonce);
  const auto accepted = read(files.path());
  bool diagnosed = false;
  {
    rlimit original{};
    check(::getrlimit(RLIMIT_FSIZE, &original) == 0, "file-size limit read failed");
    const auto previous_signal = std::signal(SIGXFSZ, SIG_IGN);
    check(previous_signal != SIG_ERR, "file-size signal setup failed");
    struct Restore {
      rlimit limit;
      void (*handler)(int);
      ~Restore() {
        (void)::setrlimit(RLIMIT_FSIZE, &limit);
        (void)std::signal(SIGXFSZ, handler);
      }
    } restore{original, previous_signal};
    auto limit = original;
    limit.rlim_cur = 16;
    check(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "file-size fault setup failed");
    try {
      // The real write partially fills the temporary file before the kernel
      // rejects the remainder. Published state must remain the accepted state.
      writer.update(headunit::PresentationState::starting, 1, false, headunit::CarplayMode::ultra);
    } catch (const std::system_error& error) {
      diagnosed = error.code() == std::error_code(EFBIG, std::generic_category()) &&
                  std::string(error.what()).find("presentation publication write failed") !=
                      std::string::npos;
    }
  }
  check(diagnosed, "actual publication write failure lost errno context");
  check(read(files.path()) == accepted, "partial write replaced accepted publication");
  unsigned entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(files.folder)) {
    (void)entry;
    ++entries;
  }
  check(entries == 2, "failed publication leaked temporary file");
  writer.update(headunit::PresentationState::ready, 1, true, headunit::CarplayMode::ultra);
  const auto next = read(files.path());
  check(field(next, "sequence") == "2" && field(next, "state") == "ready" &&
            field(next, "connectionGeneration") == "1" && field(next, "negotiatedMode") == "ultra",
        "failed publication advanced accepted state or scope failed to restore file-size limit");
}
void negotiated_mode_publication() {
  Private files;
  headunit::PresentationStatusPublisher writer(files.path(), nonce);
  writer.update(headunit::PresentationState::starting, 1, false);
  const auto unknown = read(files.path());
  writer.update(headunit::PresentationState::starting, 1, false, headunit::CarplayMode::standard);
  const auto standard = read(files.path());
  check(field(standard, "negotiatedMode") == "standard" &&
            std::stoull(field(standard, "sequence")) == std::stoull(field(unknown, "sequence")) + 1,
        "mode change did not publish immediately on same nonce/generation");
  (void)unchanged_update(files.path(), standard, [&] {
    writer.update(headunit::PresentationState::starting, 1, false, headunit::CarplayMode::standard);
  });
  writer.update(headunit::PresentationState::starting, 2, false);
  check(read(files.path()).find("\"negotiatedMode\":null") != std::string::npos,
        "new connection retained prior mode");
  writer.update(headunit::PresentationState::ready, 2, true, headunit::CarplayMode::ultra);
  check(field(read(files.path()), "negotiatedMode") == "ultra", "actual Ultra mode missing");
  writer.update(headunit::PresentationState::disconnected, 2, false, headunit::CarplayMode::ultra);
  check(read(files.path()).find("\"negotiatedMode\":null") != std::string::npos,
        "disconnect retained mode");
  rejects([&] {
    writer.update(headunit::PresentationState::starting, 2, false,
                  static_cast<headunit::CarplayMode>(99));
  });
}
void ownership_and_unsafe_paths() {
  Private files;
  rejects([&] { headunit::PresentationStatusPublisher writer(files.path(), "BAD"); });
  check(!std::filesystem::exists(files.path()), "invalid nonce created endpoint");
  {
    headunit::PresentationStatusPublisher writer(files.path(), nonce);
    rejects([&] { headunit::PresentationStatusPublisher other(files.path(), nonce); });
    const auto link = files.folder / "hardlink";
    check(::link(files.path().c_str(), link.c_str()) == 0, "hardlink failed");
    rejects([&] { writer.update(headunit::PresentationState::starting, 1, false); });
    std::filesystem::remove(link);
    writer.update(headunit::PresentationState::starting, 1, false);
    const auto alternate = files.folder / "unrelated";
    {
      std::ofstream out(alternate);
      out << "keep";
    }
    std::filesystem::remove(files.path());
    check(::symlink(alternate.c_str(), files.path().c_str()) == 0, "symlink failed");
    rejects([&] { writer.update(headunit::PresentationState::ready, 1, true); });
    check(read(alternate.string()) == "keep", "unsafe target mutated");
  }
  std::filesystem::remove(files.path());
  const auto owner = files.folder / "carplay-status.json.owner";
  std::filesystem::remove(owner);
  check(::chmod(files.folder.c_str(), 0755) == 0, "chmod failed");
  rejects([&] { headunit::PresentationStatusPublisher writer(files.path(), nonce); });
  check(::chmod(files.folder.c_str(), 0700) == 0, "chmod restore failed");
  headunit::PresentationStatusPublisher writer(files.path(), nonce);
  const auto moved = files.folder.string() + "-old";
  check(::rename(files.folder.c_str(), moved.c_str()) == 0, "directory rename failed");
  check(::mkdir(files.folder.c_str(), 0700) == 0, "replacement directory failed");
  rejects([&] { writer.update(headunit::PresentationState::starting, 1, false); });
  check(!std::filesystem::exists(files.path()), "replaced directory received publication");
  std::filesystem::remove_all(moved);
}
int interop(const std::string& folder, const std::string& token) {
  // Explicit wire fixture: these states are scheduling inputs, not genuine GPU
  // qualification. Actual publisher/file/clock and the Python reader run.
  headunit::PresentationStatusPublisher writer(
      (std::filesystem::path(folder) / "carplay-status.json").string(), token);
  auto state = headunit::PresentationState::disconnected;
  std::uint64_t generation = 0;
  bool ready = false;
  std::cout << "initial\n" << std::flush;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    const auto result = ::poll(&input, 1, 25);
    check(result >= 0, "interop input polling failed");
    if (input.revents & (POLLIN | POLLHUP)) {
      char command[8]{};
      const auto count = ::read(STDIN_FILENO, command, sizeof(command));
      check(count >= 0, "interop input read failed");
      if (!count) break;
      for (ssize_t i = 0; i < count; ++i) {
        const auto c = command[i];
        if (c == '\n') continue;
        if (c == 'r') {
          generation = std::max<std::uint64_t>(generation, 1);
          state = headunit::PresentationState::ready;
          ready = true;
        } else if (c == 'h') {
          generation = std::max<std::uint64_t>(generation, 1);
          state = headunit::PresentationState::home;
          ready = true;
        } else if (c == 'f') {
          state = headunit::PresentationState::fault;
          ready = false;
        } else if (c == 'd') {
          state = headunit::PresentationState::disconnected;
          ready = false;
        } else if (c == 'n') {
          ++generation;
          state = headunit::PresentationState::starting;
          ready = false;
        } else if (c == 's') {
          writer.update(headunit::PresentationState::stopped, generation, false);
          std::cout << "s\n" << std::flush;
          return 0;
        } else
          throw std::invalid_argument("unknown interop command");
        writer.update(state, generation, ready);
        std::cout << c << '\n' << std::flush;
      }
    }
    writer.update(state, generation, ready);
  }
  return 0;
}
}  // namespace
int main(int argc, char** argv) try {
  if (argc == 4 && std::string_view(argv[1]) == "--interop") return interop(argv[2], argv[3]);
  check(argc == 1, "usage: presentation-status-tests [--interop PRIVATE_DIR NONCE]");
  receipt_generation();
  snapshot_age_and_threads();
  publisher_wire_and_terminal();
  heartbeat_preserves_semantics();
  ownership_and_unsafe_paths();
  negotiated_mode_publication();
  publication_write_failure();
  std::cout << "presentation status: 7 groups passed (actual clock/files/flock; receipt facts "
               "supplied by fixture)\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
