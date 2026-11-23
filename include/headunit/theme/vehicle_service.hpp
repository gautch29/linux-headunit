// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/vehicle_data.hpp>
#include <functional>
#include <map>
#include <vector>
#include <set>

namespace headunit::theme::vehicle {
enum class ValueKind { boolean, integer, real, string, bytes, array };
// Receiver policy, not additional wire fields. Times use one monotonic clock.
struct SignalPolicy {
  std::int64_t iid;
  ValueKind kind;
  std::uint64_t max_age_ms;
  std::optional<double> minimum;
  std::optional<double> maximum;
};

// One source observation, explicit invalidation, or provider withdrawal. A
// value requires its original acquisition time. Withdrawal has neither value
// nor timestamp and retains the previous source-ordering watermark.
struct Publication {
  std::int64_t iid;
  std::optional<opack::Value> value;
  std::optional<std::uint64_t> sampled_ms;
};

// A verified writable characteristic may be bound to a receiver-owned model.
// The callback returns the state actually applied, which is then published to
// subscribers. No writer exists by default and this layer has no car I/O.
struct WriteBinding {
  std::int64_t iid;
  std::function<opack::Value(const opack::Value&)> apply;
};

// Local owner completion contract; no pending status or new opcode is sent on
// the wire. A completion carries its original confirmation date, never poll time.
struct WriteCompletion {
  enum class Status { pending, applied, rejected, retired };
  Status status = Status::pending;
  std::optional<opack::Value> value;
  std::optional<std::uint64_t> sampled_ms;
};
// The provider must report completion for exactly the operation start created.
// The service tick is only a coarse lower bound; matching independent readback
// or an observation predating actual dispatch cannot establish that identity.
struct PendingWrite {
  PendingWrite() = default;
  PendingWrite(std::function<WriteCompletion(std::uint64_t)> poll_operation,
               std::function<void()> cancel_operation)
      : poll(std::move(poll_operation)), cancel(std::move(cancel_operation)) {}
  ~PendingWrite() { retire(); }
  PendingWrite(const PendingWrite&) = delete;
  PendingWrite& operator=(const PendingWrite&) = delete;
  PendingWrite(PendingWrite&& other) noexcept
      : poll(std::move(other.poll)), cancel(std::move(other.cancel)) {
    other.cancel = {};
  }
  PendingWrite& operator=(PendingWrite&& other) noexcept {
    if (this != &other) {
      retire();
      poll = std::move(other.poll);
      cancel = std::move(other.cancel);
      other.cancel = {};
    }
    return *this;
  }
  // Exactly one cancellation owner, including local batch/wrapper exceptions
  // before registration. Disarm before invoking; retirement is idempotent.
  void retire() noexcept {
    auto action = std::move(cancel);
    cancel = {};
    try {
      if (action) action();
    } catch (...) {
    }
  }
  std::function<WriteCompletion(std::uint64_t now_ms)> poll;
  std::function<void()> cancel;
};
struct ConfirmationBinding {
  std::int64_t iid;
  std::function<std::optional<PendingWrite>(const opack::Value&, std::uint64_t now_ms,
                                            std::uint64_t deadline_ms)>
      start;
};

// One connection's vehicle-data service. The profile uses the SDK's
// wire configuration (accessories/services/characteristics arrays). It must be
// supplied by the vehicle integration; no sensor IDs or values are invented.
// Profiles are immutable for this service's lifetime. Reconnect to change one.
class Service {
 public:
  struct Reading {
    std::optional<opack::Value> value;
    std::optional<std::int32_t> error;
    bool operator==(const Reading&) const = default;
  };
  explicit Service(opack::Value configuration = empty_configuration(),
                   std::vector<SignalPolicy> policies = {},
                   std::map<std::int64_t, opack::Value> fixed_configuration = {},
                   std::vector<WriteBinding> writers = {},
                   std::vector<ConfirmationBinding> confirmations = {});
  ~Service();
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;
  // Fixed configuration (units, calibrated gauge bounds) is validated once,
  // never expires and cannot be changed by publish/invalidate. Sensor readings
  // must remain ordinary timed signals; disconnect clears those readings.
  // Writes admit 1..32 unique IIDs with independent confirmed values/errors.
  // Structural failures run no callbacks; a valid batch has no rollback or
  // atomic hardware-transaction promise. Opt-in confirmed writes defer the application reply.
  std::optional<Bytes> handle(std::span<const std::uint8_t> bytes, std::uint64_t now_ms,
                              std::uint64_t response_owner = 0);
  // At most two transport owners and one 1..32-IID batch per owner. A live IID
  // has one operation across both channels. Handle without an owner fails closed.
  static constexpr std::uint64_t confirmation_wait_ms = 5000;
  std::uint64_t attach_response_owner();
  bool pending_response(std::uint64_t owner) const;
  std::optional<Bytes> complete_response(std::uint64_t owner, std::uint64_t now_ms);
  void retire_response_owner(std::uint64_t owner) noexcept;
  void publish(std::int64_t iid, opack::Value value, std::uint64_t sampled_ms,
               std::uint64_t now_ms);
  // At most 512 distinct timed signals. Validate the entire owned batch and
  // monotonic now before changing readings or time. Values require a strictly
  // newer acquisition time; explicit invalidations may repeat the last time.
  // An empty valid batch advances only the service's monotonic time.
  void publish_batch(std::vector<Publication> publications, std::uint64_t now_ms);
  void invalidate(std::int64_t iid, std::uint64_t sampled_ms, std::uint64_t now_ms);
  // Signals read together by one script updater. When any subscribed member
  // changes, updates() resends every subscribed member of its group, because
  // the signed updaters rebuild composite state from a single update batch.
  // Each IID must exist and belong to at most one group of 2..512 members.
  void set_update_groups(const std::vector<std::vector<std::int64_t>>& groups);
  // Provider absence is not a new sensor observation. Clear the value while
  // retaining the last source timestamp, so a later fresh delayed sample can
  // recover without accepting a replay or inventing a new measurement time.
  void withdraw(std::int64_t iid, std::uint64_t now_ms);
  // Sensor-provider loss clears every timed reading immediately, preserving
  // fixed calibration, subscriptions and the shared monotonic time domain.
  void invalidate_dynamic(std::uint64_t now_ms);
  // Call on value changes AND a timer: expiration must notify even if the
  // sensor stops sending. Coalesces changes; no unbounded event backlog.
  std::optional<Bytes> updates(std::uint64_t now_ms);
  void disconnect();
  std::size_t subscription_count() const { return subscriptions_.size(); }
  // Local consumers have independent subscription/delivery state. These reads
  // share the same freshness policy without consuming the phone's updates.
  const opack::Value& profile() const { return configuration_; }
  opack::Value configuration_snapshot(std::uint64_t now_ms) const;
  bool contains(std::int64_t iid) const { return signals_.contains(iid); }
  bool timed(std::int64_t iid) const;
  std::optional<SignalPolicy> policy(std::int64_t iid) const;
  // Local host visibility gates may need immediate OFF within the same
  // millisecond as their last capture. Only false is permitted here; ordinary
  // sensor/true publications retain strict acquisition ordering.
  void deactivate_boolean(std::int64_t iid, std::uint64_t now_ms);
  // Preserve a copied owner's real OFF decision date, including a same-ms
  // true-to-false transition. This cannot regress the acquisition frontier.
  void deactivate_boolean(std::int64_t iid, std::uint64_t sampled_ms, std::uint64_t now_ms);
  // Owner-qualified camera visibility only. OFF is a decision dated at the
  // read/withdrawal time; ON keeps its earlier acquisition date. Track ON's
  // acquisition frontier separately so negative polling cannot starve delivery.
  // The camera owner must enforce activation/generation/withdrawal barriers.
  void publish_visibility_capture(std::int64_t iid, std::uint64_t capture_ms, std::uint64_t now_ms);
  // Host-owned RequestContent intent with a dismiss-only wire writer. OFF
  // decisions do not advance the acquisition frontier of the next leased ON.
  void publish_request_intent(std::int64_t iid, std::uint64_t renewed_ms, std::uint64_t now_ms);
  std::map<std::int64_t, Reading> snapshot(std::span<const std::int64_t> iids,
                                           std::uint64_t now_ms);

 private:
  struct Signal {
    SignalPolicy policy;
    bool supports_invalid;
    std::optional<opack::Value> value;
    std::optional<std::uint64_t> sampled_ms;
    bool fixed = false;
    std::optional<std::uint64_t> visibility_capture_ms = std::nullopt;
  };
  opack::Value configuration_;
  std::map<std::int64_t, Signal> signals_;
  std::map<std::int64_t, std::function<opack::Value(const opack::Value&)>> writers_;
  std::map<std::int64_t, ConfirmationBinding> confirmations_;
  struct Operation {
    std::int64_t iid;
    PendingWrite write;
  };
  struct Batch {
    Command reply;
    // Original coarse service tick bounds dates/deadline; it is not an actual
    // fine dispatch date or evidence that a readback completed an operation.
    std::uint64_t service_tick_ms, deadline_ms;
    std::vector<Operation> operations;
  };
  std::set<std::uint64_t> response_owners_;
  std::map<std::uint64_t, Batch> batches_;
  std::uint64_t next_response_owner_ = 0;
  bool writing(std::int64_t iid) const;
  static void cancel(PendingWrite&) noexcept;
  std::set<std::int64_t> subscriptions_;
  std::map<std::int64_t, std::vector<std::int64_t>> update_groups_;
  std::map<std::int64_t, Reading> delivered_;
  std::optional<std::uint64_t> last_now_;
  void check_time(std::uint64_t now_ms);
  void validate_value(const Signal&, const opack::Value&) const;
  Reading reading(std::int64_t iid, std::uint64_t now_ms) const;
  void append(Command&, std::int64_t, const Reading&) const;
  opack::Value configuration(std::uint64_t now_ms) const;
};
// Existing callers deliberately construct the service without writers.
using ReadOnlyService = Service;
}  // namespace headunit::theme::vehicle
