// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_commands.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
std::atomic<std::size_t> allocations{};
}
void* operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (auto* result = std::malloc(size ? size : 1)) return result;
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

using headunit::Clock;
using namespace headunit::vehicle_control;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* reason) {
  if (!ok) throw std::runtime_error(reason);
}
template <class F>
void rejects(F&& operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid vehicle control operation was accepted");
}
Clock::time_point at(std::int64_t milliseconds) {
  return Clock::time_point{std::chrono::milliseconds(milliseconds)};
}
Key key(std::string_view text) {
  const auto result = parse_key(text);
  check(bool(result), "test key is invalid");
  return *result;
}
TargetPolicy boolean(std::string_view name, bool writable = true) {
  TargetPolicy result;
  result.key = key(name);
  result.writable = writable;
  return result;
}
TargetPolicy number(std::string_view name, ValueKind kind, double minimum, double maximum,
                    double step, bool writable = true) {
  auto result = boolean(name, writable);
  result.kind = kind;
  result.minimum = minimum;
  result.maximum = maximum;
  result.step = step;
  return result;
}
std::array<TargetPolicy, 4> policies() {
  return {{number("climate.driver.temperature", ValueKind::real, 16, 32, .5),
           number("climate.driver.fan", ValueKind::integer, 0, 100, 10),
           boolean("lighting.comfort"),
           number("engine.temperature", ValueKind::real, 40, 120, 1, false)}};
}
Request request(std::uint64_t id, const Key& target, Value value = 21.,
                std::int64_t requested = 1010, std::int64_t deadline = 2000,
                std::uint64_t generation = 1) {
  return {id, generation, target, value, at(requested), at(deadline)};
}
ConfirmedState actual(const Key& target, Value value = 20.25, std::int64_t confirmed = 1040,
                      Origin origin = Origin::measured, std::uint64_t generation = 1) {
  return {generation, target, value, at(confirmed), origin};
}
Acknowledgement reply(const Request& request, std::uint64_t sequence, ReplyStatus status,
                      std::int64_t reported, std::optional<ConfirmedState> confirmed = {}) {
  return {request.identifier,
          request.provider_generation,
          sequence,
          request.target,
          status,
          at(reported),
          confirmed};
}
void keys_and_policies() {
  for (const auto text : {"A", "climate.driver_temperature", "Car-1.part_2", "A._part"})
    check(parse_key(text) && key_text(*parse_key(text)) == text,
          "valid semantic key did not round trip");
  for (const auto text :
       {"", "1car", ".car", "car.", "car..seat", "car seat", "car/seat", "car:seat"})
    check(!parse_key(text), "invalid semantic key accepted");
  const std::string longest(128, 'a');
  check(parse_key(longest) && !parse_key(longest + 'a'), "key bound not enforced");
  check(!parse_key(std::string_view{"a\0b", 3}) && !parse_key("véhicule"),
        "non-ASCII or embedded NUL accepted");
  auto malformed = key("valid");
  malformed.length = 255;
  check(!valid_key(malformed) && key_text(malformed).empty(),
        "malformed length accessed outside key storage");
  malformed = key("valid");
  malformed.bytes.back() = 'x';
  check(!valid_key(malformed), "noncanonical unused key bytes accepted");
  auto p = boolean("power");
  check(valid_policy(p) && valid_value(p, true, true) && !valid_value(p, std::int64_t{1}, false),
        "boolean type policy failed");
  p.minimum = 0;
  check(!valid_policy(p), "boolean numeric policy accepted");
  p = boolean("power");
  p.maximum_age = Clock::duration::zero();
  check(!valid_policy(p), "zero age accepted");
  p.maximum_age = 24h;
  check(valid_policy(p), "24 hour age bound rejected");
  p.maximum_age = 24h + 1ns;
  check(!valid_policy(p), "excessive age accepted");
  p = number("fan", ValueKind::integer, 0, 100, 10);
  check(valid_value(p, std::int64_t{20}, true) && !valid_value(p, std::int64_t{21}, true) &&
            valid_value(p, std::int64_t{21}, false) && !valid_value(p, 20., true),
        "integer request grid replaced actual precision/type");
  p.step = .5;
  check(!valid_policy(p), "fractional integer step accepted");
  p = number("fan", ValueKind::integer, -9007199254740991., 9007199254740991., 3);
  check(valid_value(p, std::int64_t{-9007199254740991LL}, true) &&
            valid_value(p, std::int64_t{9007199254740991LL}, false) &&
            !valid_value(p, std::int64_t{9007199254740992LL}, false) &&
            !valid_value(p, std::int64_t{9007199254740990LL}, true),
        "integer magnitude/modulo guard failed");
  p.minimum = -9007199254740992.;
  check(!valid_policy(p), "integer endpoint beyond exact domain accepted");
  p = number("temperature", ValueKind::real, 16, 32, .5);
  check(valid_value(p, 20.5, true) && !valid_value(p, 20.25, true) && valid_value(p, 20.25, false),
        "real request grid rounded measured state");
  check(!valid_value(p, std::numeric_limits<double>::infinity(), false) &&
            !valid_value(p, std::numeric_limits<double>::quiet_NaN(), false),
        "nonfinite actual state accepted");
  p = number("fraction", ValueKind::real, 0, .3, .1);
  check(
      valid_policy(p) && valid_value(p, .3, true) && valid_value(p, .1 + .2, true) == false,
      "decimal endpoint bounds or representation grid failed");  // The original request still must lie within the exact bound.
  p.maximum = 1.;
  check(valid_value(p, .1 + .2, true) && !valid_value(p, .31, true),
        "ordinary decimal grid/off-grid guard failed");
  p = number("large", ValueKind::real, 1e15, 1e15 + 10, .7);
  check(valid_policy(p) && valid_value(p, 1e15, true) && !valid_value(p, 1e15 + .75, true),
        "large minimum rounded an off-grid value onto the grid");
  p = number("crossing", ValueKind::real, -9007199254740991., 9007199254740991., 3);
  check(valid_policy(p) && !valid_value(p, 9007199254740990., true) &&
            valid_value(p, 9007199254740989., true),
        "large crossing range lost exact subtraction residual");
  p = number("tiny", ValueKind::real, 0, 1, std::numeric_limits<double>::denorm_min());
  check(!valid_policy(p), "unbounded tiny-step quotient accepted");
  p = number("tiny", ValueKind::real, 1e15, 1e15 + 1, .001);
  check(!valid_policy(p), "unrepresentable step accepted");
  p = number("single", ValueKind::real, 0, 0, std::numeric_limits<double>::denorm_min());
  check(valid_policy(p) && valid_value(p, 0., true) &&
            !valid_value(p, std::numeric_limits<double>::denorm_min(), true),
        "singleton grid failed");
  p = number("bad", ValueKind::real, 0, 1, 0);
  check(!valid_policy(p), "zero step accepted");
  p.step = 1;
  p.minimum = 2;
  check(!valid_policy(p), "reversed numeric range accepted");
  p = number("bad", ValueKind::real, 0, 9007199254740992., 1);
  check(!valid_policy(p), "unbounded real endpoint accepted");
  p = number("bad", static_cast<ValueKind>(99), 0, 1, 1);
  check(!valid_policy(p), "unknown value kind accepted");
  p = number("bad", ValueKind::real, 0, 1, 1);
  p.step.reset();
  check(!valid_policy(p), "missing numeric policy field accepted");
  rejects([] { Coordinator coordinator(std::span<const TargetPolicy>{}); });
  std::array<TargetPolicy, 33> many;
  many.fill(boolean("same"));
  rejects([&] { Coordinator coordinator(many); });
  std::array<TargetPolicy, 2> duplicate{{boolean("same"), boolean("same")}};
  rejects([&] { Coordinator coordinator(duplicate); });
}
void command_lifecycle() {
  const auto configured = policies();
  Coordinator coordinator(configured);
  coordinator.begin_generation(1, at(1000), at(1000));
  const auto& temperature = configured[0].key;
  const auto denied = request(1, temperature);
  check(coordinator.submit(denied, at(1010)).status == Status::accepted &&
            !coordinator.read(temperature, at(1010)),
        "request was published as actual state");
  check(!coordinator.dispatch(1, at(1020)), "default interlock permission dispatched a write");
  const auto denied_outcome = *coordinator.snapshot(1, at(1020));
  check(denied_outcome.status == Status::rejected && denied_outcome.reason == Reason::interlock &&
            !denied_outcome.dispatched_at,
        "denied interlock outcome not retained");
  check(!coordinator.dispatch(1, at(1020), true), "terminal denied command dispatched on retry");
  const auto accepted = request(2, temperature, 21., 1020);
  check(coordinator.submit(accepted, at(1020)).status == Status::accepted,
        "new target request not accepted");
  const auto busy = request(3, temperature, 22., 1020);
  const auto busy_outcome = coordinator.submit(busy, at(1020));
  check(busy_outcome.status == Status::rejected && busy_outcome.reason == Reason::busy &&
            !coordinator.dispatch(3, at(1020), true),
        "busy target enqueued work");
  const auto read_only = request(4, configured[3].key, 80., 1020);
  const auto read_only_outcome = coordinator.submit(read_only, at(1020));
  check(read_only_outcome.status == Status::rejected &&
            read_only_outcome.reason == Reason::not_writable &&
            !coordinator.dispatch(4, at(1020), true),
        "read-only target dispatched work");
  check(coordinator.dispatch(2, at(1030), true) == accepted &&
            !coordinator.dispatch(2, at(1030), true),
        "fake executor did not dispatch exactly once");
  auto state = *coordinator.snapshot(2, at(1030));
  check(state.status == Status::pending && state.dispatched_at == at(1030),
        "dispatch state/timestamp missing");
  coordinator.observe(actual(temperature, 21., 1040), at(1040));
  check(coordinator.snapshot(2, at(1040))->status == Status::pending,
        "independent matching observation completed a pending command");
  const auto pending = reply(accepted, 1, ReplyStatus::pending, 1040);
  check(coordinator.acknowledge(pending, at(1040)).status == Status::pending,
        "pending diagnostic completed command");
  const auto applied =
      reply(accepted, 2, ReplyStatus::applied, 1050, actual(temperature, 20.25, 1050));
  state = coordinator.acknowledge(applied, at(1051));
  check(state.status == Status::applied && state.reason == Reason::none &&
            state.updated_at == at(1050) &&
            coordinator.read(temperature, at(1051)) == applied.confirmed,
        "applied actual state was rounded or replaced by requested state");
  check(coordinator.submit(accepted, at(1051)) == state &&
            coordinator.acknowledge(applied, at(1051)) == state &&
            !coordinator.dispatch(2, at(1051), true),
        "retained duplicate repeated executor work");
  auto changed = accepted;
  changed.requested = 22.;
  rejects([&] { coordinator.submit(changed, at(1500)); });
  check(coordinator.read(temperature, at(1051)) == applied.confirmed,
        "conflicting duplicate mutated time or actual state");
  auto terminal_conflict = applied;
  terminal_conflict.sequence = 3;
  terminal_conflict.reported_at = at(1060);
  rejects([&] { coordinator.acknowledge(terminal_conflict, at(1060)); });
  check(coordinator.snapshot(2, at(1051)) == state, "terminal conflict replaced retained outcome");
}
void reply_validation_is_atomic() {
  const auto configured = policies();
  Coordinator coordinator(configured);
  coordinator.begin_generation(1, at(1000), at(1000));
  const auto accepted = request(1, configured[0].key);
  coordinator.submit(accepted, at(1010));
  coordinator.dispatch(1, at(1020), true);
  coordinator.observe(actual(accepted.target, 20., 1020), at(1020));
  const auto before = *coordinator.snapshot(1, at(1020));
  const auto valid =
      reply(accepted, 1, ReplyStatus::applied, 1030, actual(accepted.target, 20.25, 1030));
  auto invalid = valid;
  const auto reject_reply = [&] {
    rejects([&] { coordinator.acknowledge(invalid, at(3000)); });
    check(coordinator.snapshot(1, at(1020)) == before &&
              coordinator.read(accepted.target, at(1020))->value == Value{20.},
          "invalid reply changed commands, expiry or host watermark");
  };
  invalid.sequence = 0;
  reject_reply();
  invalid = valid;
  invalid.identifier = 99;
  reject_reply();
  invalid = valid;
  invalid.provider_generation = 2;
  reject_reply();
  invalid = valid;
  invalid.target = configured[1].key;
  reject_reply();
  invalid = valid;
  invalid.reported_at = at(1019);
  reject_reply();
  invalid = valid;
  invalid.reported_at = at(3001);
  reject_reply();
  invalid = valid;
  invalid.status = static_cast<ReplyStatus>(99);
  reject_reply();
  invalid = valid;
  invalid.confirmed.reset();
  reject_reply();
  invalid = valid;
  invalid.confirmed->origin = Origin::unknown;
  reject_reply();
  invalid = valid;
  invalid.confirmed->origin = static_cast<Origin>(99);
  reject_reply();
  invalid = valid;
  invalid.confirmed->provider_generation = 2;
  reject_reply();
  invalid = valid;
  invalid.confirmed->target = configured[1].key;
  reject_reply();
  invalid = valid;
  invalid.confirmed->confirmed_at = at(1019);
  reject_reply();
  invalid = valid;
  invalid.confirmed->confirmed_at = at(1031);
  reject_reply();
  invalid = valid;
  invalid.confirmed->value = std::int64_t{20};
  reject_reply();
  invalid = valid;
  invalid.confirmed->value = 33.;
  reject_reply();
  invalid = valid;
  invalid.status = ReplyStatus::pending;
  reject_reply();
  invalid = valid;
  invalid.status = ReplyStatus::rejected;
  reject_reply();
  invalid = valid;
  invalid.status = ReplyStatus::failed;
  reject_reply();
  const auto pending = reply(accepted, 2, ReplyStatus::pending, 1040);
  coordinator.acknowledge(pending, at(1040));
  const auto pending_before = *coordinator.snapshot(1, at(1040));
  invalid = reply(accepted, 1, ReplyStatus::applied, 1050, actual(accepted.target, 21., 1050));
  rejects([&] { coordinator.acknowledge(invalid, at(1060)); });
  invalid.sequence = 2;
  rejects([&] { coordinator.acknowledge(invalid, at(1060)); });
  invalid.sequence = 3;
  invalid.reported_at = at(1039);
  invalid.confirmed->confirmed_at = at(1039);
  rejects([&] { coordinator.acknowledge(invalid, at(1060)); });
  check(coordinator.snapshot(1, at(1040)) == pending_before,
        "reordered reply consumed sequence or operation time");
  const auto applied =
      reply(accepted, 3, ReplyStatus::applied, 1050, actual(accepted.target, 21., 1050));
  check(coordinator.acknowledge(applied, at(1050)).status == Status::applied,
        "valid reply failed after invalid ordering attempts");
  // Sequence 1 is independently valid for another command; no provider-global sequence exists.
  const auto other = request(2, configured[1].key, std::int64_t{30}, 1050);
  coordinator.submit(other, at(1050));
  coordinator.dispatch(2, at(1050), true);
  const auto rejected = reply(other, 1, ReplyStatus::rejected, 1050);
  check(coordinator.acknowledge(rejected, at(1050)).reason == Reason::provider_rejected,
        "ack sequence incorrectly shared by provider");
  const auto failed = request(3, configured[2].key, true, 1050);
  coordinator.submit(failed, at(1050));
  coordinator.dispatch(3, at(1050), true);
  check(coordinator.acknowledge(reply(failed, 1, ReplyStatus::failed, 1050), at(1050)).reason ==
            Reason::provider_failed,
        "provider failure outcome lost");
}
void actual_state_history() {
  const auto configured = policies();
  Coordinator coordinator(configured);
  coordinator.begin_generation(1, at(1000), at(1000));
  const auto& target = configured[0].key;
  const auto submitted = request(1, target);
  coordinator.submit(submitted, at(1010));
  coordinator.dispatch(1, at(1020), true);
  const auto newer = actual(target, 22.25, 1050);
  coordinator.observe(newer, at(1060));
  const auto older_ack =
      reply(submitted, 1, ReplyStatus::applied, 1060, actual(target, 21.25, 1040));
  check(coordinator.acknowledge(older_ack, at(1060)).status == Status::applied &&
            coordinator.read(target, at(1060)) == newer,
        "older application clobbered independent newer actual state");
  coordinator.observe(newer, at(1070));
  check(coordinator.read(target, at(1070))->confirmed_at == at(1050),
        "identical observation renewed original date");
  auto conflicting = newer;
  conflicting.value = 23.;
  rejects([&] { coordinator.observe(conflicting, at(1500)); });
  conflicting = newer;
  conflicting.origin = Origin::calculated;
  rejects([&] { coordinator.observe(conflicting, at(1500)); });
  conflicting = actual(target, 22., 1049);
  rejects([&] { coordinator.observe(conflicting, at(1500)); });
  conflicting = actual(target, 22., 1501);
  rejects([&] { coordinator.observe(conflicting, at(1500)); });
  conflicting = actual(target, 22., 1100, Origin::unknown);
  rejects([&] { coordinator.observe(conflicting, at(1500)); });
  check(coordinator.read(target, at(1070)) == newer,
        "invalid actual state advanced receipt clock or published state");
  check(coordinator.read(target, at(1549)) == newer && !coordinator.read(target, at(1550)),
        "actual state did not expire at exact configured age");
  rejects([&] { coordinator.observe(newer, at(1560)); });
  check(!coordinator.read(target, at(1550)), "same-date actual state revived after expiry");
  const auto recovered = actual(target, 22.75, 1551, Origin::calculated);
  coordinator.observe(recovered, at(1551));
  check(coordinator.read(target, at(1551)) == recovered,
        "strictly newer actual state did not recover");
  const auto equal_request = request(2, target, 23., 1551, 3000);
  coordinator.submit(equal_request, at(1551));
  coordinator.dispatch(2, at(1551), true);
  auto equal_conflict =
      reply(equal_request, 1, ReplyStatus::applied, 1551, actual(target, 23., 1551));
  rejects([&] { coordinator.acknowledge(equal_conflict, at(1600)); });
  check(coordinator.snapshot(2, at(1551))->status == Status::pending &&
            coordinator.read(target, at(1551)) == recovered,
        "equal-date applied conflict partially committed");
  equal_conflict.confirmed = recovered;
  check(coordinator.acknowledge(equal_conflict, at(1551)).status == Status::applied,
        "exact already-observed actual state could not complete command");
  coordinator.disconnect(at(1552));
  check(!coordinator.read(target, at(1552)), "disconnect left actual state available");
  check(coordinator.acknowledge(equal_conflict, at(1553)).status == Status::applied &&
            !coordinator.read(target, at(1553)),
        "exact retained ack after loss restored actual state");
  coordinator.begin_generation(2, at(1554), at(1554));
  conflicting = recovered;
  conflicting.provider_generation = 2;
  rejects([&] { coordinator.observe(conflicting, at(1560)); });
  check(!coordinator.read(target, at(1554)), "old acquisition revived across provider generation");
  const auto restarted = actual(target, 21.125, 1555, Origin::measured, 2);
  coordinator.observe(restarted, at(1555));
  check(coordinator.read(target, at(1555)) == restarted,
        "new generation measured precision or recovery failed");
  // Polling an already-expired exact applied diagnostic must never refresh actual availability.
  Coordinator expired(configured);
  expired.begin_generation(1, at(1000), at(1000));
  const auto expiring = request(1, target, 21., 1010, 3000);
  expired.submit(expiring, at(1010));
  expired.dispatch(1, at(1020), true);
  const auto previous = actual(target, 21.125, 1020);
  expired.observe(previous, at(1020));
  const auto delayed = reply(expiring, 1, ReplyStatus::applied, 1020, previous);
  check(expired.acknowledge(delayed, at(1520)).status == Status::applied &&
            !expired.read(target, at(1520)),
        "matching expired confirmation was republished as fresh actual state");
}
void deadline_and_late_diagnostics() {
  const auto configured = policies();
  const auto& target = configured[0].key;
  Coordinator coordinator(configured);
  coordinator.begin_generation(1, at(1000), at(1000));
  const auto submitted = request(1, target, 21., 1010, 1100);
  coordinator.submit(submitted, at(1010));
  coordinator.dispatch(1, at(1020), true);
  const auto pending = reply(submitted, 1, ReplyStatus::pending, 1040);
  coordinator.acknowledge(pending, at(1040));
  check(coordinator.snapshot(1, at(1099))->status == Status::pending,
        "command timed out before deadline");
  auto outcome = *coordinator.snapshot(1, at(1100));
  check(outcome.status == Status::timed_out && outcome.reason == Reason::deadline &&
            outcome.updated_at == at(1100),
        "deadline boundary/source date lost");
  const auto late_pending = reply(submitted, 2, ReplyStatus::pending, 1110);
  outcome = coordinator.acknowledge(late_pending, at(1110));
  check(outcome.status == Status::timed_out && outcome.updated_at == at(1100) &&
            outcome.acknowledgement == late_pending,
        "late pending diagnostic changed timeout outcome");
  const auto late_applied =
      reply(submitted, 3, ReplyStatus::applied, 1120, actual(target, 21.25, 1115));
  outcome = coordinator.acknowledge(late_applied, at(1121));
  check(outcome.status == Status::timed_out && outcome.updated_at == at(1100) &&
            outcome.acknowledgement == late_applied && !coordinator.read(target, at(1121)),
        "late actual application converted timeout to success or state");
  check(coordinator.acknowledge(late_applied, at(1122)) == outcome,
        "exact late diagnostic duplicate changed timeout");
  auto conflicting = late_applied;
  conflicting.sequence = 4;
  conflicting.reported_at = at(1123);
  rejects([&] { coordinator.acknowledge(conflicting, at(1123)); });
  check(coordinator.snapshot(1, at(1122)) == outcome, "late terminal conflict replaced diagnostic");
  const auto accepted = request(2, target, 21., 1122, 1200);
  coordinator.submit(accepted, at(1122));
  check(!coordinator.dispatch(2, at(1200), true) &&
            coordinator.snapshot(2, at(1200))->updated_at == at(1200),
        "accepted command dispatched at deadline");
  rejects(
      [&] { coordinator.acknowledge(reply(accepted, 1, ReplyStatus::pending, 1200), at(1201)); });
  check(coordinator.snapshot(2, at(1200))->status == Status::timed_out,
        "undispatched command accepted provider diagnostic");
  const auto transit = request(3, target, 22., 1200, 1300);
  coordinator.submit(transit, at(1200));
  coordinator.dispatch(3, at(1210), true);
  const auto delayed = reply(transit, 1, ReplyStatus::applied, 1299, actual(target, 22.25, 1298));
  check(coordinator.acknowledge(delayed, at(1300)).status == Status::timed_out &&
            !coordinator.read(target, at(1300)),
        "timely source reply received at deadline was treated as success");
  // Deadline expiry takes precedence over a loss/restart received at its boundary.
  const auto lost = request(4, target, 22., 1300, 1400);
  coordinator.submit(lost, at(1300));
  coordinator.dispatch(4, at(1300), true);
  coordinator.disconnect(at(1400));
  outcome = *coordinator.snapshot(4, at(1400));
  check(outcome.status == Status::timed_out && outcome.updated_at == at(1400),
        "provider loss replaced an already-due timeout");
  coordinator.begin_generation(2, at(1401), at(1401));
  const auto restart = request(5, target, 22., 1401, 1500, 2);
  coordinator.submit(restart, at(1401));
  coordinator.begin_generation(3, at(1500), at(1500));
  check(coordinator.snapshot(5, at(1500))->status == Status::timed_out,
        "new generation replaced an already-due accepted timeout");
}
void generation_and_request_validation() {
  const auto configured = policies();
  const auto& target = configured[0].key;
  Coordinator coordinator(configured);
  rejects([&] { coordinator.submit(request(1, target), at(1010)); });
  rejects([&] { coordinator.begin_generation(0, at(1000), at(1000)); });
  rejects([&] { coordinator.begin_generation(1, at(1001), at(1000)); });
  coordinator.begin_generation(1, at(1000), at(1000));
  const auto valid = request(1, target);
  auto invalid = valid;
  const auto reject_request = [&] {
    rejects([&] { coordinator.submit(invalid, at(1100)); });
    check(!coordinator.snapshot(1, at(1000)),
          "invalid request consumed identifier or operation time");
  };
  invalid.identifier = 0;
  reject_request();
  invalid = valid;
  invalid.provider_generation = 2;
  reject_request();
  invalid = valid;
  invalid.target = key("unknown");
  reject_request();
  invalid = valid;
  invalid.requested_at = at(999);
  reject_request();
  invalid = valid;
  invalid.requested_at = at(1101);
  reject_request();
  invalid = valid;
  invalid.deadline = at(1100);
  reject_request();
  invalid = valid;
  invalid.deadline = invalid.requested_at + maximum_command_duration + 1ns;
  reject_request();
  invalid = valid;
  invalid.requested = 20.25;
  reject_request();
  invalid = valid;
  invalid.requested = true;
  reject_request();
  coordinator.submit(valid, at(1010));
  coordinator.dispatch(1, at(1020), true);
  const auto accepted = request(2, configured[1].key, std::int64_t{20}, 1020);
  coordinator.submit(accepted, at(1020));
  coordinator.observe(actual(target, 20.25, 1020), at(1020));
  const auto pending = reply(valid, 1, ReplyStatus::pending, 1020);
  coordinator.acknowledge(pending, at(1020));
  rejects([&] { coordinator.begin_generation(1, at(1030), at(1030)); });
  rejects([&] { coordinator.begin_generation(2, at(1019), at(1030)); });
  rejects([&] { coordinator.begin_generation(2, at(1031), at(1030)); });
  check(coordinator.snapshot(1, at(1020))->status == Status::pending &&
            coordinator.read(target, at(1020)),
        "invalid provider restart mutated state");
  coordinator.disconnect(at(1030));
  check(coordinator.snapshot(1, at(1030))->status == Status::failed &&
            coordinator.snapshot(1, at(1030))->reason == Reason::provider_lost &&
            coordinator.snapshot(2, at(1030))->reason == Reason::provider_lost &&
            !coordinator.read(target, at(1030)),
        "provider loss did not fail all active commands/actual state");
  check(coordinator.acknowledge(pending, at(1030)).reason == Reason::provider_lost,
        "retained exact diagnostic after loss was rejected");
  auto unmatched = reply(valid, 2, ReplyStatus::applied, 1030, actual(target, 22., 1030));
  rejects([&] { coordinator.acknowledge(unmatched, at(1040)); });
  check(coordinator.snapshot(1, at(1030))->reason == Reason::provider_lost,
        "unmatched post-loss reply changed history");
  const auto offline = request(3, target, 22., 1030);
  const auto offline_outcome = coordinator.submit(offline, at(1030));
  check(
      offline_outcome.status == Status::rejected && offline_outcome.reason == Reason::provider_lost,
      "known disconnected provider request was silently accepted");
  coordinator.begin_generation(2, at(1040), at(1040));
  check(coordinator.submit(valid, at(1040)).reason == Reason::provider_lost,
        "exact old generation request was not retained history");
  invalid = request(4, target, 22., 1040);
  rejects([&] { coordinator.submit(invalid, at(1040)); });
  const auto new_request = request(4, target, 22., 1040, 2000, 2);
  check(coordinator.submit(new_request, at(1040)).status == Status::accepted,
        "invalid old generation consumed new identifier");
  coordinator.begin_generation(3, at(1050), at(1050));
  check(coordinator.snapshot(4, at(1050))->reason == Reason::provider_lost,
        "restart did not invalidate accepted request");
  rejects([&] { coordinator.poll(at(1049)); });
  check(coordinator.snapshot(4, at(1050))->reason == Reason::provider_lost,
        "reverse time altered outcome");
}
void bounded_retention() {
  std::array<TargetPolicy, maximum_targets> configured;
  for (std::size_t i = 0; i < configured.size(); ++i)
    configured[i] = boolean("target" + std::to_string(i));
  Coordinator coordinator(configured);
  coordinator.begin_generation(1, at(1000), at(1000));
  for (std::size_t i = 0; i < configured.size(); ++i) {
    const auto submitted = request(i + 1, configured[i].key, true);
    check(coordinator.submit(submitted, at(1010)).status == Status::accepted &&
              coordinator.dispatch(i + 1, at(1010), true) == submitted,
          "bounded active request not dispatched");
  }
  for (std::uint64_t id = 33; id <= 128; ++id) {
    const auto outcome = coordinator.submit(request(id, configured[0].key, false), at(1010));
    check(outcome.reason == Reason::busy,
          "terminal retention pressure changed target authorization");
  }
  for (std::uint64_t id = 1; id <= 32; ++id)
    check(coordinator.snapshot(id, at(1010))->status == Status::pending,
          "retention evicted an active command");
  check(!coordinator.snapshot(33, at(1010)) && !coordinator.snapshot(96, at(1010)) &&
            coordinator.snapshot(97, at(1010)) && coordinator.snapshot(128, at(1010)),
        "retention did not evict oldest terminal records");
  rejects([&] { coordinator.submit(request(33, configured[0].key, false), at(1020)); });
  rejects([&] { coordinator.submit(request(96, configured[0].key, false), at(1020)); });
  const auto duplicate = request(128, configured[0].key, false);
  check(coordinator.submit(duplicate, at(1010)).reason == Reason::busy,
        "retained terminal duplicate lost");
  auto invalid = request(1000, configured[0].key, false);
  invalid.target = key("unknown");
  rejects([&] { coordinator.submit(invalid, at(1020)); });
  check(coordinator.submit(request(129, configured[0].key, false), at(1010)).reason == Reason::busy,
        "invalid high identifier advanced lifetime watermark");
  coordinator.disconnect(at(1020));
  coordinator.begin_generation(2, at(1030), at(1030));
  rejects(
      [&] { coordinator.submit(request(128, configured[0].key, false, 1030, 2000, 2), at(1030)); });
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto highest = request(maximum, configured[0].key, true, 1030, 2000, 2);
  coordinator.submit(highest, at(1030));
  check(coordinator.submit(highest, at(1030)).status == Status::accepted,
        "maximum lifetime ID duplicate failed");
  rejects([&] {
    coordinator.submit(request(maximum - 1, configured[1].key, true, 1030, 2000, 2), at(1040));
  });
  check(coordinator.snapshot(maximum, at(1030))->status == Status::accepted,
        "identifier overflow/replay changed active state");
}
void immutable_policy_and_owner_thread() {
  auto configured = policies();
  const auto original = configured[0];
  Coordinator coordinator(configured);
  configured[0].writable = false;
  configured[0].step = 2;
  configured[0].maximum_age = 1ms;
  check(coordinator.policy(original.key) == original && !coordinator.policy(key("missing")),
        "configured policies were mutable or unknown policy fabricated");
  auto malformed = original.key;
  malformed.bytes.back() = 'x';
  check(!coordinator.policy(malformed), "malformed key exposed policy");
  coordinator.begin_generation(1, at(1000), at(1000));
  bool rejected = false;
  std::thread other([&] {
    try {
      coordinator.poll(at(1100));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
  });
  other.join();
  check(rejected, "non-owner thread mutated coordinator");
  check(coordinator.submit(request(1, original.key, 20.5), at(1010)).status == Status::accepted,
        "external policy mutation or owner violation changed request policy/time");
  coordinator.observe(actual(original.key, 20.25, 1010), at(1010));
  check(bool(coordinator.read(original.key, at(1509))),
        "external age mutation altered immutable expiry");
  check(!coordinator.read(original.key, at(1510)), "immutable maximum-age boundary failed");
}
void no_allocations_after_construction() {
  const auto configured = policies();
  Coordinator coordinator(configured);
  const auto target = configured[0].key;
  const auto submitted = request(1, target);
  const auto confirmation = actual(target, 20.25, 1030);
  const auto applied = reply(submitted, 1, ReplyStatus::applied, 1030, confirmation);
  const auto before = allocations.load(std::memory_order_relaxed);
  coordinator.begin_generation(1, at(1000), at(1000));
  const auto accepted = coordinator.submit(submitted, at(1010));
  const auto dispatched = coordinator.dispatch(1, at(1020), true);
  const auto completed = coordinator.acknowledge(applied, at(1030));
  const auto observed = coordinator.read(target, at(1030));
  const auto duplicate = coordinator.submit(submitted, at(1030));
  const auto retained = coordinator.snapshot(1, at(1030));
  coordinator.poll(at(1530));
  coordinator.disconnect(at(1531));
  coordinator.begin_generation(2, at(1532), at(1532));
  const auto after = allocations.load(std::memory_order_relaxed);
  check(before == after && accepted.status == Status::accepted && dispatched == submitted &&
            completed.status == Status::applied && observed == confirmation &&
            duplicate == completed && retained == completed,
        "successful fixed-capacity operations allocated or changed expected result");
}
}  // namespace
int main() {
  try {
    keys_and_policies();
    command_lifecycle();
    reply_validation_is_atomic();
    actual_state_history();
    deadline_and_late_diagnostics();
    generation_and_request_validation();
    bounded_retention();
    immutable_policy_and_owner_thread();
    no_allocations_after_construction();
    std::cout << "vehicle command tests passed (9 suites)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
