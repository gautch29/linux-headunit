// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/iap2_message.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace headunit {

// Original HOST CLOCK_MONOTONIC acceptance dates, not phone acquisition or
// playback receipts. No metadata TTL is implied by this copied observation.
enum class NowPlayingField : std::size_t {
  track_id,
  title,
  artist,
  album,
  genre,
  duration_ms,
  position_ms,
  status,
  player,
  artwork_transfer_id,
  artwork,
  count
};
struct NowPlayingObservation {
  std::optional<std::uint64_t> message_accepted_ns;
  std::array<std::optional<std::uint64_t>, static_cast<std::size_t>(NowPlayingField::count)>
      fields{};
  bool operator==(const NowPlayingObservation&) const = default;
};
struct NowPlayingPublicationScope {
  std::string_view launch_nonce;
  std::uint64_t engine_generation = 0;
  std::uint64_t connection_generation = 0;
};

struct NowPlayingSnapshot {
  bool available = false;
  NowPlayingObservation observation;
  std::string title, artist, album, genre, player;
  std::optional<std::uint64_t> track_id;
  std::optional<std::uint32_t> duration_ms, position_ms;
  std::optional<std::uint8_t> artwork_transfer_id;
  std::shared_ptr<const std::vector<std::uint8_t>> artwork;
  std::string status = "unknown";
};

// Bounded encoding for the existing private sidecar publication. Identity is
// supplied by the actual publisher; no track identity is inferred from text.
std::string now_playing_json(const NowPlayingSnapshot& state, std::string_view artwork_hash,
                             NowPlayingPublicationScope scope = {});

// iAP2 now-playing updates are partial. Keep a complete, session-owned snapshot
// and reject malformed messages without changing the last good state.
class Iap2NowPlaying {
 public:
  static ControlMessage subscribe();
  bool receive(const ControlMessage& message);
  bool accept_artwork(std::uint8_t transfer_id, std::span<const std::uint8_t> bytes);
  NowPlayingSnapshot snapshot() const;
  void clear();

 private:
  mutable std::mutex mutex_;
  NowPlayingSnapshot state_;
};

}  // namespace headunit
