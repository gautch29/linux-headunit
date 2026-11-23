// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace headunit {
enum class GreetingVariant { off, brand, minimal };
enum class GreetingPhase { inactive, startup, goodbye };
struct GreetingFrame {
  GreetingVariant variant = GreetingVariant::off;
  GreetingPhase phase = GreetingPhase::inactive;
  double opacity = 0;
  double reveal = 0;
};
std::optional<GreetingVariant> parse_greeting_variant(std::string_view);
// Cosmetic UI intent only. No gauge, phone readiness or power authority.
class GreetingSequence {
 public:
  void begin(GreetingPhase, GreetingVariant, Clock::time_point);
  GreetingFrame frame(Clock::time_point);
  void retire();

 private:
  GreetingVariant variant_ = GreetingVariant::off;
  GreetingPhase phase_ = GreetingPhase::inactive;
  Clock::time_point began_{}, last_{};
};
// Bounded private preference and dated cosmetic request. New owners reject
// requests acquired before their creation; redraws cannot renew a request.
class GreetingSequenceFile {
 public:
  explicit GreetingSequenceFile(std::string path, bool boot = false);
  GreetingFrame frame(Clock::time_point);

 private:
  std::string path_;
  Clock::time_point origin_, next_{};
  GreetingVariant variant_ = GreetingVariant::off;
  std::uint64_t consumed_ = 0, seen_ = 0;
  bool boot_ = false;
  GreetingSequence sequence_;
};
// Private volatile marker and actual kernel age: never replay on service restart.
bool claim_greeting_boot();
}  // namespace headunit
