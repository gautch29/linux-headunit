// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/touch_input.hpp>
#include <deque>
#include <mutex>
#include <span>
#include <array>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace headunit {
// One connection, one current UI generation. The receiver owns socket writes;
// producers only copy bounded reports. A rejected send retains the report.
class TouchMailbox {
 public:
  using Generation = std::uint64_t;
  struct Timing {
    std::uint64_t reports = 0, total_ns = 0, max_ns = 0;
    // Fixed-storage distribution of enqueue -> accepted HID queue delay.
    static constexpr std::array<std::uint64_t, 11> bounds_ns{
        250000,   500000,   1000000,  2000000,   4000000,  8000000,
        16000000, 32000000, 64000000, 128000000, 256000000};
    std::array<std::uint64_t, 12> buckets{};
    std::uint64_t percentile_upper_ns(unsigned percent) const {
      if (!reports || percent == 0 || percent > 100) return 0;
      const auto rank = (reports / 100) * percent + ((reports % 100) * percent + 99) / 100;
      std::uint64_t count = 0;
      for (std::size_t i = 0; i < buckets.size(); ++i) {
        count += buckets[i];
        if (count >= rank) return i < bounds_ns.size() ? std::min(bounds_ns[i], max_ns) : max_ns;
      }
      return max_ns;
    }
  };
  explicit TouchMailbox(bool diagnostics = false) : diagnostics_(diagnostics) {
    if (::pipe(wake_.data()) < 0) throw std::runtime_error("cannot create touch wakeup pipe");
    for (const int fd : wake_) {
      const int flags = ::fcntl(fd, F_GETFL), descriptor_flags = ::fcntl(fd, F_GETFD);
      if (flags < 0 || descriptor_flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
          ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        ::close(wake_[0]);
        ::close(wake_[1]);
        throw std::runtime_error("cannot configure touch wakeup pipe");
      }
    }
  }
  ~TouchMailbox() {
    ::close(wake_[0]);
    ::close(wake_[1]);
  }
  // Borrowed readiness only. The receiver still owns every phone socket write.
  int wake_fd() const noexcept { return wake_[0]; }
  Timing take_timing() {
    std::lock_guard lock(mutex_);
    const auto result = timing_;
    timing_ = {};
    return result;
  }
  Generation begin() {
    std::lock_guard lock(mutex_);
    reset();
    suppressed_ = false;
    return ++generation_;
  }
  void cancel(Generation generation) {
    std::lock_guard lock(mutex_);
    if (generation == generation_) {
      reset();
      suppressed_ = false;
    }
  }
  // Stop a retired worker immediately, including late callbacks/destruction.
  void retire() {
    std::lock_guard lock(mutex_);
    reset();
    ++generation_;
    suppressed_ = false;
  }
  void push(Generation generation, std::span<const TouchInput> reports) {
    std::lock_guard lock(mutex_);
    if (generation != generation_) return;
    const bool empty = pending_.empty();
    for (const auto& report : reports) {
      if (suppressed_) {
        if (!report.pressed && !report.second_pressed) suppressed_ = false;
        continue;
      }
      if (pending_.size() == 64) {
        reset();
        suppressed_ = report.pressed || report.second_pressed;
        continue;
      }
      pending_.push_back({report, diagnostics_ ? Clock::now() : Clock::time_point{}});
    }
    if (empty && !pending_.empty()) notify();
  }
  // Bounded nonblocking sender callback, on receiver thread only. Never call
  // another mailbox method from the callback; it runs under the short lock.
  template <class Send>
  unsigned drain(Send send) {
    std::lock_guard lock(mutex_);
    clear_wakeup();
    unsigned sent = 0;
    while (!pending_.empty() && sent < 64) {
      if (!send(pending_.front().report)) break;
      if (diagnostics_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 Clock::now() - pending_.front().enqueued)
                                 .count();
        const auto age = std::uint64_t(std::max<std::int64_t>(elapsed, 0));
        ++timing_.reports;
        timing_.total_ns += age;
        timing_.max_ns = std::max(timing_.max_ns, age);
        const auto bucket =
            std::lower_bound(Timing::bounds_ns.begin(), Timing::bounds_ns.end(), age) -
            Timing::bounds_ns.begin();
        ++timing_.buckets[std::size_t(bucket)];
      }
      delivered_ = pending_.front().report;
      pending_.pop_front();
      ++sent;
    }
    return sent;
  }

 private:
  using Clock = std::chrono::steady_clock;
  struct Pending {
    TouchInput report;
    Clock::time_point enqueued;
  };
  void notify() {
    const char byte = 1;
    ssize_t n;
    do {
      n = ::write(wake_[1], &byte, 1);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      throw std::runtime_error("touch wakeup write failed");
  }
  void clear_wakeup() {
    std::array<char, 128> bytes;
    for (;;) {
      const auto n = ::read(wake_[0], bytes.data(), bytes.size());
      if (n > 0) continue;
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
      throw std::runtime_error("touch wakeup read failed");
    }
  }
  void reset() {
    pending_.clear();
    if (delivered_.pressed || delivered_.second_pressed) {
      auto release = delivered_;
      release.pressed = false;
      release.second_pressed = false;
      pending_.push_back({release, diagnostics_ ? Clock::now() : Clock::time_point{}});
      // A cancellation replaces even a backpressured queue. Its prior wake
      // may already be consumed, so the replacement release needs a new one.
      notify();
    }
  }
  std::mutex mutex_;
  Generation generation_ = 0;
  TouchInput delivered_{};
  bool suppressed_ = false;
  std::deque<Pending> pending_;
  std::array<int, 2> wake_{-1, -1};
  bool diagnostics_ = false;
  Timing timing_;
};
}  // namespace headunit
