// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_commands.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace headunit::vehicle_control {
namespace {
constexpr double exact_integer_limit = 9007199254740991.;
void check(bool valid, const char* reason) {
  if (!valid) throw std::invalid_argument(reason);
}
bool letter(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}
bool character(char value) {
  return letter(value) || (value >= '0' && value <= '9') || value == '.' || value == '_' ||
         value == '-';
}
bool terminal(Status status) {
  return status == Status::applied || status == Status::rejected || status == Status::failed ||
         status == Status::timed_out;
}
bool active(Status status) {
  return status == Status::accepted || status == Status::pending;
}
bool valid_origin(Origin origin) {
  return origin == Origin::measured || origin == Origin::calculated || origin == Origin::simulation;
}
bool terminal_reply(ReplyStatus status) {
  return status == ReplyStatus::applied || status == ReplyStatus::rejected ||
         status == ReplyStatus::failed;
}
// Error-free subtraction of two binary64 values. Keeping its rounding error
// avoids falsely aligning a request when the range crosses a large minimum.
double subtraction_error(double left, double right, double difference) {
  const auto right_virtual = left - difference;
  const auto left_virtual = difference + right_virtual;
  return (left - left_virtual) + (right_virtual - right);
}
std::size_t command_index(const std::array<std::optional<Snapshot>, maximum_commands>& commands,
                          std::uint64_t identifier) {
  for (std::size_t i = 0; i < commands.size(); ++i)
    if (commands[i] && commands[i]->request.identifier == identifier) return i;
  return commands.size();
}
std::size_t free_command(const std::array<std::optional<Snapshot>, maximum_commands>& commands) {
  std::size_t oldest = commands.size();
  for (std::size_t i = 0; i < commands.size(); ++i) {
    if (!commands[i]) return i;
    if (terminal(commands[i]->status) &&
        (oldest == commands.size() ||
         commands[i]->request.identifier < commands[oldest]->request.identifier))
      oldest = i;
  }
  return oldest;
}
bool publish_confirmation(std::optional<ConfirmedState>& previous, bool& available,
                          const ConfirmedState& incoming, Clock::time_point now,
                          Clock::duration maximum_age, bool command) {
  if (previous) {
    if (incoming.confirmed_at < previous->confirmed_at) return command;
    if (incoming.confirmed_at == previous->confirmed_at) {
      if (incoming != *previous) return false;
      // Applied acknowledgements may confirm an already recorded observation,
      // but never renew availability after that observation expired or was lost.
      return command || available;
    }
  }
  previous = incoming;
  available = now - incoming.confirmed_at < maximum_age;
  return true;
}
}  // namespace
bool valid_key(const Key& key) noexcept {
  if (key.length == 0 || key.length > key.bytes.size() || !letter(key.bytes[0])) return false;
  for (std::size_t i = 0; i < key.length; ++i) {
    if (!character(key.bytes[i])) return false;
    if (key.bytes[i] == '.' && (i + 1 == key.length || (i && key.bytes[i - 1] == '.')))
      return false;
  }
  for (std::size_t i = key.length; i < key.bytes.size(); ++i)
    if (key.bytes[i] != 0) return false;
  return true;
}
std::optional<Key> parse_key(std::string_view text) noexcept {
  if (text.empty() || text.size() > 128) return std::nullopt;
  Key result;
  result.length = static_cast<std::uint8_t>(text.size());
  std::copy(text.begin(), text.end(), result.bytes.begin());
  if (!valid_key(result)) return std::nullopt;
  return result;
}
std::string_view key_text(const Key& key) noexcept {
  if (!valid_key(key)) return {};
  return {key.bytes.data(), key.length};
}
bool valid_ambient_light_value(const AmbientLightValue& value) noexcept {
  return value.brightness_permille <= 1000 && value.transition_ms <= maximum_ambient_transition_ms;
}
std::array<std::uint8_t, 9> encode_ambient_light_value(const AmbientLightValue& value) {
  check(valid_ambient_light_value(value), "invalid ambient light payload");
  return {1,
          value.rgbw[0],
          value.rgbw[1],
          value.rgbw[2],
          value.rgbw[3],
          static_cast<std::uint8_t>(value.brightness_permille >> 8),
          static_cast<std::uint8_t>(value.brightness_permille & 255),
          static_cast<std::uint8_t>(value.transition_ms >> 8),
          static_cast<std::uint8_t>(value.transition_ms & 255)};
}
std::optional<AmbientLightValue> decode_ambient_light_value(
    std::span<const std::uint8_t> bytes) noexcept {
  if (bytes.size() != 9 || bytes[0] != 1) return {};
  AmbientLightValue value{{bytes[1], bytes[2], bytes[3], bytes[4]},
                          static_cast<std::uint16_t>((unsigned(bytes[5]) << 8) | bytes[6]),
                          static_cast<std::uint16_t>((unsigned(bytes[7]) << 8) | bytes[8])};
  return valid_ambient_light_value(value) ? std::optional(value) : std::nullopt;
}
bool valid_policy(const TargetPolicy& policy) noexcept {
  if (!valid_key(policy.key) || policy.maximum_age <= Clock::duration::zero() ||
      policy.maximum_age > std::chrono::hours(24))
    return false;
  if (policy.kind == ValueKind::boolean || policy.kind == ValueKind::ambient_light)
    return !policy.minimum && !policy.maximum && !policy.step;
  if (policy.kind != ValueKind::integer && policy.kind != ValueKind::real) return false;
  if (!policy.minimum || !policy.maximum || !policy.step || !std::isfinite(*policy.minimum) ||
      !std::isfinite(*policy.maximum) || !std::isfinite(*policy.step) ||
      std::abs(*policy.minimum) > exact_integer_limit ||
      std::abs(*policy.maximum) > exact_integer_limit || *policy.minimum > *policy.maximum ||
      *policy.step <= 0 || *policy.step > exact_integer_limit)
    return false;
  if (policy.kind == ValueKind::integer &&
      (std::trunc(*policy.minimum) != *policy.minimum ||
       std::trunc(*policy.maximum) != *policy.maximum || std::trunc(*policy.step) != *policy.step))
    return false;
  if (policy.kind == ValueKind::real && *policy.minimum < *policy.maximum) {
    const auto range = *policy.maximum - *policy.minimum;
    const auto grid_count = range / *policy.step;
    if (!std::isfinite(grid_count) || grid_count > exact_integer_limit ||
        *policy.minimum + *policy.step <= *policy.minimum)
      return false;
  }
  return true;
}
bool valid_value(const TargetPolicy& policy, const Value& value, bool request) noexcept {
  if (!valid_policy(policy)) return false;
  if (policy.kind == ValueKind::boolean) return std::holds_alternative<bool>(value);
  if (policy.kind == ValueKind::ambient_light) {
    const auto* light = std::get_if<AmbientLightValue>(&value);
    return light && valid_ambient_light_value(*light) && (request || light->transition_ms == 0);
  }
  if (policy.kind == ValueKind::integer) {
    const auto* integer = std::get_if<std::int64_t>(&value);
    if (!integer || *integer < -static_cast<std::int64_t>(exact_integer_limit) ||
        *integer > static_cast<std::int64_t>(exact_integer_limit) ||
        *integer < static_cast<std::int64_t>(*policy.minimum) ||
        *integer > static_cast<std::int64_t>(*policy.maximum))
      return false;
    return !request || (*integer - static_cast<std::int64_t>(*policy.minimum)) %
                               static_cast<std::int64_t>(*policy.step) ==
                           0;
  }
  const auto* real = std::get_if<double>(&value);
  if (!real || !std::isfinite(*real) || *real < *policy.minimum || *real > *policy.maximum)
    return false;
  if (!request) return true;
  const double difference = *real - *policy.minimum;
  const double difference_error = subtraction_error(*real, *policy.minimum, difference);
  const double grid = difference / *policy.step;
  if (!std::isfinite(grid) || grid < 0 || grid > exact_integer_limit) return false;
  // Allow only representation error, capped at one millionth of a step. Do
  // not normalize the original request or a confirmed actual reading. A fused
  // residual preserves precision even when long double is only binary64.
  const double tolerance =
      std::min(*policy.step * 1e-6, 8 * std::numeric_limits<double>::epsilon() *
                                        std::max({1., std::abs(*real), std::abs(*policy.minimum)}));
  const auto index = std::round(grid);
  for (const auto offset : {-1., 0., 1.}) {
    const auto adjacent = index + offset;
    if (adjacent < 0 || adjacent > exact_integer_limit) continue;
    const auto residual = std::fma(-adjacent, *policy.step, difference) + difference_error;
    if (std::abs(residual) <= tolerance) return true;
  }
  return false;
}
Coordinator::Coordinator(std::span<const TargetPolicy> policies)
    : owner_(std::this_thread::get_id()) {
  static_assert(std::is_nothrow_copy_assignable_v<State>);
  static_assert(std::is_trivially_copyable_v<ConfirmedState>);
  check(!policies.empty() && policies.size() <= maximum_targets,
        "vehicle command policies must contain 1..32 targets");
  for (std::size_t i = 0; i < policies.size(); ++i) {
    check(valid_policy(policies[i]), "invalid vehicle command target policy");
    for (std::size_t previous = 0; previous < i; ++previous)
      check(policies[i].key != policies[previous].key, "duplicate vehicle command target policy");
    policies_[i] = policies[i];
  }
  target_count_ = policies.size();
}
void Coordinator::check_time(Clock::time_point now) const {
  check(owner_ == std::this_thread::get_id(),
        "vehicle command coordinator used by another owner thread");
  check(now > Clock::time_point{} && (!state_.last_now || now >= *state_.last_now),
        "vehicle command operation time reversed or is not positive");
}
std::size_t Coordinator::target_index(const Key& key) const {
  check(valid_key(key), "invalid semantic vehicle command target");
  for (std::size_t i = 0; i < target_count_; ++i)
    if (policies_[i].key == key) return i;
  throw std::invalid_argument("unknown semantic vehicle command target");
}
void Coordinator::expire(State& state, Clock::time_point now,
                         const std::array<TargetPolicy, maximum_targets>& policies,
                         std::size_t count) {
  for (auto& command : state.commands)
    if (command && active(command->status) && now >= command->request.deadline) {
      command->status = Status::timed_out;
      command->reason = Reason::deadline;
      command->updated_at = command->request.deadline;
    }
  for (std::size_t i = 0; i < count; ++i)
    if (state.confirmed[i] && now - state.confirmed[i]->confirmed_at >= policies[i].maximum_age)
      state.available[i] = false;
}
void Coordinator::begin_generation(std::uint64_t generation, Clock::time_point started,
                                   Clock::time_point now) {
  check_time(now);
  check(generation && generation > state_.generation,
        "vehicle command provider generation did not advance");
  check(started > Clock::time_point{} && started <= now &&
            (!state_.last_now || started >= *state_.last_now),
        "vehicle command provider start is future, nonpositive or older than the operation "
        "watermark");
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  for (auto& command : candidate.commands)
    if (command && active(command->status)) {
      command->status = Status::failed;
      command->reason = Reason::provider_lost;
      command->updated_at = now;
    }
  candidate.available.fill(false);
  candidate.generation = generation;
  candidate.started_at = started;
  candidate.connected = true;
  candidate.last_now = now;
  state_ = candidate;
}
void Coordinator::disconnect(Clock::time_point now) {
  check_time(now);
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  for (auto& command : candidate.commands)
    if (command && active(command->status)) {
      command->status = Status::failed;
      command->reason = Reason::provider_lost;
      command->updated_at = now;
    }
  candidate.available.fill(false);
  candidate.connected = false;
  candidate.last_now = now;
  state_ = candidate;
}
Snapshot Coordinator::submit(const Request& request, Clock::time_point now) {
  check_time(now);
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  check(request.identifier > 0, "vehicle command identifier must be positive");
  const auto existing = command_index(candidate.commands, request.identifier);
  if (existing != candidate.commands.size()) {
    check(candidate.commands[existing]->request == request,
          "conflicting duplicate vehicle command request");
    const auto outcome = *candidate.commands[existing];
    candidate.last_now = now;
    state_ = candidate;
    return outcome;
  }
  check(request.identifier > candidate.identifier_watermark,
        "evicted or reordered vehicle command request identifier");
  const auto target = target_index(request.target);
  check(candidate.generation && request.provider_generation == candidate.generation,
        "vehicle command request provider generation mismatch");
  check(request.requested_at >= candidate.started_at && request.requested_at <= now &&
            request.deadline > now &&
            request.deadline - request.requested_at <= maximum_command_duration,
        "invalid original vehicle command request/deadline times");
  check(valid_value(policies_[target], request.requested, true),
        "invalid vehicle command requested value or grid");
  const auto slot = free_command(candidate.commands);
  check(slot < candidate.commands.size(),
        "vehicle command retention contains no evictable terminal record");
  Snapshot result;
  result.request = request;
  result.updated_at = now;
  if (!candidate.connected) {
    result.status = Status::rejected;
    result.reason = Reason::provider_lost;
  } else if (!policies_[target].writable) {
    result.status = Status::rejected;
    result.reason = Reason::not_writable;
  } else
    for (const auto& command : candidate.commands)
      if (command && active(command->status) && command->request.target == request.target) {
        result.status = Status::rejected;
        result.reason = Reason::busy;
        break;
      }
  candidate.commands[slot] = result;
  candidate.identifier_watermark = request.identifier;
  candidate.last_now = now;
  state_ = candidate;
  return result;
}
std::optional<Request> Coordinator::dispatch(std::uint64_t identifier, Clock::time_point now,
                                             bool permitted) {
  check_time(now);
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  const auto index = command_index(candidate.commands, identifier);
  check(identifier && index < candidate.commands.size(),
        "unknown vehicle command dispatch identifier");
  auto& command = *candidate.commands[index];
  std::optional<Request> work;
  if (command.status == Status::accepted) {
    if (!candidate.connected || command.request.provider_generation != candidate.generation) {
      command.status = Status::failed;
      command.reason = Reason::provider_lost;
      command.updated_at = now;
    } else if (!permitted) {
      command.status = Status::rejected;
      command.reason = Reason::interlock;
      command.updated_at = now;
    } else {
      command.status = Status::pending;
      command.dispatched_at = now;
      command.updated_at = now;
      work = command.request;
    }
  }
  candidate.last_now = now;
  state_ = candidate;
  return work;
}
Snapshot Coordinator::acknowledge(const Acknowledgement& acknowledgement, Clock::time_point now) {
  check_time(now);
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  const auto index = command_index(candidate.commands, acknowledgement.identifier);
  check(acknowledgement.identifier && index < candidate.commands.size(),
        "unknown vehicle command acknowledgement identifier");
  auto& command = *candidate.commands[index];
  if (command.acknowledgement && *command.acknowledgement == acknowledgement) {
    const auto outcome = command;
    candidate.last_now = now;
    state_ = candidate;
    return outcome;
  }
  check(candidate.connected && acknowledgement.provider_generation == candidate.generation &&
            acknowledgement.provider_generation == command.request.provider_generation &&
            acknowledgement.target == command.request.target,
        "vehicle command acknowledgement provider/target mismatch or provider lost");
  const auto target = target_index(acknowledgement.target);
  check(command.dispatched_at && acknowledgement.sequence &&
            acknowledgement.reported_at >= *command.dispatched_at &&
            acknowledgement.reported_at <= now,
        "invalid vehicle command acknowledgement sequence or original report time");
  if (command.acknowledgement)
    check(acknowledgement.sequence > command.acknowledgement->sequence &&
              acknowledgement.reported_at >= command.acknowledgement->reported_at,
          "vehicle command acknowledgement ordering conflict");
  check(acknowledgement.status == ReplyStatus::pending || terminal_reply(acknowledgement.status),
        "invalid vehicle command reply status");
  if (acknowledgement.status == ReplyStatus::applied) {
    check(
        acknowledgement.confirmed &&
            acknowledgement.confirmed->provider_generation == acknowledgement.provider_generation &&
            acknowledgement.confirmed->target == acknowledgement.target &&
            valid_origin(acknowledgement.confirmed->origin) &&
            valid_value(policies_[target], acknowledgement.confirmed->value, false) &&
            acknowledgement.confirmed->confirmed_at >= *command.dispatched_at &&
            acknowledgement.confirmed->confirmed_at <= acknowledgement.reported_at,
        "invalid vehicle command applied confirmation");
  } else
    check(!acknowledgement.confirmed,
          "non-applied vehicle command reply contains actual confirmation");
  check(command.status == Status::pending || command.status == Status::timed_out,
        "vehicle command terminal outcome conflict");
  if (command.status == Status::timed_out) {
    check(!command.acknowledgement || !terminal_reply(command.acknowledgement->status),
          "conflicting late terminal vehicle command acknowledgement");
    // Receipt at/after the deadline is diagnostic only, even if a timely
    // source confirmation was delayed in transit, or actual application was late.
    command.acknowledgement = acknowledgement;
  } else {
    if (acknowledgement.status == ReplyStatus::applied) {
      check(acknowledgement.confirmed->confirmed_at < command.request.deadline,
            "vehicle command application occurred at/after deadline");
      check(publish_confirmation(candidate.confirmed[target], candidate.available[target],
                                 *acknowledgement.confirmed, now, policies_[target].maximum_age,
                                 true),
            "conflicting equal-date vehicle command applied state");
      command.status = Status::applied;
      command.reason = Reason::none;
    } else if (acknowledgement.status == ReplyStatus::rejected) {
      command.status = Status::rejected;
      command.reason = Reason::provider_rejected;
    } else if (acknowledgement.status == ReplyStatus::failed) {
      command.status = Status::failed;
      command.reason = Reason::provider_failed;
    }
    command.updated_at = acknowledgement.reported_at;
    command.acknowledgement = acknowledgement;
  }
  const auto outcome = command;
  candidate.last_now = now;
  state_ = candidate;
  return outcome;
}
void Coordinator::observe(const ConfirmedState& observation, Clock::time_point now) {
  check_time(now);
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  const auto target = target_index(observation.target);
  check(candidate.connected && candidate.generation &&
            observation.provider_generation == candidate.generation,
        "vehicle command observed provider generation mismatch or provider lost");
  check(observation.confirmed_at >= candidate.started_at && observation.confirmed_at <= now &&
            valid_origin(observation.origin) &&
            valid_value(policies_[target], observation.value, false),
        "invalid original vehicle command observed state");
  check(publish_confirmation(candidate.confirmed[target], candidate.available[target], observation,
                             now, policies_[target].maximum_age, false),
        "vehicle command observed state reversed, conflicted or revived after loss");
  candidate.last_now = now;
  state_ = candidate;
}
std::optional<ConfirmedState> Coordinator::read(const Key& target, Clock::time_point now) {
  check_time(now);
  const auto index = target_index(target);
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  const auto result = candidate.available[index] ? candidate.confirmed[index] : std::nullopt;
  candidate.last_now = now;
  state_ = candidate;
  return result;
}
std::optional<Snapshot> Coordinator::snapshot(std::uint64_t identifier, Clock::time_point now) {
  check_time(now);
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  const auto index = command_index(candidate.commands, identifier);
  const auto result = index < candidate.commands.size() ? candidate.commands[index] : std::nullopt;
  candidate.last_now = now;
  state_ = candidate;
  return result;
}
void Coordinator::poll(Clock::time_point now) {
  check_time(now);
  auto candidate = state_;
  expire(candidate, now, policies_, target_count_);
  candidate.last_now = now;
  state_ = candidate;
}
std::optional<TargetPolicy> Coordinator::policy(const Key& target) const {
  check(owner_ == std::this_thread::get_id(),
        "vehicle command coordinator used by another owner thread");
  if (!valid_key(target)) return std::nullopt;
  for (std::size_t i = 0; i < target_count_; ++i)
    if (policies_[i].key == target) return policies_[i];
  return std::nullopt;
}
}  // namespace headunit::vehicle_control
