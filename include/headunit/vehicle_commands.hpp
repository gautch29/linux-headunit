// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/vehicle.hpp>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <variant>

namespace headunit::vehicle_control {

inline constexpr std::size_t maximum_targets = 32, maximum_commands = 64;
inline constexpr auto maximum_command_duration = std::chrono::seconds(60);
// Stable semantic provider keys, independent of Apple characteristics and bus addresses.
struct Key {
  std::array<char, 128> bytes{};
  std::uint8_t length = 0;
  bool operator==(const Key&) const = default;
};
bool valid_key(const Key&) noexcept;
std::optional<Key> parse_key(std::string_view) noexcept;
std::string_view key_text(const Key&) noexcept;
// One atomic strip command: sRGB channels, independent white, permille intensity.
// Fade runs on the MCU, never as a stream of control-thread PWM writes.
struct AmbientLightValue {
  std::array<std::uint8_t, 4> rgbw{};
  std::uint16_t brightness_permille = 0;
  std::uint16_t transition_ms = 0;
  bool operator==(const AmbientLightValue&) const = default;
};
inline constexpr unsigned maximum_ambient_transition_ms = 5000;
bool valid_ambient_light_value(const AmbientLightValue&) noexcept;
// Version 1 payload, network order. The outer MCU envelope supplies command
// identity, provider generation, deadline, sequence, clock mapping and integrity.
std::array<std::uint8_t, 9> encode_ambient_light_value(const AmbientLightValue&);
std::optional<AmbientLightValue> decode_ambient_light_value(std::span<const std::uint8_t>) noexcept;
using Value = std::variant<bool, std::int64_t, double, AmbientLightValue>;
enum class ValueKind { boolean, integer, real, ambient_light };
struct TargetPolicy {
  Key key{};
  ValueKind kind = ValueKind::boolean;
  bool writable = false;
  std::optional<double> minimum{}, maximum{}, step{};
  Clock::duration maximum_age = std::chrono::milliseconds(500);
  bool operator==(const TargetPolicy&) const = default;
};
// Numeric bounds/step are finite and bounded by 2^53-1. Non-singleton real
// grids must have a representable first step and a bounded finite grid count.
bool valid_policy(const TargetPolicy&) noexcept;
// Requests must also respect the configured grid. Confirmed actual values
// retain their original precision and must meet only type/finite/range bounds.
bool valid_value(const TargetPolicy&, const Value&, bool request) noexcept;

enum class Origin { unknown, measured, calculated, simulation };
struct ConfirmedState {
  std::uint64_t provider_generation = 0;
  Key target{};
  Value value{false};
  Clock::time_point confirmed_at{};
  Origin origin = Origin::unknown;
  bool operator==(const ConfirmedState&) const = default;
};
struct Request {
  std::uint64_t identifier = 0, provider_generation = 0;
  Key target{};
  Value requested{false};
  Clock::time_point requested_at{}, deadline{};
  bool operator==(const Request&) const = default;
};
enum class Status { accepted, pending, applied, rejected, failed, timed_out };
enum class Reason {
  none,
  not_writable,
  busy,
  interlock,
  provider_rejected,
  provider_failed,
  provider_lost,
  deadline
};
enum class ReplyStatus { pending, applied, rejected, failed };
struct Acknowledgement {
  std::uint64_t identifier = 0, provider_generation = 0, sequence = 0;
  Key target{};
  ReplyStatus status = ReplyStatus::pending;
  Clock::time_point reported_at{};
  std::optional<ConfirmedState> confirmed{};
  bool operator==(const Acknowledgement&) const = default;
};
struct Snapshot {
  Request request{};
  Status status = Status::accepted;
  Reason reason = Reason::none;
  Clock::time_point updated_at{};
  std::optional<Clock::time_point> dispatched_at{};
  std::optional<Acknowledgement> acknowledgement{};
  bool operator==(const Snapshot&) const = default;
};

// One bounded owner-thread control coordinator. It performs no hardware I/O.
// No automatic retries, implicit interlock permission or requested-value publication.
class Coordinator {
 public:
  explicit Coordinator(std::span<const TargetPolicy>);
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;
  void begin_generation(std::uint64_t, Clock::time_point started_at, Clock::time_point now);
  void disconnect(Clock::time_point now);
  Snapshot submit(const Request&, Clock::time_point now);
  // The executor must explicitly authorize this exact command after checking
  // its current target-specific interlocks. A denied command is never returned.
  // An already-dispatched duplicate also returns no work; loss is resolved by
  // timeout/diagnostics, not a potentially repeated physical write.
  std::optional<Request> dispatch(std::uint64_t identifier, Clock::time_point now,
                                  bool permitted = false);
  Snapshot acknowledge(const Acknowledgement&, Clock::time_point now);
  // Independent actual-state readings never complete a pending command merely
  // because they happen to equal its requested value.
  void observe(const ConfirmedState&, Clock::time_point now);
  std::optional<ConfirmedState> read(const Key&, Clock::time_point now);
  std::optional<Snapshot> snapshot(std::uint64_t identifier, Clock::time_point now);
  void poll(Clock::time_point now);
  std::optional<TargetPolicy> policy(const Key&) const;

 private:
  struct State {
    std::array<std::optional<Snapshot>, maximum_commands> commands{};
    std::array<std::optional<ConfirmedState>, maximum_targets> confirmed{};
    std::array<bool, maximum_targets> available{};
    std::uint64_t generation = 0, identifier_watermark = 0;
    Clock::time_point started_at{};
    std::optional<Clock::time_point> last_now{};
    bool connected = false;
  };
  std::array<TargetPolicy, maximum_targets> policies_{};
  std::size_t target_count_ = 0;
  const std::thread::id owner_;
  State state_{};
  void check_time(Clock::time_point) const;
  std::size_t target_index(const Key&) const;
  static void expire(State&, Clock::time_point, const std::array<TargetPolicy, maximum_targets>&,
                     std::size_t);
};

}  // namespace headunit::vehicle_control
