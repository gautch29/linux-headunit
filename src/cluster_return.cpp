// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/cluster_return.hpp>
#include <headunit/presentation_feedback_timeout.hpp>
#include <chrono>

namespace headunit {
std::uint64_t ClusterReturn::begin_native(Clock::time_point now, bool feedback_timeout) {
  if (generation_) throw std::logic_error("native cluster presenter is already active");
  generation_ = ++next_generation_;
  token_ = 0;
  not_before_ = now + std::chrono::seconds(1);
  enabled_ = feedback_timeout && !used_;
  callback_ = requested_ = false;
  return generation_;
}
void ClusterReturn::committed(std::uint64_t generation, std::uint64_t token,
                              Clock::time_point now) {
  if (!generation_ || generation != generation_ || !token || token <= token_) return;
  token_ = token;
  committed_at_ = now;
  callback_ = false;
}
void ClusterReturn::frame_done(std::uint64_t generation, std::uint64_t token,
                               Clock::time_point now) {
  if (!generation_ || generation != generation_ || !token || token != token_ || callback_ ||
      now < committed_at_)
    return;
  callback_at_ = now;
  callback_ = true;
}
bool ClusterReturn::request_return(Clock::time_point now) {
  if (!generation_ || !enabled_ || used_ || requested_ || !callback_ ||
      committed_at_ < not_before_ || now < callback_at_ ||
      now - callback_at_ >= std::chrono::milliseconds(250))
    return false;
  requested_ = used_ = true;
  return true;
}
ClusterReturn::CallbackObservation ClusterReturn::callback_observation() const {
  return {committed_at_, callback_at_};
}
void ClusterReturn::abort_return(std::uint64_t generation) {
  if (!generation_ || generation != generation_) return;
  used_ = true;
  enabled_ = callback_ = requested_ = false;
}
bool ClusterReturn::retire_native() {
  const bool requested = requested_;
  generation_ = token_ = 0;
  enabled_ = callback_ = requested_ = false;
  return requested;
}
void run_cluster_presenters(const std::function<void()>& local,
                            const std::function<void(ClusterReturn&, std::uint64_t)>& native,
                            const std::function<Clock::time_point()>& now,
                            const std::function<bool()>& stopping,
                            const std::function<void(std::string_view, bool)>& failure,
                            bool enable_output_retry) {
  ClusterReturn recovery;
  while (!stopping()) {
    bool feedback_timeout = false;
    try {
      local();
      return;
    } catch (const PresentationFeedbackTimeout& error) {
      feedback_timeout = true;
      failure(error.what(), true);
    } catch (const std::exception& error) {
      failure(error.what(), false);
    }
    if (stopping()) return;
    const auto generation = recovery.begin_native(now(), enable_output_retry && feedback_timeout);
    try {
      native(recovery, generation);
    } catch (...) {
      recovery.retire_native();
      throw;
    }
    const bool requested = recovery.retire_native();
    if (!requested || stopping()) return;
  }
}
}  // namespace headunit
