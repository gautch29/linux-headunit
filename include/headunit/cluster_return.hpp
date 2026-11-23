// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <cstdint>
#include <functional>
#include <string_view>

namespace headunit {
// Existing native background's frame callback grants one bounded retry only.
// It is not a presentation receipt or proof that the phone withdrew its surface.
class ClusterReturn {
 public:
  std::uint64_t begin_native(Clock::time_point now, bool feedback_timeout);
  void committed(std::uint64_t generation, std::uint64_t token, Clock::time_point now);
  void frame_done(std::uint64_t generation, std::uint64_t token, Clock::time_point now);
  bool request_return(Clock::time_point now);
  struct CallbackObservation {
    Clock::time_point submitted_at, received_at;
  };
  CallbackObservation callback_observation() const;
  void abort_return(std::uint64_t generation);
  bool retire_native();

 private:
  std::uint64_t generation_ = 0, next_generation_ = 0, token_ = 0;
  Clock::time_point not_before_{}, committed_at_{}, callback_at_{};
  bool enabled_ = false, callback_ = false, requested_ = false, used_ = false;
};
// Both blocking presenters execute on the calling render owner. Each callable
// must fully retire its resources before returning or throwing. No successor
// starts until that unwind has completed; terminal shutdown always wins.
void run_cluster_presenters(const std::function<void()>& local,
                            const std::function<void(ClusterReturn&, std::uint64_t)>& native,
                            const std::function<Clock::time_point()>& now,
                            const std::function<bool()>& stopping,
                            const std::function<void(std::string_view, bool)>& failure,
                            bool enable_output_retry = false);
}  // namespace headunit
