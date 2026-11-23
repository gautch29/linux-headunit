// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_applied_provider.hpp>

#include <chrono>
#include <ratio>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace headunit::theme::vehicle {
namespace {
namespace control = headunit::vehicle_control;
using Milliseconds = std::chrono::milliseconds;

Clock::time_point host_time(std::uint64_t now_ms) {
  // The host clock has at least millisecond precision. Coarsening its maximum
  // before converting the caller's unsigned time avoids signed overflow.
  static_assert(std::is_integral_v<Clock::rep>);
  static_assert(std::ratio_less_equal_v<Clock::period, std::milli>);
  constexpr auto maximum = std::chrono::duration_cast<Milliseconds>(Clock::duration::max()).count();
  if (now_ms > static_cast<std::uint64_t>(maximum))
    throw std::invalid_argument("confirmed-state provider time exceeds the host clock");
  return Clock::time_point{std::chrono::duration_cast<Clock::duration>(
      Milliseconds{static_cast<Milliseconds::rep>(now_ms)})};
}

std::uint64_t confirmation_ms(Clock::time_point confirmed_at) {
  if (confirmed_at.time_since_epoch() < Clock::duration::zero())
    throw std::invalid_argument("confirmed-state provider received a negative confirmation date");
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<Milliseconds>(confirmed_at.time_since_epoch()).count());
}

class ConfirmedProvider final : public Provider {
  struct Frontier {
    control::Key key{};
    std::optional<control::ConfirmedState> confirmation{};
    std::uint64_t sampled_ms = 0;
    bool withdrawn = false;
  };
  std::shared_ptr<control::Coordinator> coordinator_;
  OwnerClock owner_clock_;
  std::array<Frontier, control::maximum_targets> frontiers_{};
  std::size_t count_ = 0;
  std::optional<std::uint64_t> service_ms_{};

  Frontier& frontier(const control::Key& key) {
    for (std::size_t i = 0; i < count_; ++i)
      if (frontiers_[i].key == key) return frontiers_[i];
    if (count_ == frontiers_.size())
      throw std::invalid_argument("confirmed-state provider target capacity exceeded");
    auto& result = frontiers_[count_++];
    result.key = key;
    return result;
  }

 public:
  ConfirmedProvider(std::shared_ptr<control::Coordinator> coordinator, OwnerClock owner_clock)
      : coordinator_(std::move(coordinator)), owner_clock_(std::move(owner_clock)) {
    if (!coordinator_ || !owner_clock_)
      throw std::invalid_argument(
          "confirmed-state provider requires a coordinator and explicit owner clock");
  }

  std::optional<Sample> read(std::string_view semantic_key, std::uint64_t now_ms) override {
    const auto service_start = host_time(now_ms);
    if (service_ms_ && now_ms < *service_ms_)
      throw std::invalid_argument("confirmed-state provider service tick moved backwards");
    const auto key = control::parse_key(semantic_key);
    if (!key || !coordinator_->policy(*key)) return {};
    const auto owner_now = owner_clock_();
    if (owner_now < service_start)
      throw std::invalid_argument(
          "confirmed-state provider service tick is ahead of its owner clock");
    // Invalid caller time/thread must leave this adapter's state unchanged.
    const auto actual = coordinator_->read(*key, owner_now);
    if (actual && confirmation_ms(actual->confirmed_at) > now_ms) {
      // A real owner operation may cross the caller's service tick. Wait for
      // that original confirmation date to become eligible; do not consume or
      // withdraw the confirmation frontier, nor invent a timestamp to fit it.
      service_ms_ = now_ms;
      return {};
    }
    auto& previous = frontier(*key);
    service_ms_ = now_ms;
    if (!actual) {
      previous.withdrawn = true;
      return {};
    }
    const auto date = confirmation_ms(actual->confirmed_at);
    if (previous.confirmation) {
      if (*actual == *previous.confirmation) {
        if (previous.withdrawn) return {};
      } else {
        // Service timestamps cannot distinguish acquisitions within one ms.
        // Withdraw instead of silently retaining an older value or fabricating
        // a later date. Loss also leaves this ordering frontier intact.
        if (date <= previous.sampled_ms ||
            actual->confirmed_at <= previous.confirmation->confirmed_at ||
            actual->provider_generation < previous.confirmation->provider_generation) {
          previous.withdrawn = true;
          return {};
        }
      }
    }
    previous.confirmation = *actual;
    previous.sampled_ms = date;
    previous.withdrawn = false;
    // Ambient state has no qualified Apple characteristic representation.
    // Never coerce the composite command into a scalar Climate/settings value.
    return std::visit(
        [date](const auto& value) -> std::optional<Sample> {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, control::AmbientLightValue>)
            return {};
          else
            return Sample{opack::Value{value}, date};
        },
        actual->value);
  }

  opack::Value apply(std::string_view, const opack::Value&, std::uint64_t) override {
    throw std::invalid_argument(
        "confirmed-state provider cannot synchronously acknowledge a vehicle command");
  }
};
}  // namespace

std::shared_ptr<Provider> confirmed_state_provider(
    std::shared_ptr<control::Coordinator> coordinator, OwnerClock owner_clock) {
  return std::make_shared<ConfirmedProvider>(std::move(coordinator), std::move(owner_clock));
}
}  // namespace headunit::theme::vehicle
