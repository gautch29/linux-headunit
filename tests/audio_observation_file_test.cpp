// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_observation.hpp"
#include "audio_observation_retention_fixture.hpp"
#include <sys/resource.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace headunit::audio_observation;
namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
std::uint64_t number(std::string_view value) {
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  check(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
        "invalid numeric fixture export");
  return result;
}
std::uint64_t field(const std::string& footer, const char* name) {
  const auto key = std::string("\"") + name + "\":";
  const auto at = footer.find(key);
  check(at != std::string::npos, "missing exporter counter");
  const auto begin = at + key.size();
  return number(std::string_view(footer).substr(begin, footer.find_first_of(",}", begin) - begin));
}
using Row = std::array<std::uint64_t, 9>;
Row row(const Event& event) {
  return {event.date_ns, std::uint64_t(event.kind),
          event.source,  event.route,
          event.epoch,   event.a,
          event.b,       event.c,
          event.d};
}
void retention_failures(const std::filesystem::path& directory) {
  const auto original = directory / "retention-original";
  const auto evidence = directory / "retention-evidence";
  std::filesystem::create_directory(original);
  std::filesystem::create_directory(evidence);
  const std::string bytes =
      "[9007199254740993,28,7,11,3,0,18446744073709551615,480,18446744073709551615]\n";
  audio_retention_fixture::write_new(original / "trace.jsonl", bytes);
  audio_retention_fixture::write_new(evidence / "empty", "");
  check(audio_retention_fixture::read(evidence / "empty").empty(), "empty closed copy changed");
  check(audio_retention_fixture::header(original / "trace.jsonl") == bytes,
        "bounded live header peek changed original bytes");
  const auto fails = [](auto operation) {
    bool failed = false;
    try {
      operation();
    } catch (const std::runtime_error&) {
      failed = true;
    }
    check(failed, "mandatory retention failure was swallowed");
  };
  fails([&] { audio_retention_fixture::read(original / "missing"); });
  fails([&] { audio_retention_fixture::header(evidence / "empty"); });
  fails([&] { audio_retention_fixture::write_new(evidence / "empty", "overwrite"); });
  fails([&] { audio_retention_fixture::verify(original / "trace.jsonl", "corrupt"); });
  fails([&] {
    audio_retention_fixture::retain(original, directory / "missing", {"trace.jsonl"}, true);
  });
  check(audio_retention_fixture::read(original / "trace.jsonl") == bytes,
        "failed retention removed original source");
  const int available = open((original / "trace.jsonl").c_str(), O_RDONLY | O_CLOEXEC);
  check(available >= 0 && close(available) == 0, "retention descriptor baseline failed");
  {
    rlimit limit{};
    check(getrlimit(RLIMIT_FSIZE, &limit) == 0, "retention write limit unavailable");
    const auto previous = std::signal(SIGXFSZ, SIG_IGN);
    check(previous != SIG_ERR, "retention write signal guard failed");
    struct Restore {
      rlimit limit;
      void (*handler)(int);
      ~Restore() {
        (void)setrlimit(RLIMIT_FSIZE, &limit);
        (void)std::signal(SIGXFSZ, handler);
      }
    } restore{limit, previous};
    auto bounded = limit;
    bounded.rlim_cur = 8;
    check(setrlimit(RLIMIT_FSIZE, &bounded) == 0, "retention partial write guard failed");
    fails([&] { audio_retention_fixture::retain(original, evidence, {"trace.jsonl"}, true); });
    check(audio_retention_fixture::read(original / "trace.jsonl") == bytes,
          "partial failed copy removed original source");
  }
  audio_retention_fixture::Descriptor probe{
      open((original / "trace.jsonl").c_str(), O_RDONLY | O_CLOEXEC)};
  check(probe.fd == available, "failed copy leaked its owned descriptor");
  probe.close();
  fails([&] { probe.close(); });
  const auto retained = audio_retention_fixture::retain(original, evidence, {"trace.jsonl"}, true);
  check(
      !std::filesystem::exists(original) &&
          audio_retention_fixture::read(retained / "trace.jsonl") == bytes &&
          audio_retention_fixture::read(retained / "retention.json").find("closedCopiesVerified") !=
              std::string::npos,
      "verified recovery did not preserve closed original bytes before retirement");
  std::cout << "retention_open_existing_mismatch_directory_partial_write_close_failures=PASS "
               "original_preserved_until_verified_recovery=PASS descriptor_reaped=PASS\n";
}
void ready(File& file) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (file.status().state == File::State::starting &&
         std::chrono::steady_clock::now() < deadline) {
    (void)file.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(file.status().state == File::State::running, "real collector did not start");
}
void stopped(File& file) {
  const auto pid = file.status().collector_pid;
  check(kill(pid, SIGSTOP) == 0, "collector stop failed");
  int status = 0;
  check(
      waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP,
      "collector stop not observed");
}
void absent(const File& file) {
  const auto status = file.status();
  check(status.reaped, "collector was not actually reaped");
  check(kill(status.collector_pid, 0) == -1 && errno == ESRCH, "collector survived retirement");
}
void compare_file(const std::filesystem::path& path, File& file,
                  const std::vector<Row>& originals) {
  std::ifstream input(path);
  std::vector<Row> exported;
  std::string line, footer;
  while (std::getline(input, line)) {
    if (!line.empty() && line[0] == '[') {
      check(line.back() == ']', "partial exported row");
      std::string_view values(line.data() + 1, line.size() - 2);
      Row parsed;
      for (std::size_t i = 0; i < parsed.size(); ++i) {
        const auto comma = values.find(',');
        parsed[i] = number(values.substr(0, comma));
        check((comma == std::string_view::npos) == (i + 1 == parsed.size()),
              "exported row column count changed");
        if (comma != std::string_view::npos) values.remove_prefix(comma + 1);
      }
      exported.push_back(parsed);
    } else if (line.find("\"finished\":true") != std::string::npos) {
      footer = line;
    }
  }
  check(!footer.empty(), "successful retirement lacks finished footer");
  for (const auto& exported_row : exported)
    check(std::count(originals.begin(), originals.end(), exported_row) == 1,
          "export altered original date/source/route/epoch/value");
  auto unique = exported;
  std::sort(unique.begin(), unique.end());
  check(std::adjacent_find(unique.begin(), unique.end()) == unique.end(),
        "export duplicated an original record");
  std::array<Event, Capture::capacity + Capture::lifecycle_capacity> residual;
  const auto pending = file.capture()->take(residual);
  const auto status = file.status();
  check(status.exporter.has_value(), "missing terminal exporter receipt");
  std::cout << "case=" << path.stem().string() << " originals=" << originals.size()
            << " written=" << exported.size() << " ipc_dropped=" << status.transport_dropped_events
            << " residual_after_finish=" << pending << '\n';
  check(pending == 0, "finished collector left accepted original records in local Capture");
  check(field(footer, "recorded") == originals.size() &&
            field(footer, "writtenEvents") == exported.size() &&
            status.exporter->written_events == exported.size() &&
            field(footer, "transportDroppedEvents") == status.transport_dropped_events &&
            originals.size() == exported.size() + status.transport_dropped_events,
        "accepted originals were neither written nor accounted at IPC rejection");
}
}  // namespace
int main(int argc, char** argv) try {
  if (const auto result = File::collector_main(argc, argv)) return *result;
  char directory[] = "/tmp/hu-audio-file-XXXXXX";
  check(mkdtemp(directory) != nullptr, "private fixture directory failed");
  struct Remove {
    const char* path;
    ~Remove() { std::filesystem::remove_all(path); }
  } remove{directory};
  retention_failures(directory);
  for (const auto name : {"normal", "held-resumed", "burst", "full-lifecycle", "late"}) {
    const auto path = std::filesystem::path(directory) / (std::string(name) + ".jsonl");
    File file(path.string(), argv[0]);
    ready(file);
    std::vector<Row> originals;
    const auto record = [&](Kind kind, std::uint64_t value, bool lifecycle) {
      timespec acquired{};
      check(clock_gettime(CLOCK_MONOTONIC, &acquired) == 0, "original clock read failed");
      Event event{std::uint64_t(acquired.tv_sec) * 1000000000 + std::uint64_t(acquired.tv_nsec),
                  kind,
                  7,
                  11,
                  value % 3 + 1,
                  value,
                  unavailable,
                  value + 1,
                  unavailable};
      file.capture()->record(event, lifecycle);
      originals.push_back(row(event));
    };
    const bool burst =
        std::string_view(name) == "burst" || std::string_view(name) == "full-lifecycle";
    const bool held = std::string_view(name) == "held-resumed";
    if (held) stopped(file);
    record(Kind::stream_start, 0, true);
    const auto count = burst || held ? Capture::capacity : std::size_t(2);
    for (std::size_t i = 0; i < count; ++i) {
      record(Kind::datagram, i + 1, false);
    }
    const auto lifetimes = std::string_view(name) == "full-lifecycle"
                               ? Capture::lifecycle_capacity - 1
                               : std::size_t(1);
    for (std::size_t i = 0; i < lifetimes; ++i)
      record(Kind::stream_stop, count + i + 1, true);
    if (held) {
      // Fill the real stopped child's socket before resuming it. Original
      // records are all admitted before this nonblocking handoff can fail.
      // Four ordinary polls offer more bytes than either platform's admitted
      // socket capacity; the stopped collector cannot free that capacity.
      for (unsigned i = 0; i < 4; ++i)
        (void)file.poll();
      check(file.status().transport_dropped_events > 0, "stopped collector concealed IPC loss");
      check(kill(file.status().collector_pid, SIGCONT) == 0, "collector resume failed");
    }
    if (std::string_view(name) == "late") {
      file.capture()->stop();
      Point{file.capture(), 99, 98, 97}.observe(Kind::stream_start, 42, 0, 0, 0, true);
    }
    check(file.finish(), "real collector retirement failed");
    absent(file);
    Point{file.capture(), 99, 98, 97}.observe(Kind::stream_stop, 42, 0, 0, 0, true);
    compare_file(path, file, originals);
    check(file.finish(), "repeated finish changed terminal result");
  }
  {
    File file((std::filesystem::path(directory) / "cancelled.jsonl").string(), argv[0]);
    ready(file);
    stopped(file);
    Point{file.capture(), 7, 11, 1}.observe(Kind::stream_stop, 1, 0, 0, 0, true);
    check(!file.finish() && file.status().state == File::State::cancelled,
          "stopped child falsely finished");
    absent(file);
  }
  {
    File file((std::filesystem::path(directory) / "killed.jsonl").string(), argv[0]);
    ready(file);
    stopped(file);
    check(kill(file.status().collector_pid, SIGKILL) == 0, "collector kill failed");
    Point{file.capture(), 7, 11, 1}.observe(Kind::stream_stop, 1, 0, 0, 0, true);
    check(!file.finish() && file.status().state == File::State::failed,
          "failed child falsely finished");
    absent(file);
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
