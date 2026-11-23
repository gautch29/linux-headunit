// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>

namespace headunit {

// First observations only; no phone values, protocol payloads or replacement
// readiness state. Dates use the host CLOCK_MONOTONIC domain on each platform.
class ConnectionDiagnostics {
 public:
  enum class Scope { iap, control, startup };
  enum class Stage {
    engine_created,
    accepted,
    sync_received,
    link_established,
    identification_accepted,
    certificate_begin,
    certificate_end,
    challenge_begin,
    challenge_end,
    mfi_authenticated,
    wifi_settings_prepared,
    carplay_start_prepared,
    wifi_iap_received,
    pair_setup_begin,
    pair_setup_end,
    pair_verify_begin,
    pair_verify_end,
    encrypted_control,
    auth_setup_begin,
    auth_setup_end,
    info_begin,
    info_end,
    initial_setup_begin,
    initial_setup_end,
    stream_setup_begin,
    stream_setup_end,
    setup_features,
    negotiated_standard,
    ultra_requested,
    negotiated_ultra,
    main_stream_setup,
    cluster_stream_setup,
    record_accepted,
    clock_locked,
    video_config,
    video_frame,
    presentation_ready,
    retired,
    failed,
    control_listening,
    iap_profile_ready,
    count
  };
  enum class ControlRetirement : unsigned {
    peer_eof = 1,
    socket_error,
    invalid_request,
    full_teardown,
    owner_failure,
    shutdown
  };
  ConnectionDiagnostics(Scope scope, std::uint64_t generation);
  // Diagnostic failures never change the owner transition.
  void mark(Stage stage, unsigned result = 0, std::uint64_t link_generation = 0) noexcept;
  std::uint64_t generation() const { return generation_; }

  // Explicit observation boundary for synthetic tests. Production uses mark().
  bool observe(Stage stage, std::uint64_t date, unsigned result, std::uint64_t link_generation,
               std::ostream& output) noexcept;
  ConnectionDiagnostics(Scope scope, std::uint64_t generation, bool enabled);

 private:
  Scope scope_;
  std::uint64_t generation_;
  bool enabled_;
  std::array<bool, static_cast<unsigned>(Stage::count)> seen_{};
  std::uint64_t start_ = 0, last_ = 0;
};

}  // namespace headunit
