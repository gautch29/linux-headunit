// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_session_engine.hpp"

#include "headunit/iap2_message.hpp"

#include <stdexcept>
#include <utility>
#include <limits>
#include <iostream>

namespace headunit {
namespace {
void increment(std::size_t& value) noexcept {
  if (value != std::numeric_limits<std::size_t>::max()) ++value;
}
constexpr std::array<std::uint16_t, 13> known_control_ids{0x1d00, 0x1d02, 0x5201, 0x5202, 0x5001,
                                                          0xaa00, 0xaa01, 0xaa02, 0xaa03, 0xaa04,
                                                          0xaa05, 0x5702, 0x4300};
}  // namespace

Iap2SessionEngine::Iap2SessionEngine(std::uint8_t initial_sequence, Iap2Identity identity,
                                     MfiAuthenticator* chip, std::optional<Iap2WifiHandoff> handoff,
                                     std::optional<Iap2CarplayStart> carplay,
                                     std::uint64_t diagnostic_generation)
    : diagnostics_(ConnectionDiagnostics::Scope::iap, diagnostic_generation),
      initial_sequence_(initial_sequence),
      identity_(std::move(identity)),
      handshake_(initial_sequence),
      mfi_auth_(chip ? std::make_unique<Iap2MfiAuth>(*chip) : nullptr),
      handoff_(std::move(handoff)) {
  for (std::size_t index = 0; index < known_control_ids.size(); ++index)
    coverage_.known_controls[index].identifier = known_control_ids[index];
  if (identity_.route_guidance)
    navigation_ = std::make_shared<RouteGuidance>(*identity_.route_guidance);
  if (identity_.supports_now_playing) now_playing_ = std::make_shared<Iap2NowPlaying>();
  if (identity_.supports_theme_assets && (!carplay || !carplay->installed_asset))
    throw std::invalid_argument("theme support requires installed asset metadata");
  if (carplay) {
    if (bool(carplay->installed_asset) != identity_.supports_theme_assets)
      throw std::invalid_argument("theme identity/start metadata mismatch");
    carplay_start_ = make_carplay_start(*carplay);
  }
  diagnostics_.mark(ConnectionDiagnostics::Stage::engine_created);
}

std::vector<std::uint8_t> Iap2SessionEngine::sync_preamble() {
  const auto marker = LinkHandshake::sync_preamble();
  return {marker.begin(), marker.end()};
}

Iap2SessionEngine::~Iap2SessionEngine() {
  retire();
}

void Iap2SessionEngine::retire() noexcept {
  if (retired_) return;
  retired_ = true;
  diagnostics_.mark(ConnectionDiagnostics::Stage::retired);
  try {
    unsigned printed = 0, omitted = 0;
    for (std::size_t session = 0; session < coverage_.delivered_sessions.size(); ++session) {
      const auto delivered = coverage_.delivered_sessions[session];
      if (!delivered) continue;
      if (printed == 32) {
        ++omitted;
        continue;
      }
      ++printed;
      std::cout << "iAP coverage generation=" << diagnostic_generation() << " session=" << session
                << " delivered=" << delivered << '\n';
    }
    const auto report = [&](const ControlCoverage& count) {
      if (!count.received) return;
      std::cout << "iAP coverage control=" << count.identifier << " received=" << count.received
                << " dispatched=" << count.dispatched << " rejected=" << count.rejected
                << " unhandled=" << count.unhandled << '\n';
    };
    for (const auto& count : coverage_.known_controls)
      report(count);
    for (const auto& count : coverage_.unknown_controls)
      if (count) report(*count);
    std::cout << "iAP coverage omittedSessions=" << omitted
              << " unknownEvictions=" << coverage_.unknown_evictions
              << " noticeDrops=" << coverage_.notice_drops
              << " fileReplyDrops=" << coverage_.file_reply_drops << '\n'
              << std::flush;
  } catch (...) {
    // Diagnostic failure cannot prevent irreversible transport retirement.
  }
}

NowPlayingSnapshot Iap2SessionEngine::now_playing_snapshot() const {
  auto state = now_playing_ ? now_playing_->snapshot() : NowPlayingSnapshot{};
  if (failed()) state.observation = {};
  return state;
}

void Iap2SessionEngine::require_live() {
  if (!failed()) return;
  diagnostics_.mark(ConnectionDiagnostics::Stage::failed);
  throw std::runtime_error("iAP2 session is retired or terminally failed");
}

void Iap2SessionEngine::emit(Iap2SessionOutput& output, const LinkFrame& frame,
                             bool bluetooth_only) const {
  auto bytes = encode_link_frame(frame);
  (wifi_active_ && !bluetooth_only ? output.wifi : output.bluetooth).push_back(std::move(bytes));
}

Iap2SessionEngine::ControlCoverage& Iap2SessionEngine::control_coverage(
    std::uint16_t identifier) noexcept {
  for (auto& entry : coverage_.known_controls)
    if (entry.identifier == identifier) return entry;
  for (auto& entry : coverage_.unknown_controls)
    if (entry && entry->identifier == identifier) return *entry;
  auto& entry = coverage_.unknown_controls[next_unknown_];
  if (entry) increment(coverage_.unknown_evictions);
  entry = ControlCoverage{identifier};
  next_unknown_ = (next_unknown_ + 1) % coverage_.unknown_controls.size();
  return *entry;
}

void Iap2SessionEngine::handle_control(Iap2SessionOutput& output, const ControlMessage& message,
                                       Clock::time_point now) {
  auto& count = control_coverage(message.identifier);
  increment(count.received);
  try {
    std::optional<ControlMessage> reply;
    if (message.identifier == 0x1d00) {
      increment(count.dispatched);
      reply = make_identification_reply(identity_);
    } else if (message.identifier == 0x1d02) {
      increment(count.dispatched);
      identified_ = true;
      diagnostics_.mark(ConnectionDiagnostics::Stage::identification_accepted);
    } else if (navigation_ && (message.identifier == 0x5201 || message.identifier == 0x5202)) {
      increment(count.dispatched);
      if (navigation_->receive(message, now))
        ++navigation_updates_;
      else {
        ++navigation_rejections_;
        increment(count.rejected);
      }
    } else if (message.identifier == 0x5001 && now_playing_) {
      increment(count.dispatched);
      const auto previous = now_playing_->snapshot();
      if (now_playing_->receive(message)) {
        ++now_playing_updates_;
        const auto current = now_playing_->snapshot();
        // File IDs are only eight bits and can be reused by a later track. An
        // old partial cover must not complete against the newly selected item.
        if (previous.track_id != current.track_id || previous.title != current.title ||
            previous.artwork_transfer_id != current.artwork_transfer_id)
          artwork_transfer_.clear();
      } else {
        ++now_playing_rejections_;
        increment(count.rejected);
      }
    } else if (message.identifier >= 0xaa00 && message.identifier <= 0xaa05 && mfi_auth_) {
      increment(count.dispatched);
      using Stage = ConnectionDiagnostics::Stage;
      if (message.identifier == 0xaa00) diagnostics_.mark(Stage::certificate_begin);
      if (message.identifier == 0xaa02) diagnostics_.mark(Stage::challenge_begin);
      reply = mfi_auth_->on_message(message);
      if (message.identifier == 0xaa00) diagnostics_.mark(Stage::certificate_end);
      if (message.identifier == 0xaa02) diagnostics_.mark(Stage::challenge_end);
      if (mfi_auth_->state() == Iap2MfiAuth::State::Complete)
        diagnostics_.mark(Stage::mfi_authenticated);
      require_live();
    } else if (message.identifier == 0x5702 && handoff_) {
      increment(count.dispatched);
      if (!mfi_auth_ || mfi_auth_->state() != Iap2MfiAuth::State::Complete) {
        throw std::invalid_argument("Wi-Fi handoff before MFi authentication");
      }
      reply = handoff_->on_message(message);
      handoff_sent_ = reply.has_value();
    } else if (message.identifier == 0x4300 && carplay_start_) {
      increment(count.dispatched);
      if (!mfi_auth_ || mfi_auth_->state() != Iap2MfiAuth::State::Complete)
        throw std::invalid_argument("CarPlay start before MFi authentication");
      if (wireless_carplay_available(message) && !carplay_start_sent_) {
        reply = carplay_start_;
        carplay_start_sent_ = true;
      }
    } else {
      increment(count.unhandled);
      if (output.unhandled.size() < 64)
        output.unhandled.push_back({message.identifier, {}});
      else
        increment(coverage_.notice_drops);
    }
    if (reply) {
      const auto payload = encode_control_message(*reply);
      const auto frame = reliable_->send(10, payload, now);
      if (!frame) throw std::runtime_error("iAP2 response window is full");
      emit(output, *frame);
      if (reply->identifier == 0x5703)
        diagnostics_.mark(ConnectionDiagnostics::Stage::wifi_settings_prepared);
      if (reply->identifier == 0x4301)
        diagnostics_.mark(ConnectionDiagnostics::Stage::carplay_start_prepared);
    }
  } catch (...) {
    increment(count.rejected);
    throw;
  }
}

void Iap2SessionEngine::receive_frames(Iap2SessionOutput& output, std::span<const LinkFrame> frames,
                                       Clock::time_point now) {
  for (const auto& frame : frames) {
    if (!reliable_ && frame.control == 0xc0 && frame.session == 0 &&
        handshake_.state() == LinkHandshakeState::established) {
      reliable_.emplace(initial_sequence_, frame.sequence);
      diagnostics_.mark(ConnectionDiagnostics::Stage::link_established);
      continue;
    }
    if (!reliable_) continue;
    auto result = reliable_->receive(frame);
    for (const auto& outgoing : result.outgoing)
      emit(output, outgoing);
    for (const auto& delivered : result.delivered) {
      increment(coverage_.delivered_sessions[delivered.session]);
      if (delivered.session == 12 && now_playing_) {
        auto transfer = artwork_transfer_.receive(delivered.payload);
        if (!transfer.reply.empty() && file_replies_.size() < 8)
          file_replies_.push_back(std::move(transfer.reply));
        else if (!transfer.reply.empty())
          increment(coverage_.file_reply_drops);
        if (transfer.completed)
          now_playing_->accept_artwork(transfer.completed->first, transfer.completed->second);
        continue;
      }
      if (delivered.session != 10) continue;
      for (const auto& message : control_reader_.feed(delivered.payload)) {
        handle_control(output, message, now);
      }
    }
  }
  flush_file_replies(output, now);
}

void Iap2SessionEngine::flush_file_replies(Iap2SessionOutput& output, Clock::time_point now) {
  while (!file_replies_.empty() && reliable_ && reliable_->unacknowledged() < 4) {
    const auto frame = reliable_->send(12, file_replies_.front(), now);
    if (!frame) break;
    emit(output, *frame);
    file_replies_.pop_front();
  }
}

Iap2SessionOutput Iap2SessionEngine::receive_bluetooth(std::span<const std::uint8_t> bytes,
                                                       Clock::time_point now) {
  require_live();
  Iap2SessionOutput output;
  if (wifi_active_) throw std::invalid_argument("Bluetooth data after Wi-Fi handoff");
  for (const auto& frame : handshake_.feed(bytes, now)) {
    emit(output, frame, true);
  }
  if (handshake_.state() != LinkHandshakeState::waiting_for_sync)
    diagnostics_.mark(ConnectionDiagnostics::Stage::sync_received);
  const auto frames = bluetooth_reader_.feed(bytes);
  receive_frames(output, frames, now);
  return output;
}

Iap2SessionOutput Iap2SessionEngine::receive_wifi(std::span<const std::uint8_t> bytes,
                                                  Clock::time_point now) {
  require_live();
  if (!reliable_) throw std::invalid_argument("Wi-Fi iAP before Bluetooth link");
  wifi_active_ = true;
  if (!bytes.empty()) diagnostics_.mark(ConnectionDiagnostics::Stage::wifi_iap_received);
  Iap2SessionOutput output;
  const auto frames = wifi_reader_.feed(bytes);
  receive_frames(output, frames, now);
  return output;
}

Iap2SessionOutput Iap2SessionEngine::send_control(const ControlMessage& message,
                                                  Clock::time_point now) {
  require_live();
  if (!reliable_) throw std::invalid_argument("iAP2 link not established");
  Iap2SessionOutput output;
  const auto payload = encode_control_message(message);
  const auto frame = reliable_->send(10, payload, now);
  if (!frame) throw std::runtime_error("iAP2 send window is full");
  emit(output, *frame);
  return output;
}

Iap2SessionOutput Iap2SessionEngine::tick(Clock::time_point now) {
  require_live();
  Iap2SessionOutput output;
  if (!wifi_active_) {
    for (const auto& frame : handshake_.tick(now))
      emit(output, frame, true);
    require_live();
  }
  if (reliable_) {
    for (const auto& frame : reliable_->tick(now))
      emit(output, frame);
    require_live();
    flush_file_replies(output, now);
    // Optional metadata must never block authentication or consume the last
    // send-window slot. A small bounded retry covers phone route restoration.
    if (navigation_ && identified_ && navigation_updates_ == 0 && navigation_attempts_ < 3 &&
        (!mfi_auth_ || mfi_auth_->state() == Iap2MfiAuth::State::Complete) &&
        reliable_->unacknowledged() < 4 &&
        (!navigation_request_at_ || now - *navigation_request_at_ >= std::chrono::seconds(5))) {
      const auto frame = reliable_->send(
          10, encode_control_message(start_route_guidance(*identity_.route_guidance)), now);
      if (frame) {
        emit(output, *frame);
        navigation_request_at_ = now;
        ++navigation_attempts_;
      }
    }
    if (now_playing_ && identified_ && now_playing_updates_ == 0 && now_playing_attempts_ < 3 &&
        (!mfi_auth_ || mfi_auth_->state() == Iap2MfiAuth::State::Complete) &&
        reliable_->unacknowledged() < 4 &&
        (!now_playing_request_at_ || now - *now_playing_request_at_ >= std::chrono::seconds(5))) {
      const auto frame =
          reliable_->send(10, encode_control_message(Iap2NowPlaying::subscribe()), now);
      if (frame) {
        emit(output, *frame);
        now_playing_request_at_ = now;
        ++now_playing_attempts_;
      }
    }
  }
  return output;
}

}  // namespace headunit
