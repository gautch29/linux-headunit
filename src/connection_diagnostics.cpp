// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/connection_diagnostics.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <time.h>

namespace headunit {
namespace {
constexpr std::array<std::string_view, static_cast<unsigned>(ConnectionDiagnostics::Stage::count)>
    names{"engine-created",
          "accepted",
          "sync-received",
          "link-established",
          "identification-accepted",
          "certificate-begin",
          "certificate-end",
          "challenge-begin",
          "challenge-end",
          "mfi-authenticated",
          "wifi-settings-prepared",
          "carplay-start-prepared",
          "wifi-iap-received",
          "pair-setup-begin",
          "pair-setup-end",
          "pair-verify-begin",
          "pair-verify-end",
          "encrypted-control",
          "auth-setup-begin",
          "auth-setup-end",
          "info-begin",
          "info-end",
          "initial-setup-begin",
          "initial-setup-end",
          "stream-setup-begin",
          "stream-setup-end",
          "setup-features",
          "negotiated-standard",
          "ultra-requested",
          "negotiated-ultra",
          "main-stream-setup",
          "cluster-stream-setup",
          "record-accepted",
          "clock-locked",
          "video-config",
          "video-frame",
          "presentation-ready",
          "retired",
          "failed",
          "control-listening",
          "iap-profile-ready"};
// Also bound repeated owner generations for an indefinitely running receiver.
std::atomic<unsigned> reports{0};
constexpr unsigned maximum_reports = 512;
bool enabled() {
  const auto* value = std::getenv("HEADUNIT_CONNECTION_DIAGNOSTICS");
  return value && std::string_view(value) == "1";
}
}  // namespace
ConnectionDiagnostics::ConnectionDiagnostics(Scope scope, std::uint64_t generation)
    : ConnectionDiagnostics(scope, generation, enabled()) {}
ConnectionDiagnostics::ConnectionDiagnostics(Scope scope, std::uint64_t generation, bool enabled)
    : scope_(scope), generation_(generation), enabled_(enabled) {}

bool ConnectionDiagnostics::observe(Stage stage, std::uint64_t date, unsigned result,
                                    std::uint64_t link_generation, std::ostream& output) noexcept {
  const auto index = static_cast<unsigned>(stage);
  if (!enabled_ || index >= seen_.size() || seen_[index] || !date || date < last_ ||
      seen_[static_cast<unsigned>(Stage::retired)] ||
      (seen_[static_cast<unsigned>(Stage::failed)] && stage != Stage::retired))
    return false;
  seen_[index] = true;
  if (!start_) start_ = date;
  last_ = date;
  // Saturating process bound. Failure to emit still consumes this observation;
  // it must not retry/spin or affect the connection's actual state.
  auto count = reports.load(std::memory_order_relaxed);
  do {
    if (count >= maximum_reports) return false;
  } while (!reports.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
  try {
    output << "CarPlay connection diagnostic mono_ns=" << date << " scope="
           << (scope_ == Scope::iap       ? "iap"
               : scope_ == Scope::control ? "control"
                                          : "startup")
           << " generation=" << generation_ << " stage=" << names[index]
           << " elapsed_ns=" << date - start_ << " result=" << result
           << " linkGeneration=" << link_generation << '\n'
           << std::flush;
    return bool(output);
  } catch (...) {
    return false;
  }
}
void ConnectionDiagnostics::mark(Stage stage, unsigned result,
                                 std::uint64_t link_generation) noexcept {
  if (!enabled_ || static_cast<unsigned>(stage) >= seen_.size() ||
      seen_[static_cast<unsigned>(stage)])
    return;
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0) return;
  observe(stage, std::uint64_t(now.tv_sec) * 1'000'000'000 + std::uint64_t(now.tv_nsec), result,
          link_generation, std::cout);
}
}  // namespace headunit
