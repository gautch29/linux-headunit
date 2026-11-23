// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/carplay_media_stream.hpp>
#include <headunit/hap_crypto.hpp>
#include "audio_pipeline_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
using namespace headunit::audio_control;
void check(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}
std::vector<std::vector<std::uint8_t>> frames(const char* path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(file), {});
  std::vector<std::vector<std::uint8_t>> result;
  for (std::size_t at = 0; at + 7 <= bytes.size();) {
    const auto length = (std::size_t(bytes[at + 3] & 3) << 11) | (std::size_t(bytes[at + 4]) << 3) |
                        (bytes[at + 5] >> 5);
    const auto header = (bytes[at + 1] & 1) ? 7u : 9u;
    check(bytes[at] == 0xff && length > header && at + length <= bytes.size(),
          "invalid AAC fixture");
    result.emplace_back(bytes.begin() + at + header, bytes.begin() + at + length);
    at += length;
  }
  check(result.size() >= 20, "AAC fixture is too short");
  return result;
}
class Sender {
 public:
  Sender(const std::vector<std::vector<std::uint8_t>>& frames, std::uint16_t port,
         std::span<const std::uint8_t> shared)
      : frames_(frames),
        key_(headunit::hap_hkdf_sha512(shared, "DataStream-Salt43",
                                       "DataStream-Output-Encryption-Key")) {
    fd_ = socket(AF_INET6, SOCK_DGRAM, 0);
    check(fd_ >= 0, "test UDP socket failed");
    if (fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) {
      close(fd_);
      throw std::runtime_error("test UDP socket flags failed");
    }
    target_.sin6_family = AF_INET6;
    target_.sin6_addr = in6addr_loopback;
    target_.sin6_port = htons(port);
    worker_ = std::jthread([this](std::stop_token stop) {
      try {
        std::uint16_t sequence = 1;
        std::uint32_t timestamp = 1024;
        std::uint64_t nonce_value = 0;
        auto next = Clock::now();
        std::size_t index = 0;
        while (!stop.stop_requested()) {
          std::array<std::uint8_t, 12> header{0x80,
                                              96,
                                              std::uint8_t(sequence >> 8),
                                              std::uint8_t(sequence),
                                              std::uint8_t(timestamp >> 24),
                                              std::uint8_t(timestamp >> 16),
                                              std::uint8_t(timestamp >> 8),
                                              std::uint8_t(timestamp),
                                              1,
                                              2,
                                              3,
                                              4};
          std::array<std::uint8_t, 8> nonce{};
          for (std::size_t i = 0; i < 8; ++i)
            nonce[i] = std::uint8_t(nonce_value >> (i * 8));
          const std::string text(reinterpret_cast<const char*>(nonce.data()), nonce.size());
          const auto sealed = headunit::hap_encrypt(key_, text, frames_[index++ % frames_.size()],
                                                    std::span(header).subspan(4, 8));
          std::vector<std::uint8_t> packet(header.begin(), header.end());
          packet.insert(packet.end(), sealed.begin(), sealed.end());
          packet.insert(packet.end(), nonce.begin(), nonce.end());
          check(sendto(fd_, packet.data(), packet.size(), 0,
                       reinterpret_cast<const sockaddr*>(&target_),
                       sizeof(target_)) == static_cast<ssize_t>(packet.size()),
                "test UDP send failed");
          ++sequence;
          timestamp += 1024;
          ++nonce_value;
          next += std::chrono::nanoseconds(1024LL * 1000000000 / 48000);
          std::this_thread::sleep_until(next);
        }
      } catch (...) {
        failure_ = std::current_exception();
      }
    });
  }
  ~Sender() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
    close(fd_);
  }
  void finish() {
    worker_.request_stop();
    worker_.join();
    if (failure_) std::rethrow_exception(failure_);
  }

 private:
  const std::vector<std::vector<std::uint8_t>>& frames_;
  headunit::HapKey32 key_{};
  int fd_ = -1;
  sockaddr_in6 target_{};
  std::jthread worker_;
  std::exception_ptr failure_;
};
struct Samples {
  std::mutex mutex;
  std::vector<unsigned> peaks;
  void receipt(const headunit::AudioOutput::Receipt& receipt) {
    std::lock_guard lock(mutex);
    // Production sink-pad PCM, after decryption, decoding, gain and mute.
    peaks.push_back(receipt.peak_sample.value_or(65536));
  }
  std::vector<unsigned> fresh(headunit::CarplayMediaStream& receiver) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(120));  // Prior accepted output may drain at old gain.
    std::size_t start;
    {
      std::lock_guard lock(mutex);
      start = peaks.size();
    }
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
      check(receiver.status().fault.empty(), "audio worker faulted");
      {
        std::lock_guard lock(mutex);
        if (peaks.size() >= start + 12) return {peaks.begin() + start, peaks.begin() + start + 12};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("audio sink did not receive fresh PCM");
  }
};
unsigned median(std::vector<unsigned> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}
void apply(headunit::CarplayMediaStream& receiver, Command command) {
  const auto controls = receiver.audio_control();
  check(controls->request(command) == RequestResult::accepted, "audio command was not accepted");
  const auto deadline = Clock::now() + std::chrono::seconds(1);
  while (Clock::now() < deadline) {
    check(receiver.status().fault.empty(), "audio worker faulted applying controls");
    if (controls->snapshot().applied == command) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("audio owner did not confirm mixer controls");
}
}  // namespace
int main(int argc, char** argv) try {
  check(argc == 2, "expected AAC fixture");
  const auto fixture = frames(argv[1]);
  const std::array<std::uint8_t, 32> shared{};
  sockaddr_in6 phone{};
  phone.sin6_family = AF_INET6;
  phone.sin6_addr = in6addr_loopback;
  Samples samples;
  std::shared_ptr<Mailbox> retired;
  {
    headunit::CarplayMediaStream receiver(shared, 43, phone, "fakesink", 60, {}, Level{40, false},
                                          [&](const auto& receipt) { samples.receipt(receipt); });
    retired = receiver.audio_control();
    check(retired->snapshot().available, "initialized audio worker unavailable");
    check(retired->snapshot().applied == Command(0, {40, false}),
          "initial configured level was lost");
    Sender sender(fixture, receiver.data_port(), shared);
    const auto initial = median(samples.fresh(receiver));
    check(initial > 100 && initial < 32768, "initial PCM lacks bounded nonzero samples");
    const auto progress = retired->snapshot().output;
    check(progress && progress->accepted_blocks > 0 &&
              progress->sampled_ns <= headunit::audio_control::monotonic_time_ns(),
          "actual AAC output did not publish dated accepted-block progress");
    apply(receiver, {1, {100, false}});
    const auto full = median(samples.fresh(receiver));
    check(full > initial * 2 && full < initial * 3,
          "initial configured gain was not applied to decoded PCM");
    apply(receiver, {2, {50, false}});
    const auto half = median(samples.fresh(receiver));
    check(half > full * 0.4 && half < full * 0.6, "half gain did not scale decoded PCM");
    check(receiver.request_audio_focus({.25, 0}), "phone duck was not accepted");
    const auto ducked = median(samples.fresh(receiver));
    check(ducked > half * .2 && ducked < half * .3 &&
              retired->snapshot().applied.level == Level{50, false},
          "phone focus changed user gain or failed to scale actual decoded PCM");
    check(receiver.request_audio_focus({1., 0}), "phone release was not accepted");
    const auto released = median(samples.fresh(receiver));
    check(released > half * .8 && released < half * 1.2,
          "phone release did not restore user-level PCM");
    check(receiver.set_media_resource_enabled(false), "media resource denial rejected");
    check(receiver.request_audio_focus({1., 0}), "unduck during resource denial rejected");
    const auto denied = samples.fresh(receiver);
    check(std::all_of(denied.begin(), denied.end(), [](auto peak) { return peak == 0; }) &&
              retired->snapshot().applied.level == Level{50, false},
          "resource denial failed to silence sink PCM or changed user controls");
    check(receiver.set_media_resource_enabled(false) && receiver.status().resource_requested == 1,
          "duplicate resource target restarted configuration");
    check(receiver.request_audio_focus({.25, 0}), "duck during denial rejected");
    check(receiver.set_media_resource_enabled(true), "media resource return rejected");
    const auto returned = median(samples.fresh(receiver));
    check(returned > half * .2 && returned < half * .3,
          "resource return cleared retained duck or user gain");
    check(receiver.request_audio_focus({1., 0}), "restored resource unduck rejected");
    apply(receiver, {3, {50, true}});
    const auto muted = samples.fresh(receiver);
    check(std::all_of(muted.begin(), muted.end(), [](auto peak) { return peak == 0; }),
          "mute did not silence sink PCM");
    apply(receiver, {4, {0, false}});
    const auto zero = samples.fresh(receiver);
    check(std::all_of(zero.begin(), zero.end(), [](auto peak) { return peak == 0; }),
          "zero gain did not silence sink PCM");
    apply(receiver, {5, {100, false}});
    const auto resumed = median(samples.fresh(receiver));
    check(resumed > full * 0.8 && resumed < full * 1.2, "unmuting did not restore decoded PCM");
    check(retired->request({4, {0, true}}) == RequestResult::stale, "old gain command replayed");
    sender.finish();
    const auto status = receiver.status();
    check(status.authenticated >= 40 && status.decoded_blocks >= 80 && !status.rejected &&
              status.decoder && !status.decoder->discarded_pcm_frames,
          "authenticated AAC control fixture or local PCM accounting failed");
  }
  check(!retired->snapshot().available &&
            retired->request({6, {0, true}}) == RequestResult::unavailable,
        "destroyed audio worker still accepts controls");
  {
    // Real output backpressure: stop the production sink pad, keep sending
    // encrypted AAC, and verify rejected retries cannot advance the ramp.
    audio_fixture::OutputBus actual;
    Samples blocked_samples;
    headunit::CarplayMediaStream receiver(
        shared, 43, phone, "fakesink", 20, {}, {},
        [&](const auto& receipt) { blocked_samples.receipt(receipt); });
    check(receiver.request_audio_focus({.2, 2000}), "initial focus request failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    check(receiver.status().focus_configured == 1 && receiver.status().phone_duck_gain == 1 &&
              receiver.status().decoded_blocks == 0 && receiver.status().output_underruns == 0 &&
              !receiver.status().output_starved,
          "focus ACK implied PCM before a source existed");
    actual.block_sink();
    struct Unblock {
      audio_fixture::OutputBus& output;
      ~Unblock() { output.unblock_sink(); }
    } unblock{actual};
    Sender sender(fixture, receiver.data_port(), shared);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    // Horizon retries can occur before appsrc is physically full, especially
    // with a larger startup reserve. The blocked pad can hold CAPS before any
    // PCM leaves appsrc, so there is no fixed count of in-flight buffers.
    while (actual.queued_input_bytes() < 8 * sizeof(headunit::AudioMixer::Block) &&
           Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto pressure_at_full = receiver.status().output_backpressure;
    // Require fresh failed turns after saturation so the corresponding accepted
    // PCM/envelope snapshot has been published, rather than racing its update.
    while (receiver.status().output_backpressure < pressure_at_full + 5 && Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto blocked = receiver.status();
    check(blocked.output_backpressure >= pressure_at_full + 5 && blocked.output_accepted >= 8 &&
              actual.queued_input_bytes() == 8 * sizeof(headunit::AudioMixer::Block) &&
              blocked.phone_duck_gain < 1 && blocked.phone_duck_gain > .8,
          "actual output did not reach bounded backpressure during ramp");
    check(receiver.set_media_resource_enabled(false), "backpressured resource denial rejected");
    check(receiver.request_audio_focus({.2, 10}), "repeated focus target failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    check(receiver.status().phone_duck_gain == blocked.phone_duck_gain &&
              receiver.status().focus_requested == 1,
          "backpressure/repeated target advanced or restarted ramp");
    actual.unblock_sink();
    const auto resume = Clock::now() + std::chrono::seconds(1);
    while (receiver.status().phone_duck_gain >= blocked.phone_duck_gain && Clock::now() < resume)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(receiver.status().phone_duck_gain < blocked.phone_duck_gain,
          "accepted PCM did not resume ramp");
    const auto denied_retry = blocked_samples.fresh(receiver);
    check(
        std::all_of(denied_retry.begin(), denied_retry.end(), [](auto peak) { return peak == 0; }),
        "retained PCM retry bypassed resource denial after backpressure");
    sender.finish();
    // Decoder/output may still contain bounded real PCM after peer stops.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto starved = receiver.status();
    check(starved.output_starved && starved.output_underruns > 0 &&
              starved.output_accepted == starved.decoded_blocks,
          "drained media worker did not cache exhausted local PCM schedule");
    check(receiver.request_audio_focus({1., 500}), "starved release request failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto idle = receiver.status();
    check(idle.decoded_blocks == starved.decoded_blocks &&
              idle.phone_duck_gain == starved.phone_duck_gain && idle.output_starved &&
              idle.output_underruns == starved.output_underruns &&
              idle.output_accepted == starved.output_accepted,
          "source loss manufactured ramp PCM progress");
    const auto exhausted = receiver.audio_control()->snapshot().output;
    check(exhausted && exhausted->starved && exhausted->accepted_blocks == idle.output_accepted &&
              exhausted->underruns == idle.output_underruns,
          "direct output telemetry lost real starvation counters");
    actual.error("offline focus output retired");
    const auto retirement = Clock::now() + std::chrono::seconds(1);
    while ((receiver.status().fault.empty() || receiver.audio_control()->snapshot().available) &&
           Clock::now() < retirement)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(receiver.status().fault == "offline focus output retired" &&
              !receiver.request_audio_focus({.2, 500}) &&
              !receiver.set_media_resource_enabled(true),
          "retired output accepted phone focus");
    check(!receiver.audio_control()->snapshot().output,
          "failed direct output retained live telemetry");
  }
  std::cout << "encrypted AAC: user gain/mute, phone duck/release, repeated targets, real "
               "backpressure, source loss and retired-route checks passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
