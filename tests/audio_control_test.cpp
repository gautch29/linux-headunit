// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_control.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
std::atomic<std::size_t> allocations{};
}
void* operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
void operator delete(void* value) noexcept {
  std::free(value);
}
void operator delete[](void* value) noexcept {
  std::free(value);
}
void operator delete(void* value, std::size_t) noexcept {
  std::free(value);
}
void operator delete[](void* value, std::size_t) noexcept {
  std::free(value);
}

using namespace headunit::audio_control;
namespace {
void check(bool valid, const char* reason) {
  if (!valid) throw std::runtime_error(reason);
}
template <class F>
void rejects(F&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid audio mailbox operation was accepted");
}
Command command(std::uint64_t sequence, unsigned volume, bool muted = false) {
  return {sequence, {volume, muted}};
}
void initial_availability() {
  Mailbox defaults;
  check(defaults.snapshot() == Snapshot{}, "initial default volume/history changed");
  Mailbox mailbox({42, true});
  const Snapshot initial{false, {0, {42, true}}, {0, {42, true}}};
  check(mailbox.snapshot() == initial &&
            mailbox.request(command(1, 50)) == RequestResult::unavailable &&
            mailbox.snapshot() == initial,
        "unopened mailbox published or retained an unavailable request");
  rejects([&] { mailbox.next(); });
  rejects([&] { mailbox.close(); });
  rejects([&] { mailbox.applied(command(1, 50)); });
  mailbox.open();
  auto opened = initial;
  opened.available = true;
  check(mailbox.snapshot() == opened && !mailbox.next(),
        "opening fabricated a mixer command or changed initial applied level");
  rejects([&] { mailbox.open(); });
  check(mailbox.snapshot() == opened, "repeated open changed actual mixer history");
}
void output_progress_is_owner_dated_and_retired() {
  Mailbox mailbox;
  rejects([&] { mailbox.observe_output({1, 0, 0, 0, false}); });
  mailbox.open();
  const OutputProgress first{100, 5, 1, 0, false};
  mailbox.observe_output(first);
  check(mailbox.snapshot().output == first, "output observation was not copied");
  mailbox.observe_output(first);
  const auto before = mailbox.snapshot();
  for (const auto& bad : std::array<OutputProgress, 5>{
           OutputProgress{0, 5, 1, 0, false}, OutputProgress{99, 5, 1, 0, false},
           OutputProgress{101, 4, 1, 0, false}, OutputProgress{101, 5, 0, 0, false},
           OutputProgress{100, 5, 1, 0, true}}) {
    rejects([&] { mailbox.observe_output(bad); });
    check(mailbox.snapshot() == before, "invalid output observation changed history");
  }
  bool other_rejected = false;
  std::thread other([&] {
    try {
      mailbox.observe_output({101, 6, 1, 0, false});
    } catch (const std::invalid_argument&) {
      other_rejected = true;
    }
  });
  other.join();
  check(other_rejected && mailbox.snapshot() == before, "another owner refreshed output");
  mailbox.observe_output({102, 5, 1, 1, true});
  rejects([&] { mailbox.observe_output({103, 6, 2, 0, false}); });
  OutputProgress timed{104, 6, 2, 1, false, OutputTiming{70000000, 50000000, 0, 0}};
  mailbox.observe_output(timed);
  check(mailbox.snapshot().output == timed,
        "timing observation was not copied with its owner date");
  auto bad_timing = timed;
  bad_timing.timing->scheduled_ahead_ns = 0;
  rejects([&] { mailbox.observe_output(bad_timing); });
  bad_timing.sampled_ns = 105;
  bad_timing.timing->maximum_owner_gap_ns = 49999999;
  rejects([&] { mailbox.observe_output(bad_timing); });
  bad_timing.timing.reset();
  rejects([&] { mailbox.observe_output(bad_timing); });
  check(mailbox.snapshot().output == timed, "invalid timing changed the retained observation");
  mailbox.close();
  check(!mailbox.snapshot().output, "retired route retained live output telemetry");
  rejects([&] { mailbox.observe_output({104, 6, 2, 1, false}); });
}
void requested_and_applied_are_separate() {
  Mailbox mailbox({35, false});
  mailbox.open();
  const auto requested = command(1, 70, true);
  check(mailbox.request(requested) == RequestResult::accepted,
        "valid volume/mute request rejected");
  auto snapshot = mailbox.snapshot();
  check(snapshot.available && snapshot.requested == requested && snapshot.applied == command(0, 35),
        "requested volume appeared as applied mixer state");
  check(mailbox.next() == requested && !mailbox.next(),
        "audio lease repeated work before confirmation");
  check(mailbox.snapshot() == snapshot, "leasing published requested volume as actual");
  mailbox.applied(requested);
  snapshot = mailbox.snapshot();
  check(snapshot.applied == requested && snapshot.requested == requested && !mailbox.next(),
        "exact mixer confirmation did not retire pending work");
  rejects([&] { mailbox.applied(requested); });
  check(mailbox.snapshot() == snapshot, "duplicate mixer confirmation changed actual history");
  // An explicit same-level command is a distinct sequence, not an inferred
  // already-applied confirmation; it still requires an owner acknowledgement.
  const auto same_level = command(2, 70, true);
  mailbox.request(same_level);
  check(mailbox.snapshot().applied == requested && mailbox.next() == same_level,
        "same-level newer request was implicitly confirmed");
  mailbox.applied(same_level);
  check(mailbox.snapshot().applied == same_level, "same-level explicit confirmation lost sequence");
}
void latest_request_and_inflight_race() {
  Mailbox mailbox;
  mailbox.open();
  const auto first = command(1, 20), second = command(2, 60, true), third = command(3, 80);
  mailbox.request(first);
  check(mailbox.next() == first, "first lease missing");
  RequestResult accepted = RequestResult::unavailable;
  Snapshot during;
  std::thread caller([&] {
    accepted = mailbox.request(second);
    during = mailbox.snapshot();
  });
  caller.join();
  check(accepted == RequestResult::accepted && during.requested == second &&
            during.applied.sequence == 0 && !mailbox.next(),
        "concurrent request replaced in-flight lease or actual mixer state");
  rejects([&] { mailbox.applied(second); });
  check(mailbox.snapshot() == during && !mailbox.next(),
        "wrong leased acknowledgement changed lease/history");
  mailbox.applied(first);
  check(mailbox.snapshot().requested == second && mailbox.snapshot().applied == first,
        "older lease confirmation consumed newer request");
  mailbox.request(third);
  check(mailbox.next() == third, "latest unleased command was not coalesced");
  mailbox.applied(third);
  check(mailbox.snapshot().applied == third && !mailbox.next(),
        "latest request failed to apply after concurrent update");
}
void replay_and_sequence_bounds() {
  Mailbox mailbox;
  mailbox.open();
  const auto first = command(10, 0, true);
  mailbox.request(first);
  const auto before = mailbox.snapshot();
  check(mailbox.request(first) == RequestResult::stale &&
            mailbox.request(command(10, 100)) == RequestResult::stale &&
            mailbox.request(command(9, 50)) == RequestResult::stale && mailbox.snapshot() == before,
        "stale/exact/conflicting replay changed requested state");
  check(mailbox.next() == first, "stale request invalidated pending work");
  mailbox.applied(first);
  const auto highest = command(std::numeric_limits<std::uint64_t>::max(), 100);
  mailbox.request(highest);
  check(mailbox.request(highest) == RequestResult::stale &&
            mailbox.request(command(std::numeric_limits<std::uint64_t>::max() - 1, 50)) ==
                RequestResult::stale,
        "sequence boundary wrapped or revived replay");
  rejects([&] { mailbox.request(command(0, 80)); });
  check(mailbox.next() == highest, "invalid sequence consumed highest pending request");
  mailbox.applied(highest);
}
void invalid_inputs_are_atomic() {
  rejects([] { Mailbox invalid({101, false}); });
  rejects([] { Mailbox invalid({std::numeric_limits<unsigned>::max(), true}); });
  Mailbox mailbox;
  const auto unopened = mailbox.snapshot();
  rejects([&] { mailbox.request(command(0, 0)); });
  rejects([&] { mailbox.request(command(1, 101)); });
  check(mailbox.snapshot() == unopened, "invalid unopened request latched command history");
  mailbox.open();
  const auto valid = command(1, 25, true);
  mailbox.request(valid);
  mailbox.next();
  const auto before = mailbox.snapshot();
  for (const auto invalid :
       std::array<Command, 5>{command(0, 25, true), command(2, 101),
                              command(std::numeric_limits<std::uint64_t>::max(), 101),
                              command(1, 26, true), command(1, 25, false)}) {
    rejects([&] { mailbox.applied(invalid); });
    check(mailbox.snapshot() == before && !mailbox.next(),
          "invalid applied input changed state or lease");
  }
  rejects([&] { mailbox.request(command(2, 101)); });
  rejects([&] { mailbox.request(command(0, 50)); });
  check(mailbox.snapshot() == before, "invalid request changed sequence, volume or mute");
  mailbox.applied(valid);
  check(mailbox.request(command(2, 26)) == RequestResult::accepted &&
            mailbox.next() == command(2, 26),
        "invalid newer request consumed usable sequence");
}
void terminal_close_and_owner_rules() {
  Mailbox mailbox;
  mailbox.open();
  const auto outstanding = command(1, 50, true);
  mailbox.request(outstanding);
  mailbox.next();
  bool next_rejected = false, applied_rejected = false, close_rejected = false,
       open_rejected = false;
  std::thread outsider([&] {
    try {
      mailbox.next();
    } catch (const std::invalid_argument&) {
      next_rejected = true;
    }
    try {
      mailbox.applied(outstanding);
    } catch (const std::invalid_argument&) {
      applied_rejected = true;
    }
    try {
      mailbox.close();
    } catch (const std::invalid_argument&) {
      close_rejected = true;
    }
    try {
      mailbox.open();
    } catch (const std::invalid_argument&) {
      open_rejected = true;
    }
  });
  outsider.join();
  check(next_rejected && applied_rejected && close_rejected && open_rejected &&
            mailbox.snapshot().available && !mailbox.next(),
        "non-owner operation changed open mixer state or lease");
  const auto before = mailbox.snapshot();
  mailbox.close();
  auto closed = before;
  closed.available = false;
  check(mailbox.snapshot() == closed && !mailbox.next(),
        "close published pending level or kept lease available");
  check(mailbox.request(outstanding) == RequestResult::unavailable &&
            mailbox.request(command(2, 75)) == RequestResult::unavailable,
        "closed request revived availability or returned stale instead of unavailable");
  rejects([&] { mailbox.open(); });
  rejects([&] { mailbox.applied(outstanding); });
  rejects([&] { mailbox.request(command(2, 101)); });
  mailbox.close();
  check(mailbox.snapshot() == closed, "repeated owner close changed history");
}
void concurrent_request_and_snapshot_stress() {
  Mailbox mailbox;
  mailbox.open();
  std::atomic<std::uint64_t> sequence{1};
  std::atomic<unsigned> finished{};
  std::atomic<bool> consistent{true};
  std::array<std::thread, 4> callers;
  for (auto& caller : callers)
    caller = std::thread([&] {
      try {
        for (unsigned count = 0; count < 1000; ++count) {
          const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
          const auto result = mailbox.request(command(id, unsigned(id % 101), (id % 2) != 0));
          if (result == RequestResult::unavailable) consistent = false;
          const auto snapshot = mailbox.snapshot();
          if (!snapshot.available || snapshot.applied.sequence > snapshot.requested.sequence ||
              snapshot.applied.level.volume_percent > 100 ||
              snapshot.requested.level.volume_percent > 100)
            consistent = false;
        }
      } catch (...) {
        consistent = false;
      }
      finished.fetch_add(1, std::memory_order_release);
    });
  while (finished.load(std::memory_order_acquire) != callers.size()) {
    if (const auto pending = mailbox.next())
      mailbox.applied(*pending);
    else
      std::this_thread::yield();
  }
  for (auto& caller : callers)
    caller.join();
  while (const auto pending = mailbox.next())
    mailbox.applied(*pending);
  const auto snapshot = mailbox.snapshot();
  check(consistent && snapshot.requested.sequence == 4000 &&
            snapshot.applied == snapshot.requested && !mailbox.next(),
        "concurrent latest requests/snapshots corrupted sequence, lease or actual level");
}
void owner_can_differ_from_constructor_and_no_allocations() {
  static_assert(!std::is_copy_constructible_v<Mailbox>);
  static_assert(sizeof(Mailbox) <= 256);
  Mailbox elsewhere({55, false});
  bool correct = false;
  std::thread owner([&] {
    elsewhere.open();
    const auto submitted = command(1, 75, true);
    correct =
        elsewhere.request(submitted) == RequestResult::accepted && elsewhere.next() == submitted;
    elsewhere.applied(submitted);
    elsewhere.close();
  });
  owner.join();
  check(correct && !elsewhere.snapshot().available &&
            elsewhere.snapshot().applied == command(1, 75, true),
        "constructor thread was incorrectly imposed as mixer owner");
  rejects([&] { elsewhere.next(); });
  rejects([&] { elsewhere.close(); });
  Mailbox mailbox;
  const auto first = command(1, 50), second = command(2, 30, true);
  const auto before = allocations.load(std::memory_order_relaxed);
  bool fixed = true;
  fixed &= mailbox.request(first) == RequestResult::unavailable;
  mailbox.open();
  fixed &= mailbox.request(first) == RequestResult::accepted;
  fixed &= mailbox.next() == first;
  fixed &= mailbox.request(second) == RequestResult::accepted;
  fixed &= !mailbox.next();
  mailbox.applied(first);
  fixed &= mailbox.snapshot().applied == first;
  fixed &= mailbox.next() == second;
  mailbox.applied(second);
  mailbox.observe_output({1, 0, 0, 0, false});
  mailbox.observe_output({2, 1, 1, 0, false});
  fixed &= mailbox.request(first) == RequestResult::stale;
  mailbox.close();
  mailbox.close();
  fixed &= mailbox.request(second) == RequestResult::unavailable;
  fixed &= !mailbox.next();
  fixed &= mailbox.snapshot().applied == second;
  const auto after = allocations.load(std::memory_order_relaxed);
  check(fixed && before == after,
        "valid bounded mailbox operation allocated or changed expected state");
}
}  // namespace
int main() {
  try {
    initial_availability();
    output_progress_is_owner_dated_and_retired();
    requested_and_applied_are_separate();
    latest_request_and_inflight_race();
    replay_and_sequence_bounds();
    invalid_inputs_are_atomic();
    terminal_close_and_owner_rules();
    concurrent_request_and_snapshot_stress();
    owner_can_differ_from_constructor_and_no_allocations();
    std::cout << "audio control tests passed (9 suites)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
