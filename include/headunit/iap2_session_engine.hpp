// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/iap2_handshake.hpp"
#include "headunit/connection_diagnostics.hpp"
#include "headunit/iap2_file_transfer.hpp"
#include "headunit/iap2_carplay.hpp"
#include "headunit/iap2_identity.hpp"
#include "headunit/iap2_mfi_auth.hpp"
#include "headunit/iap2_now_playing.hpp"
#include "headunit/iap2_reliable_link.hpp"
#include "headunit/iap2_wifi_handoff.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <array>

namespace headunit {

struct Iap2SessionOutput {
  std::vector<std::vector<std::uint8_t>> bluetooth;
  std::vector<std::vector<std::uint8_t>> wifi;
  // At most 64 numeric diagnostic notices. Parameters are deliberately absent;
  // these are not an application dispatch queue or semantic acknowledgements.
  std::vector<ControlMessage> unhandled;
};

// One iAP2 link sequence shared by Bluetooth and the later CarPlay data stream.
// Transport ownership and MFi provider lifetime remain with the caller.
class Iap2SessionEngine {
 public:
  using Clock = Iap2ReliableLink::Clock;
  Iap2SessionEngine(std::uint8_t initial_sequence, Iap2Identity identity,
                    MfiAuthenticator* chip = nullptr,
                    std::optional<Iap2WifiHandoff> handoff = std::nullopt,
                    std::optional<Iap2CarplayStart> carplay = std::nullopt,
                    std::uint64_t diagnostic_generation = 0);

  ~Iap2SessionEngine();
  static std::vector<std::uint8_t> sync_preamble();
  Iap2SessionOutput receive_bluetooth(std::span<const std::uint8_t> bytes, Clock::time_point now);
  Iap2SessionOutput receive_wifi(std::span<const std::uint8_t> bytes, Clock::time_point now);
  Iap2SessionOutput send_control(const ControlMessage& message, Clock::time_point now);
  Iap2SessionOutput tick(Clock::time_point now);
  std::shared_ptr<RouteGuidance> navigation() const { return navigation_; }
  std::shared_ptr<Iap2NowPlaying> now_playing() const { return now_playing_; }
  // Copy for the native publisher. Terminal engine state withdraws only the
  // additive host observation, preserving the existing metadata lifetime.
  NowPlayingSnapshot now_playing_snapshot() const;
  unsigned now_playing_requests() const { return now_playing_attempts_; }
  std::size_t now_playing_updates() const { return now_playing_updates_; }
  std::size_t now_playing_rejections() const { return now_playing_rejections_; }
  unsigned navigation_requests() const { return navigation_attempts_; }
  std::size_t navigation_updates() const { return navigation_updates_; }
  std::size_t navigation_rejections() const { return navigation_rejections_; }
  // Transport retirement is irreversible even while another owner retains a
  // shared reference. A new connection must create a new engine.
  void retire() noexcept;
  bool established() const { return reliable_.has_value() && !failed(); }
  bool failed() const {
    return retired_ || handshake_.state() == LinkHandshakeState::timed_out ||
           (reliable_ && reliable_->timed_out()) ||
           (mfi_auth_ && mfi_auth_->state() == Iap2MfiAuth::State::Failed);
  }
  bool wifi_active() const { return wifi_active_; }
  bool handoff_sent() const { return handoff_sent_ || carplay_start_sent_; }
  void transport_accepted() noexcept { diagnostics_.mark(ConnectionDiagnostics::Stage::accepted); }
  std::uint64_t diagnostic_generation() const { return diagnostics_.generation(); }
  bool carplay_start_sent() const { return carplay_start_sent_; }
  LinkHandshakeState handshake_state() const { return handshake_.state(); }
  struct ControlCoverage {
    std::uint16_t identifier = 0;
    std::size_t received = 0, dispatched = 0, rejected = 0, unhandled = 0;
    bool operator==(const ControlCoverage&) const = default;
  };
  struct Coverage {
    std::array<std::size_t, 256> delivered_sessions{};
    std::array<ControlCoverage, 13> known_controls{};
    std::array<std::optional<ControlCoverage>, 32> unknown_controls{};
    std::size_t unknown_evictions = 0, notice_drops = 0, file_reply_drops = 0;
    bool operator==(const Coverage&) const = default;
  };
  // Content-free lifetime counts; link duplicates/gaps are not new deliveries.
  // Dispatch is handler invocation, not applied state or audible presentation.
  const Coverage& coverage() const noexcept { return coverage_; }

 private:
  void require_live();
  void emit(Iap2SessionOutput& output, const LinkFrame& frame, bool bluetooth_only = false) const;
  void receive_frames(Iap2SessionOutput& output, std::span<const LinkFrame> frames,
                      Clock::time_point now);
  void handle_control(Iap2SessionOutput& output, const ControlMessage& message,
                      Clock::time_point now);
  void flush_file_replies(Iap2SessionOutput& output, Clock::time_point now);
  ControlCoverage& control_coverage(std::uint16_t identifier) noexcept;

  ConnectionDiagnostics diagnostics_;
  std::uint8_t initial_sequence_;
  Iap2Identity identity_;
  LinkHandshake handshake_;
  LinkFrameReader bluetooth_reader_;
  LinkFrameReader wifi_reader_;
  ControlMessageReader control_reader_;
  std::optional<Iap2ReliableLink> reliable_;
  std::unique_ptr<Iap2MfiAuth> mfi_auth_;
  std::optional<Iap2WifiHandoff> handoff_;
  std::optional<ControlMessage> carplay_start_;
  std::shared_ptr<RouteGuidance> navigation_;
  std::shared_ptr<Iap2NowPlaying> now_playing_;
  Iap2FileTransfer artwork_transfer_;
  std::deque<std::vector<std::uint8_t>> file_replies_;
  bool identified_ = false;
  unsigned navigation_attempts_ = 0;
  std::size_t navigation_updates_ = 0, navigation_rejections_ = 0;
  std::optional<Clock::time_point> navigation_request_at_;
  unsigned now_playing_attempts_ = 0;
  std::size_t now_playing_updates_ = 0, now_playing_rejections_ = 0;
  std::optional<Clock::time_point> now_playing_request_at_;
  bool retired_ = false;
  bool carplay_start_sent_ = false;
  bool wifi_active_ = false;
  bool handoff_sent_ = false;
  Coverage coverage_;
  std::size_t next_unknown_ = 0;
};

}  // namespace headunit
