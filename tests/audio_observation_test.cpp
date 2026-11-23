// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/audio_observation.hpp"
#include "headunit/audio_playback.hpp"
#include "headunit/audio_rtp_decoder.hpp"
#include "audio_pipeline_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <spawn.h>

extern char** environ;
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace headunit;
using namespace headunit::audio_observation;
namespace {
volatile std::sig_atomic_t group_stopped = 0;
void stop_group_fixture(int) {
  group_stopped = 1;
}
void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
std::vector<Event> drain(const std::shared_ptr<Capture>& capture) {
  std::array<Event, 512> batch;
  std::vector<Event> result;
  while (const auto count = capture->take(batch))
    result.insert(result.end(), batch.begin(), batch.begin() + count);
  return result;
}
std::vector<AudioMixer::Block> decode(const std::shared_ptr<Capture>& capture) {
  std::vector<AudioMixer::Block> result;
  AudioRtpDecoder decoder(200, AudioRtpDecoder::Format::pcm_48000_stereo, {capture, 1, 2, 1});
  for (unsigned n = 0; n < 12; ++n) {
    const auto time = n * 480;
    std::vector<std::uint8_t> packet{0x80,
                                     96,
                                     0,
                                     std::uint8_t(n),
                                     std::uint8_t(time >> 24),
                                     std::uint8_t(time >> 16),
                                     std::uint8_t(time >> 8),
                                     std::uint8_t(time),
                                     1,
                                     2,
                                     3,
                                     4};
    for (unsigned i = 0; i < 960; ++i) {
      const auto value = std::uint16_t(n * 100 + i);
      packet.push_back(value >> 8);
      packet.push_back(value);
    }
    check(decoder.submit(packet), "production PCM submit failed");
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (result.size() < 12 && std::chrono::steady_clock::now() < deadline) {
    const auto blocks = decoder.take();
    result.insert(result.end(), blocks.begin(), blocks.end());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto counts = decoder.diagnostics();
  check(result.size() == 12 && counts.submitted == 12 && !counts.input_backpressure &&
            !counts.discarded_pcm_frames && counts.jitter && !counts.jitter->considered_lost,
        "production PCM samples/admission changed");
  return result;
}
std::vector<AudioOutput::Receipt> play(const std::shared_ptr<Capture>& capture) {
  std::mutex mutex;
  std::vector<AudioOutput::Receipt> receipts;
  {
    AudioPlayback playback("fakesink", "",
                           [&](const auto& receipt) {
                             std::lock_guard lock(mutex);
                             receipts.push_back(receipt);
                           },
                           {}, {capture, 0, 2});
    const auto channel = playback.attach(true, {capture, 1, 2, 1});
    for (unsigned i = 0; i < 6; ++i) {
      AudioMixer::Block block;
      block.fill(1000 + i * 100);
      check(playback.submit(channel, block, true, {}, {capture, 1, 2, i < 3 ? 1u : 2u}),
            "production channel rejected an admitted six-block burst");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock(mutex);
        if (receipts.size() == 6) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(playback.status().accepted == 6, "capture changed playback acceptance");
    playback.detach(channel);
    bool retired = false;
    try {
      (void)playback.submit(channel, {}, true, {});
    } catch (const std::runtime_error&) {
      retired = true;
    }
    check(retired, "retired channel accepted PCM with diagnostics");
  }
  return receipts;
}
void compare(const std::vector<AudioOutput::Receipt>& a,
             const std::vector<AudioOutput::Receipt>& b) {
  check(a.size() == 6 && b.size() == a.size(), "capture changed sink-pad receipt count");
  for (std::size_t i = 0; i < a.size(); ++i) {
    check(a[i].presentation_ns - a[0].presentation_ns == i * 10000000 &&
              b[i].presentation_ns - b[0].presentation_ns == i * 10000000 &&
              a[i].duration_ns == 10000000 && b[i].duration_ns == a[i].duration_ns &&
              a[i].discontinuity == (i == 0) && b[i].discontinuity == a[i].discontinuity &&
              a[i].bytes == sizeof(AudioMixer::Block) && b[i].bytes == a[i].bytes &&
              a[i].peak_sample == 1000 + i * 100 && b[i].peak_sample == a[i].peak_sample,
          "diagnostics changed actual PCM, sample-time spacing or discontinuity policy");
  }
}
}  // namespace
int main(int argc, char** argv) try {
  if (const auto result = File::collector_main(argc, argv)) return *result;
  if (argc == 3 && std::string_view(argv[1]) == "--collector-parent-fixture") {
    File owned(argv[2], argv[0]);
    for (;;) {
      (void)owned.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--collector-group-fixture") {
    check(setsid() == getpid(), "fixture could not own its receiver process group");
    std::signal(SIGTERM, stop_group_fixture);
    std::signal(SIGINT, stop_group_fixture);
    File owned(argv[2], argv[0]);
    Point point{owned.capture(), 1, 2, 1};
    point.observe(Kind::stream_start, 0, 0, 0, 0, true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (owned.status().state != File::State::running &&
           std::chrono::steady_clock::now() < deadline) {
      (void)owned.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(owned.status().state == File::State::running, "group fixture collector did not start");
    const auto collector = owned.status().collector_pid;
    check(getsid(collector) == getsid(0), "collector left the supervised receiver session");
    check(getpgid(collector) == getpgrp(), "collector detached from the owned receiver group");
    {
      std::ofstream ready(std::string(argv[2]) + ".ready");
      ready << collector << '\n';
    }
    while (!group_stopped && std::chrono::steady_clock::now() < deadline) {
      (void)owned.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(group_stopped, "receiver process-group signal did not arrive");
    point.observe(Kind::stream_stop, 0, 0, 0, 0, true);
    const auto finished = owned.finish();
    const auto state = owned.status();
    {
      std::ofstream result(std::string(argv[2]) + ".result");
      result << "finished=" << finished << " reaped=" << state.reaped
             << " state=" << int(state.state) << '\n';
    }
    check(finished && state.reaped && state.state == File::State::finished,
          "receiver process-group termination lost the supervised collector footer");
    return 0;
  }
  auto capture = std::make_shared<Capture>();
  check(decode(nullptr) == decode(capture), "enabled capture changed exact decoded PCM");
  compare(play(nullptr), play(capture));
  const auto events = drain(capture);
  for (const auto kind :
       {Kind::decoder_start, Kind::decoder_stop, Kind::jitter_enter, Kind::jitter_release,
        Kind::decoded_buffer, Kind::decoded_pull, Kind::pcm_pull, Kind::supply, Kind::channel_pop,
        Kind::channel_detach, Kind::output_accept, Kind::route_stop})
    check(
        std::any_of(events.begin(), events.end(),
                    [kind](const auto& e) { return e.kind == kind && e.date_ns && e.route == 2; }),
        "missing actual dated production boundary");
  for (const auto epoch : {1u, 2u})
    check(std::count_if(events.begin(), events.end(),
                        [epoch](const auto& e) {
                          return e.kind == Kind::channel_pop && e.epoch == epoch;
                        }) == 3,
          "queued PCM lost its original epoch");
  check(std::all_of(events.begin(), events.end(),
                    [](const auto& e) {
                      return e.kind != Kind::output_accept || !e.d || e.a - e.b == 60000000;
                    }),
        "diagnostics changed the production startup reserve");
  // Model one delayed output turn using the existing real Gst clock fixture.
  {
    audio_fixture::OutputBus actual;
    AudioOutput output("fakesink", "", {}, {capture, 0, 3});
    check(output.submit({}), "diagnosed output initial PCM failed");
    actual.advance_running_time(100 * GST_MSECOND);
    output.poll();
    output.poll();
    check(output.underruns() == 1 && output.starved(),
          "capture concealed or multiplied exhaustion");
    check(output.submit({}) && output.discontinuities() == 2,
          "capture changed fresh resume origin");
    actual.error("diagnostic fault fixture");
    bool failed = false;
    try {
      output.poll();
    } catch (const std::runtime_error&) {
      failed = true;
    }
    check(failed, "diagnosed output lost terminal bus failure");
  }
  const auto exhausted = drain(capture);
  check(std::count_if(exhausted.begin(), exhausted.end(),
                      [](const auto& e) {
                        return e.kind == Kind::output_exhausted && e.a > e.b && e.d == 1;
                      }) == 1,
        "exhaustion lacks original clock pair or duplicate detection");
  // Finite packet budget is independent of the fixed retirement reservoir.
  auto finite = std::make_shared<Capture>(1);
  Point point{finite, 4, 5, 6};
  point.observe(Kind::datagram, 8);
  point.observe(Kind::datagram, 9);
  point.observe(Kind::stream_stop, 0, 0, 0, 0, true);
  check(finite->counters().budget == 1 && drain(finite).size() == 2,
        "budget exhaustion lost explicit retirement");
  auto overflow = std::make_shared<Capture>();
  Point overflow_point{overflow, 1, 2, 1};
  for (std::size_t i = 0; i < Capture::capacity + 1; ++i)
    overflow_point.observe(Kind::datagram);
  check(overflow->counters().overflow == 1 && drain(overflow).size() == Capture::capacity,
        "fixed diagnostic ring overwrote or hid overflow");
  for (std::size_t i = 0; i < Capture::lifecycle_capacity + 1; ++i)
    overflow_point.observe(Kind::stream_stop, 0, 0, 0, 0, true);
  check(overflow->counters().lifecycle_overflow == 1 &&
            drain(overflow).size() == Capture::lifecycle_capacity,
        "fixed lifecycle reservoir silently overwrote evidence");
  finite->stop();
  const auto before = finite->counters().recorded;
  point.observe(Kind::stream_start, 0, 0, 0, 0, true);
  check(finite->counters().recorded == before, "retired capture accepted new events");
  // Stop must seal against concurrent streaming owners before a final export.
  auto concurrent = std::make_shared<Capture>();
  std::array<std::jthread, 4> producers;
  for (auto& producer : producers)
    producer = std::jthread([concurrent](std::stop_token stop) {
      Point source{concurrent, 1, 2, 1};
      while (!stop.stop_requested())
        source.observe(Kind::datagram, 12);
    });
  const auto seal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!concurrent->counters().recorded && std::chrono::steady_clock::now() < seal_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  concurrent->stop();
  const auto sealed = concurrent->counters().recorded;
  for (auto& producer : producers)
    producer.request_stop();
  for (auto& producer : producers)
    producer.join();
  check(sealed && concurrent->counters().recorded == sealed,
        "final export raced a surviving streaming producer");
  char directory[] = "/tmp/hu-audio-trace-XXXXXX";
  check(mkdtemp(directory), "private fixture directory failed");
  for (const auto signum : {SIGTERM, SIGINT}) {
    const auto group_path =
        (std::filesystem::path(directory) / ("group-" + std::to_string(signum) + ".jsonl"))
            .string();
    std::array<char*, 4> arguments{argv[0], const_cast<char*>("--collector-group-fixture"),
                                   const_cast<char*>(group_path.c_str()), nullptr};
    pid_t owner = -1;
    check(posix_spawn(&owner, argv[0], nullptr, nullptr, arguments.data(), environ) == 0,
          "receiver process-group fixture spawn failed");
    struct RetireGroup {
      pid_t owner;
      ~RetireGroup() {
        if (owner > 0) {
          (void)kill(-owner, SIGKILL);
          (void)kill(owner, SIGKILL);
          (void)waitpid(owner, nullptr, 0);
        }
      }
    } retire{owner};
    int collector = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (collector < 0 && std::chrono::steady_clock::now() < deadline) {
      std::ifstream ready(group_path + ".ready");
      if (!(ready >> collector)) {
        collector = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    check(collector > 0, "group fixture never admitted a collector");
    check(kill(collector, SIGSTOP) == 0, "group fixture could not hold the collector");
    check(kill(-owner, signum) == 0, "receiver group signal failed");
    check(kill(collector, SIGCONT) == 0, "group fixture could not release the collector");
    int result = 0;
    pid_t reaped = 0;
    while ((reaped = waitpid(owner, &result, WNOHANG)) == 0 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (reaped == owner) retire.owner = -1;
    check(reaped == owner && WIFEXITED(result) && WEXITSTATUS(result) == 0,
          "actual receiver-group retirement failed to finish and reap its collector");
    std::ifstream input(group_path);
    std::string contents((std::istreambuf_iterator<char>(input)), {});
    check(contents.find("\"finished\":true") != std::string::npos,
          "group retirement returned without a completed original export");
    errno = 0;
    check(kill(collector, 0) == -1 && errno == ESRCH,
          "group retirement left an actual collector process");
  }
  const auto path = std::filesystem::path(directory) / "trace.jsonl";
  {
    File file(path.string(), argv[0]);
    const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (file.status().state == File::State::starting &&
           std::chrono::steady_clock::now() < ready) {
      (void)file.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(file.status().state == File::State::running, "collector did not open private file");
    Point{file.capture(), 1, 2, 3}.observe(Kind::stream_stop, 0, 0, 0, 0, true);
    check(file.finish(), "private exporter failed");
    struct stat mode{};
    check(!stat(path.c_str(), &mode) && (mode.st_mode & 0777) == 0600,
          "private trace file permissions changed");
    File duplicate(path.string(), argv[0]);
    check(!duplicate.finish() && duplicate.status().reaped && duplicate.status().exporter &&
              duplicate.status().exporter->error == EEXIST,
          "exporter overwrote existing evidence");
  }
  std::ifstream file(path);
  std::string bytes((std::istreambuf_iterator<char>(file)), {});
  check(bytes.find("CLOCK_MONOTONIC") != std::string::npos &&
            bytes.find("\"finished\":true") != std::string::npos,
        "private trace lacks final receipt");
  chmod(directory, 0755);
  {
    File public_file((std::filesystem::path(directory) / "public.jsonl").string(), argv[0]);
    check(!public_file.finish() && public_file.status().reaped && public_file.status().exporter &&
              public_file.status().exporter->error == EACCES,
          "exporter accepted a public directory");
  }
  chmod(directory, 0700);
  const auto link = std::filesystem::path(directory) / "link.jsonl";
  std::filesystem::create_symlink(path, link);
  {
    File linked(link.string(), argv[0]);
    check(!linked.finish() && linked.status().reaped, "exporter followed a replaced path");
  }
  {
    rlimit original{};
    check(getrlimit(RLIMIT_FSIZE, &original) == 0, "file fault limits unavailable");
    struct Restore {
      rlimit limit;
      ~Restore() { (void)setrlimit(RLIMIT_FSIZE, &limit); }
    } restore{original};
    auto limited = original;
    limited.rlim_cur = 0;
    check(setrlimit(RLIMIT_FSIZE, &limited) == 0, "file fault limit failed");
    File failed_file((std::filesystem::path(directory) / "failed.jsonl").string(), argv[0]);
    check(!failed_file.finish() && failed_file.status().reaped && failed_file.status().exporter &&
              failed_file.status().exporter->error == EFBIG,
          "actual failed export was counted complete");
    const auto recorded = failed_file.capture()->counters().recorded;
    Point{failed_file.capture(), 1, 2, 3}.observe(Kind::datagram, 13);
    check(failed_file.capture()->counters().recorded == recorded, "failed exporter kept recording");
  }
  {
    File blocked((std::filesystem::path(directory) / "blocked.jsonl").string(), argv[0]);
    const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (blocked.status().state == File::State::starting &&
           std::chrono::steady_clock::now() < ready) {
      (void)blocked.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(blocked.status().state == File::State::running,
          "blocked collector fixture startup failed");
    check(kill(blocked.status().collector_pid, SIGSTOP) == 0, "actual collector stop failed");
    Point producer{blocked.capture(), 1, 2, 1};
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 10000; ++i) {
      producer.observe(Kind::datagram, i);
      if (i % 64 == 63) (void)blocked.poll();
    }
    check(blocked.status().transport_dropped_events > 0,
          "stalled collector did not report actual IPC backpressure loss");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
          "stalled collector blocked production record/handoff");
    compare(play(nullptr), play(blocked.capture()));
    const auto pid = blocked.status().collector_pid;
    check(!blocked.finish() && blocked.status().state == File::State::cancelled &&
              blocked.status().reaped,
          "stalled collector retirement was falsely completed or not reaped");
    check(kill(pid, 0) == -1 && errno == ESRCH, "retirement left a surviving collector");
    // A broken collector also leaves normal PCM/time semantics intact.
    File broken((std::filesystem::path(directory) / "broken.jsonl").string(), argv[0]);
    check(kill(broken.status().collector_pid, SIGKILL) == 0,
          "actual collector failure injection failed");
    (void)broken.poll();
    compare(play(nullptr), play(broken.capture()));
    check(!broken.finish() && broken.status().reaped, "broken exporter retirement did not reap");
  }
  {
    auto hot = std::make_shared<Capture>();
    Point source{hot, 1, 2, 1};
    std::array<Event, Capture::capacity> batch;
    std::uint64_t record_ns = 0;
    constexpr unsigned rounds = 12;
    for (unsigned round = 0; round < rounds; ++round) {
      const auto start = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < Capture::capacity; ++i)
        source.observe(Kind::datagram, i);
      record_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
      (void)hot->take(batch);  // Outside the hot-record measurement.
    }
    File measured_file((std::filesystem::path(directory) / "measured.jsonl").string(), argv[0]);
    const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (measured_file.status().state == File::State::starting &&
           std::chrono::steady_clock::now() < ready) {
      (void)measured_file.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(measured_file.status().state == File::State::running,
          "measurement collector startup failed");
    Point measured_point{measured_file.capture(), 1, 2, 1};
    std::uint64_t handoff_ns = 0;
    for (unsigned round = 0; round < 128; ++round) {
      for (unsigned i = 0; i < 64; ++i)
        measured_point.observe(Kind::datagram, i);
      const auto start = std::chrono::steady_clock::now();
      (void)measured_file.poll();
      handoff_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    }
    check(measured_file.finish() && measured_file.status().exporter,
          "measurement export incomplete");
    const auto measured = *measured_file.status().exporter;
    check(measured.written_events > 0 &&
              measured_file.capture()->counters().recorded ==
                  measured.written_events + measured_file.status().transport_dropped_events,
          "measurement hid IPC loss or incomplete exported records");
    std::cout << "component_hot_record_ns_per_event=" << record_ns / (rounds * Capture::capacity)
              << " parent_handoff_ns_per_64_record_poll=" << handoff_ns / 128
              << " collector_encoding_ns_per_written_event="
              << measured.encoding_ns / measured.written_events
              << " collector_write_ns_per_call=" << measured.write_ns / measured.write_calls
              << " written_events=" << measured.written_events
              << " ipc_dropped=" << measured_file.status().transport_dropped_events
              << " written_bytes=" << measured.written_bytes << '\n';
  }
  {
    int previous = 0;
    check(prctl(PR_GET_CHILD_SUBREAPER, &previous) == 0 && prctl(PR_SET_CHILD_SUBREAPER, 1) == 0,
          "parent-death fixture reaper unavailable");
    const auto path = (std::filesystem::path(directory) / "parent-death.jsonl").string();
    std::array<char*, 4> arguments{argv[0], const_cast<char*>("--collector-parent-fixture"),
                                   const_cast<char*>(path.c_str()), nullptr};
    pid_t owner = -1;
    check(posix_spawn(&owner, argv[0], nullptr, nullptr, arguments.data(), environ) == 0,
          "actual collector parent fixture spawn failed");
    struct Retire {
      pid_t owner;
      int previous;
      ~Retire() {
        if (owner > 0) {
          (void)kill(owner, SIGKILL);
          (void)waitpid(owner, nullptr, 0);
        }
        (void)prctl(PR_SET_CHILD_SUBREAPER, previous);
      }
    } retire{owner, previous};
    int collector = -1;
    const auto ready = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (collector < 0 && std::chrono::steady_clock::now() < ready) {
      std::ifstream input(path);
      std::string header;
      std::getline(input, header);
      const auto at = header.find("\"collectorPid\":");
      if (at != std::string::npos)
        collector = std::stoi(header.substr(at + 15));
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(collector > 0, "actual collector parent-death startup missing");
    check(kill(owner, SIGKILL) == 0, "actual owner crash failed");
    check(waitpid(owner, nullptr, 0) == owner, "actual crashed owner not reaped");
    retire.owner = -1;
    int result = 0;
    pid_t reaped = 0;
    const auto dead = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((reaped = waitpid(collector, &result, WNOHANG)) == 0 &&
           std::chrono::steady_clock::now() < dead)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(reaped == collector && WIFSIGNALED(result) && WTERMSIG(result) == SIGKILL,
          "parent death left a surviving collector");
  }
  std::filesystem::remove_all(directory);
  // Component microbenchmark only; real phone/GPU overhead remains unqualified.
  auto measured = std::make_shared<Capture>();
  const auto start = std::chrono::steady_clock::now();
  std::array<Event, 1024> batch;
  Point measured_point{measured, 1, 2, 1};
  for (unsigned i = 0; i < 100000; ++i) {
    measured_point.observe(Kind::datagram, i);
    if ((i + 1) % batch.size() == 0) (void)measured->take(batch);
  }
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
          .count();
  std::cout << "component_capture_ns_per_event=" << ns / 100000
            << " recorded=" << measured->counters().recorded << " event_bytes=" << sizeof(Event)
            << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
