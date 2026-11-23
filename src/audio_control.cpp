// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_control.hpp>

#include <stdexcept>
#include <type_traits>
#include <ctime>
#include <limits>

namespace headunit::audio_control {
namespace {
void check(bool valid, const char* reason) {
  if (!valid) throw std::invalid_argument(reason);
}
void check_level(Level level) {
  check(level.volume_percent <= 100, "audio volume percent must be 0..100");
}
void check_command(const Command& command) {
  check(command.sequence > 0, "audio command sequence must be positive");
  check_level(command.level);
}
}  // namespace
std::uint64_t monotonic_time_ns() {
  timespec value{};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 ||
      value.tv_nsec >= 1000000000L ||
      std::uint64_t(value.tv_sec) >
          (std::numeric_limits<std::uint64_t>::max() - std::uint64_t(value.tv_nsec)) /
              1000000000ULL)
    throw std::runtime_error("audio monotonic clock unavailable");
  const auto result = std::uint64_t(value.tv_sec) * 1000000000ULL + std::uint64_t(value.tv_nsec);
  if (!result) throw std::runtime_error("audio monotonic clock has no origin");
  return result;
}
Mailbox::Mailbox(Level initial) {
  static_assert(std::is_trivially_copyable_v<Snapshot>);
  static_assert(std::is_nothrow_copy_assignable_v<Command>);
  check_level(initial);
  state_.requested.level = state_.applied.level = initial;
}
RequestResult Mailbox::request(Command command) {
  check_command(command);
  std::lock_guard lock(mutex_);
  if (!state_.available) return RequestResult::unavailable;
  if (command.sequence <= state_.requested.sequence) return RequestResult::stale;
  state_.requested = command;
  return RequestResult::accepted;
}
Snapshot Mailbox::snapshot() const {
  std::lock_guard lock(mutex_);
  return state_;
}
void Mailbox::check_owner() const {
  check(owner_ && *owner_ == std::this_thread::get_id(),
        "audio mailbox operation requires its opened mixer owner");
}
void Mailbox::open() {
  std::lock_guard lock(mutex_);
  check(!owner_ && !closed_, "audio mailbox cannot be opened twice or reopened");
  owner_ = std::this_thread::get_id();
  state_.available = true;
}
std::optional<Command> Mailbox::next() {
  std::lock_guard lock(mutex_);
  check_owner();
  if (!state_.available || lease_ || state_.requested.sequence <= state_.applied.sequence)
    return std::nullopt;
  lease_ = state_.requested;
  return lease_;
}
void Mailbox::applied(const Command& command) {
  check_command(command);
  std::lock_guard lock(mutex_);
  check_owner();
  check(state_.available && lease_ && *lease_ == command,
        "audio applied command does not match its active mixer lease");
  state_.applied = command;
  lease_.reset();
}
void Mailbox::close() {
  std::lock_guard lock(mutex_);
  check_owner();
  state_.available = false;
  state_.output.reset();
  lease_.reset();
  closed_ = true;
}
void Mailbox::observe_output(OutputProgress next) {
  std::lock_guard lock(mutex_);
  check_owner();
  check(state_.available && next.sampled_ns > 0,
        "output observation needs an available owner and positive acquisition date");
  if (const auto& previous = state_.output) {
    check(next.sampled_ns >= previous->sampled_ns &&
              next.accepted_blocks >= previous->accepted_blocks &&
              next.discontinuities >= previous->discontinuities &&
              next.underruns >= previous->underruns &&
              (next.sampled_ns != previous->sampled_ns || next == *previous),
          "output observation regressed or reused its acquisition date");
  }
  if (const auto& previous = state_.output; previous && previous->timing) {
    check(
        next.timing && next.timing->maximum_owner_gap_ns >= previous->timing->maximum_owner_gap_ns,
        "output timing regressed or disappeared within its owner generation");
  }
  state_.output = next;
}
}  // namespace headunit::audio_control
