// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/camera_state.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace headunit::theme::vehicle {
void CameraStateSnapshot::retire_off_locked() noexcept {
  if (off_request_ && off_result_.status == OffStatus::pending)
    off_result_.status = OffStatus::retired;
}
bool CameraStateSnapshot::pending_off_locked(std::uint64_t generation, std::uint64_t now) {
  if (!off_request_ || generation != generation_ || generation != off_request_->generation ||
      !active_ || closed_ || off_result_.status != OffStatus::pending)
    return false;
  if (now < off_request_->requested_ns) {
    retire_off_locked();
    return false;
  }
  if (now >= off_request_->deadline_ns) {
    off_result_.status = OffStatus::timed_out;
    return false;
  }
  return true;
}
std::optional<CameraStateSnapshot::OffRequest> CameraStateSnapshot::request_off(
    std::uint64_t now, std::uint64_t deadline) {
  std::lock_guard lock(mutex_);
  if (!active_ || closed_ || !now || now < observed_ns_ || deadline <= now ||
      deadline - now > maximum_off_wait_ns)
    return {};
  if (off_request_ && now < off_request_->requested_ns) return {};
  if (off_request_ && pending_off_locked(generation_, now)) return {};
  if (next_off_sequence_ == std::numeric_limits<std::uint64_t>::max()) return {};
  off_request_ = OffRequest{generation_, ++next_off_sequence_, now, deadline};
  off_result_ = OffResult{};
  return off_request_;
}
std::optional<CameraStateSnapshot::OffRequest> CameraStateSnapshot::pending_off(
    std::uint64_t generation, std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if (!pending_off_locked(generation, now) || off_result_.withdrawn_ns) return {};
  return off_request_;
}
bool CameraStateSnapshot::owner_off(std::uint64_t generation, std::uint64_t sequence,
                                    std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if (!off_request_ || sequence != off_request_->sequence || !pending_off_locked(generation, now) ||
      off_result_.withdrawn_ns || now < observed_ns_)
    return false;
  off_result_.withdrawn_ns = now;
  visible_ = false;
  observed_ns_ = off_ns_ = now;
  return true;
}
void CameraStateSnapshot::cancel_off(std::uint64_t generation) noexcept {
  std::lock_guard lock(mutex_);
  if (generation == generation_) retire_off_locked();
}
void CameraStateSnapshot::cancel_off(std::uint64_t generation, std::uint64_t sequence) noexcept {
  std::lock_guard lock(mutex_);
  if (off_request_ && generation == off_request_->generation && sequence == off_request_->sequence)
    retire_off_locked();
}
std::uint64_t CameraStateSnapshot::off_for_composition(std::uint64_t generation,
                                                       std::uint64_t now) {
  std::lock_guard lock(mutex_);
  return pending_off_locked(generation, now) && off_result_.withdrawn_ns &&
                 now >= off_result_.withdrawn_ns
             ? off_request_->sequence
             : 0;
}
bool CameraStateSnapshot::presented_off(std::uint64_t generation, std::uint64_t sequence,
                                        std::uint64_t submission_epoch, std::uint64_t current_epoch,
                                        std::uint64_t draw, std::uint64_t submitted,
                                        std::uint64_t presented, std::uint64_t observed) {
  std::lock_guard lock(mutex_);
  if (!off_request_ || sequence != off_request_->sequence ||
      !pending_off_locked(generation, observed) || !off_result_.withdrawn_ns ||
      submission_epoch != current_epoch || !draw || submitted < off_result_.withdrawn_ns ||
      presented < submitted || observed < presented)
    return false;
  off_result_.status = OffStatus::applied;
  off_result_.presented_ns = presented;
  return true;
}
std::optional<CameraStateSnapshot::OffResult> CameraStateSnapshot::off_result(
    std::uint64_t generation, std::uint64_t sequence, std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if (!off_request_ || generation != off_request_->generation || sequence != off_request_->sequence)
    return {};
  (void)pending_off_locked(generation, now);
  if (generation != generation_ || !active_ || closed_) {
    auto result = off_result_;
    result.status = OffStatus::retired;
    return result;
  }
  return off_result_;
}
std::uint64_t CameraStateSnapshot::begin(std::uint64_t now) {
  std::lock_guard lock(mutex_);
  if (closed_) throw std::invalid_argument("camera connection is retired");
  if (generation_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("camera state generation exhausted");
  if (generation_ && now < observed_ns_)
    throw std::invalid_argument("camera state owner clock reversed");
  retire_off_locked();
  ++generation_;
  active_ = true;
  visible_ = false;
  observed_ns_ = off_ns_ = now;
  capture_ns_ = 0;
  return generation_;
}
bool CameraStateSnapshot::observe(std::uint64_t generation, bool visible, std::uint64_t capture,
                                  std::uint64_t observed) {
  std::lock_guard lock(mutex_);
  if (!generation || generation != generation_ || !active_) return false;
  if (observed < observed_ns_) {
    active_ = false;
    visible_ = false;
    retire_off_locked();
    return false;
  }
  observed_ns_ = observed;
  if (!visible) {
    // Date the transition, not every unavailable poll. Hardware acquisition
    // precedes delivery; advancing this cutoff while waiting would reject each
    // delayed frame forever. begin/owner_off/retire still establish hard barriers.
    if (visible_) off_ns_ = std::max(off_ns_, observed);
    visible_ = false;
    return true;
  }
  if (capture > observed || capture <= off_ns_ || capture < capture_ns_) {
    if (visible_) off_ns_ = std::max(off_ns_, observed);
    visible_ = false;
    return false;
  }
  // Repeated observations retain the original capture date. In particular a
  // heartbeat cannot turn an old image into a new camera frame.
  capture_ns_ = capture;
  visible_ = true;
  return true;
}
void CameraStateSnapshot::retire(std::uint64_t generation, std::uint64_t now) noexcept {
  std::lock_guard lock(mutex_);
  if (!generation || generation != generation_) return;
  retire_off_locked();
  active_ = false;
  visible_ = false;
  observed_ns_ = std::max(observed_ns_, now);
  off_ns_ = observed_ns_;
}
void CameraStateSnapshot::close(std::uint64_t now) noexcept {
  std::lock_guard lock(mutex_);
  retire_off_locked();
  closed_ = true;
  active_ = false;
  visible_ = false;
  observed_ns_ = std::max(observed_ns_, now);
  off_ns_ = observed_ns_;
}
Sample CameraStateSnapshot::read(std::uint64_t now, std::uint64_t age) {
  if (age < 100 || age > 1000) throw std::invalid_argument("invalid camera state freshness");
  if (now > std::numeric_limits<std::uint64_t>::max() / 1000000)
    throw std::invalid_argument("camera read date overflow");
  std::lock_guard lock(mutex_);
  const auto observed = observed_ns_ / 1000000, capture = capture_ns_ / 1000000;
  if (active_ && observed <= now && now - observed < age) {
    if (!visible_) return {opack::Value{false}, now};
    if (capture <= now && now - capture < age) return {opack::Value{true}, capture};
  }
  // A raced owner observation may postdate this loop's sampled now. Fail
  // closed for this read, but don't retire that genuinely newer observation.
  if (observed <= now) {
    if (visible_) off_ns_ = std::max(off_ns_, now * 1000000);
    visible_ = false;
  }
  // A receiver-owned negative decision at the actual read time. This date is
  // never used for true/capture publication. Missing/dead/stalled owners cannot
  // expose an invalid Boolean or keep a camera view selected indefinitely.
  return {opack::Value{false}, now};
}
namespace {
// All confirmation callbacks run on the receiver owner thread. Pending writes
// retain this one actual-clock frontier so cancellation/new operations cannot
// reset it; this creates neither a clock nor another acquisition owner.
struct ConfirmationClock {
  explicit ConfirmationClock(std::function<std::uint64_t()> clock) : now_ns(std::move(clock)) {}
  std::optional<std::uint64_t> sample(std::uint64_t service_tick_ms) {
    const auto fine = now_ns();
    if (fine / 1000000 < service_tick_ms || (observed_ns && fine < *observed_ns)) return {};
    observed_ns = fine;
    return fine;
  }
  std::function<std::uint64_t()> now_ns;
  std::optional<std::uint64_t> observed_ns;
};
class CameraProvider final : public Provider {
 public:
  CameraProvider(std::shared_ptr<Provider> base, CameraCapability c,
                 std::shared_ptr<CameraStateSnapshot> state, std::function<std::uint64_t()> clock)
      : base_(std::move(base)),
        configuration_(std::move(c)),
        state_(std::move(state)),
        clock_(clock ? std::make_shared<ConfirmationClock>(std::move(clock)) : nullptr) {
    if (!base_ || !state_ || configuration_.binding.empty() || configuration_.max_age_ms < 100 ||
        configuration_.max_age_ms > 1000)
      throw std::invalid_argument("camera state requires a validated configuration and owner");
  }
  std::optional<Sample> read(std::string_view key, std::uint64_t now) override {
    if (key == configuration_.binding) return state_->read(now, configuration_.max_age_ms);
    return base_->read(key, now);
  }
  bool supports_confirmation(std::string_view key) const override {
    return key == configuration_.binding ? bool(clock_) : base_->supports_confirmation(key);
  }
  std::optional<PendingWrite> start_confirmation(std::string_view key, const opack::Value& value,
                                                 std::uint64_t now,
                                                 std::uint64_t deadline) override {
    if (key != configuration_.binding) return base_->start_confirmation(key, value, now, deadline);
    if (!clock_ || value != opack::Value{false} ||
        deadline > std::numeric_limits<std::uint64_t>::max() / 1000000)
      return {};
    auto state = state_;
    auto clock = clock_;
    // Allocate callback/identity storage before creating owner intent. Once
    // request_off succeeds only nonthrowing identity assignment and ownership
    // transfer remain; later wrapper/batch failures own exact cancellation.
    auto request = std::make_shared<CameraStateSnapshot::OffRequest>();
    PendingWrite pending{
        [state, clock, request](std::uint64_t tick) {
          const auto fine = clock->sample(tick);
          if (!fine || *fine < request->requested_ns)
            return WriteCompletion{WriteCompletion::Status::retired, {}, {}};
          const auto result = state->off_result(request->generation, request->sequence, *fine);
          // A cached applied receipt cannot pass after actual time reaches the
          // original deadline merely because this loop's earlier tick has not.
          if (*fine >= request->deadline_ns || !result)
            return WriteCompletion{WriteCompletion::Status::retired, {}, {}};
          if (result->status == CameraStateSnapshot::OffStatus::pending) return WriteCompletion{};
          if (result->status != CameraStateSnapshot::OffStatus::applied)
            return WriteCompletion{WriteCompletion::Status::retired, {}, {}};
          if (result->presented_ns / 1000000 > tick) return WriteCompletion{};
          return WriteCompletion{WriteCompletion::Status::applied, opack::Value{false},
                                 result->presented_ns / 1000000};
        },
        [state, request] {
          state->cancel_off(request->generation, request->sequence);
        }};
    // now is the earlier service tick, not the fine dispatch instant. Decoding
    // may cross milliseconds; the actual request still has the original deadline.
    const auto fine = clock->sample(now);
    const auto deadline_ns = deadline * 1000000;
    if (!fine || *fine >= deadline_ns) return {};
    const auto admitted = state->request_off(*fine, deadline_ns);
    if (!admitted) return {};
    *request = *admitted;
    return std::optional<PendingWrite>{std::move(pending)};
  }
  opack::Value apply(std::string_view key, const opack::Value& value, std::uint64_t now) override {
    if (key == configuration_.binding)
      throw std::invalid_argument("camera state is owner-controlled");
    return base_->apply(key, value, now);
  }

 private:
  std::shared_ptr<Provider> base_;
  CameraCapability configuration_;
  std::shared_ptr<CameraStateSnapshot> state_;
  std::shared_ptr<ConfirmationClock> clock_;
};
}  // namespace
std::shared_ptr<Provider> camera_state_provider(std::shared_ptr<Provider> base,
                                                const CameraCapability& c,
                                                std::shared_ptr<CameraStateSnapshot> state,
                                                std::function<std::uint64_t()> clock) {
  return std::make_shared<CameraProvider>(std::move(base), c, std::move(state), std::move(clock));
}
}  // namespace headunit::theme::vehicle
