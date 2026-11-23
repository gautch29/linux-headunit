// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_observation.hpp"
#include "audio_observation_storage_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <sys/wait.h>

using namespace headunit::audio_observation;
using audio_storage_fixture::check;
namespace {
using Clock = std::chrono::steady_clock;
std::string row(const Event& e) {
  return "[" + std::to_string(e.date_ns) + "," + std::to_string(std::uint64_t(e.kind)) + "," +
         std::to_string(e.source) + "," + std::to_string(e.route) + "," + std::to_string(e.epoch) +
         "," + std::to_string(e.a) + "," + std::to_string(e.b) + "," + std::to_string(e.c) + "," +
         std::to_string(e.d) + "]\n";
}
Event acquired(std::uint64_t a, std::uint64_t c) {
  timespec date{};
  check(clock_gettime(CLOCK_MONOTONIC, &date) == 0, "original acquisition failed");
  return {std::uint64_t(date.tv_sec) * 1000000000 + std::uint64_t(date.tv_nsec),
          Kind::supply,
          9007199254740993,
          9007199254740995,
          7,
          a,
          unavailable,
          c,
          unavailable};
}
void running(File& file) {
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (file.status().state == File::State::starting && Clock::now() < deadline) {
    (void)file.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(file.status().state == File::State::running, "collector did not open normal trace");
}
void written(File& file, std::size_t count) {
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < deadline) {
    (void)file.poll();
    const auto status = file.status();
    if (status.exporter && status.exporter->written_events == count) return;
    check(status.state != File::State::failed, "baseline rows failed before exhaustion");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("baseline rows were not acknowledged");
}
std::uint64_t digits(unsigned width) {
  std::uint64_t value = 1;
  for (unsigned i = 1; i < width; ++i)
    value *= 10;
  return value;
}
void align(File& file, const std::filesystem::path& path, std::vector<Event>& originals) {
  // Supply genuine acquired synthetic rows whose encoded lengths end on a
  // page boundary. No external writer modifies the collector's trace.
  const auto page = std::uint64_t(sysconf(_SC_PAGESIZE));
  check(page > 0 && page <= 65536, "fixture page size invalid");
  const auto bytes = std::filesystem::file_size(path);
  const auto base = row(acquired(1, 1)).size();
  auto needed = page - bytes % page;
  std::uint64_t count = 0;
  while (!(count = (needed + base + 35) / (base + 36)) || count * base > needed)
    needed += page;
  check(count <= 512, "alignment supply exceeded fixture bound");
  auto extra = needed - count * base;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto width = std::min<std::uint64_t>(extra, 36);
    const auto a = std::min<std::uint64_t>(width, 18);
    auto event = acquired(digits(a + 1), digits(width - a + 1));
    check(row(event).size() == base + width, "acquisition digit width changed during fixture");
    file.capture()->record(event);
    originals.push_back(event);
    extra -= width;
    if (i % 64 == 63) written(file, originals.size());
  }
  written(file, originals.size());
  check(std::filesystem::file_size(path) % page == 0, "trace did not reach actual page boundary");
}
void terminal(File& file) {
  check(!file.finish(), "exhausted collector falsely finished");
  const auto before = file.status();
  check(before.state == File::State::failed && before.reaped && before.exporter,
        "exhausted collector lacks failure/reaped/terminal receipt");
  check(kill(before.collector_pid, 0) == -1 && errno == ESRCH,
        "exhausted collector survived waitpid retirement");
  int result = 0;
  check(waitpid(before.collector_pid, &result, WNOHANG) == -1 && errno == ECHILD,
        "collector was not reaped by its owner");
  const auto sealed = file.capture()->counters().recorded;
  Point{file.capture(), 99, 98, 97}.observe(Kind::supply, 1, 2, 3, 4);
  check(!file.poll() && !file.finish() && !file.finish(), "terminal failure replayed success");
  const auto after = file.status();
  check(after.state == before.state && after.reaped &&
            after.exporter->error == before.exporter->error &&
            after.exporter->written_events == before.exporter->written_events &&
            file.capture()->counters().recorded == sealed,
        "late producer or repeated finish changed terminal evidence");
}
}  // namespace
int main(int argc, char** argv) try {
  if (const auto result = File::collector_main(argc, argv)) return *result;
  if (!std::getenv("HEADUNIT_PCM_STORAGE_FIXTURE")) {
    std::cout << "SKIP: explicit private <=1MiB Docker tmpfs required\n";
    return 77;
  }
  const auto evidence = audio_storage_fixture::receipts();
  for (const auto name : {"normal", "header", "batch", "partial-batch", "footer"}) {
    audio_storage_fixture::Scratch scratch;
    const auto path = scratch.root / "trace.jsonl";
    const std::string mode = name;
    if (mode == "header") scratch.fill();
    File file(path.string(), argv[0]);
    std::vector<Event> originals;
    if (mode != "header") {
      running(file);
      auto event = acquired(0, 480);
      file.capture()->record(event);
      originals.push_back(event);
      written(file, originals.size());
      if (mode == "batch" || mode == "footer") align(file, path, originals);
      if (mode != "normal") scratch.fill();
      if (mode == "batch" || mode == "partial-batch") {
        for (unsigned i = 0; i < 64; ++i) {
          event = acquired(i, 480);
          file.capture()->record(event);
          originals.push_back(event);
        }
      }
    }
    if (mode == "normal") {
      check(file.finish() && file.status().reaped && file.finish(), "normal collector failed");
    } else {
      terminal(file);
      const auto exported = *file.status().exporter;
      check(exported.error == (mode == "partial-batch" ? EIO : ENOSPC),
            "collector did not report real exhausted write result");
      check(exported.partial_bytes == 0 || mode == "partial-batch",
            "unexpected partial-byte receipt");
      if (mode == "partial-batch")
        check(exported.partial_bytes > 0, "exhausted tmpfs did not produce a partial write");
    }
    const auto bytes = audio_storage_fixture::read(path);
    const auto status = file.status();
    check((bytes.find("\"finished\":true") != std::string::npos) == (mode == "normal"),
          "failed trace contains false successful footer");
    if (mode == "header") check(bytes.empty(), "failed header supplied false trace coverage");
    std::string complete;
    for (const auto& event : originals)
      complete += row(event);
    const auto begin = bytes.find('\n');
    if (begin != std::string::npos) {
      const auto rows = bytes.substr(begin + 1);
      if (mode == "normal")
        check(rows.starts_with(complete), "normal export changed original numeric rows");
      else
        check(complete.starts_with(rows), "partial export changed or replayed original rows");
    }
    audio_retention_fixture::write_new(evidence / (mode + ".trace.jsonl"), bytes);
    audio_retention_fixture::write_new(evidence / (mode + ".originals.jsonl"), complete);
    const auto exported = *status.exporter;
    const auto receipt =
        "{\"case\":\"" + mode + "\",\"finished\":" + (mode == "normal" ? "true" : "false") +
        ",\"coverageComplete\":" + (mode == "normal" ? "true" : "null") +
        ",\"reaped\":true,\"probeErrno\":" + std::to_string(mode == "normal" ? 0 : ENOSPC) +
        ",\"exportError\":" + std::to_string(exported.error) +
        ",\"partialBytes\":" + std::to_string(exported.partial_bytes) +
        ",\"writtenEvents\":" + std::to_string(exported.written_events) +
        ",\"originalEvents\":" + std::to_string(originals.size()) + "}";
    audio_retention_fixture::write_new(evidence / (mode + ".json"), receipt + '\n');
    scratch.retire(evidence);
    check(!std::filesystem::exists(scratch.root), "verified fixture originals survived retirement");
    std::cout << receipt << '\n';
  }
  // Real failed retention must preserve originals on unwind. The fallback
  // remains in the owned container layer until the host retains it.
  std::filesystem::path preserved;
  std::string original;
  {
    audio_storage_fixture::Scratch scratch;
    preserved = scratch.root;
    File file((scratch.root / "trace.jsonl").string(), argv[0]);
    running(file);
    const auto event = acquired(0, 480);
    file.capture()->record(event);
    written(file, 1);
    check(file.finish() && file.status().reaped, "unwind baseline did not finish");
    original = audio_retention_fixture::read(scratch.root / "trace.jsonl");
    scratch.fill();
    bool failed = false;
    try {
      scratch.retire(scratch.root);
    } catch (const std::runtime_error&) {
      failed = true;
    }
    check(failed && std::filesystem::exists(scratch.root) &&
              audio_retention_fixture::read(scratch.root / "trace.jsonl") == original,
          "failed exhausted retention erased or changed originals");
    audio_retention_fixture::write_new(evidence / "retention-unwind.original.trace.jsonl",
                                       original);
  }
  check(std::filesystem::exists(preserved) &&
            audio_retention_fixture::read(preserved / "trace.jsonl") == original,
        "no-throw unwind removed unretained originals");
  audio_retention_fixture::write_new(evidence / "retention-unwind.json",
                                     "{\"failedCopyPreservedOriginal\":true,"
                                     "\"unwindPreservedOriginal\":true,"
                                     "\"verifiedPersistentFallbackRequired\":true}\n");
  // The deliberate failure kept the mount full. Only externally retained,
  // closed and byte-verified recovery may release it for the next fixture.
  (void)audio_retention_fixture::retain(preserved, evidence, {"trace.jsonl"}, true);
  check(!std::filesystem::exists(preserved), "verified unwind recovery left the tmpfs full");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
