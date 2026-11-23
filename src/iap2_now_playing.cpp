// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_now_playing.hpp"

#include "headunit/iap2_parameter.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>
#include <time.h>

namespace headunit {
namespace {

std::optional<std::uint64_t> accepted_time_ns() noexcept {
  timespec value{};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0) return std::nullopt;
  return std::uint64_t(value.tv_sec) * 1000000000 + std::uint64_t(value.tv_nsec);
}

std::string json_text(std::string_view value) {
  std::string result = "\"";
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') result.push_back('\\');
    if (byte >= 32) result.push_back(static_cast<char>(byte));
  }
  result.push_back('"');
  return result;
}

constexpr auto index(NowPlayingField field) {
  return static_cast<std::size_t>(field);
}

void retire_item_dates(NowPlayingObservation& observation) {
  for (const auto field : {NowPlayingField::artist, NowPlayingField::album, NowPlayingField::genre,
                           NowPlayingField::duration_ms, NowPlayingField::position_ms,
                           NowPlayingField::artwork_transfer_id, NowPlayingField::artwork})
    observation.fields[index(field)].reset();
}

std::uint64_t unsigned_number(const Iap2Parameter& field, std::size_t width) {
  if (field.value.size() != width) throw std::invalid_argument("now-playing number width");
  std::uint64_t result = 0;
  for (const auto byte : field.value)
    result = (result << 8) | byte;
  return result;
}

std::string text(const Iap2Parameter& field) {
  auto bytes = std::span(field.value);
  if (!bytes.empty() && bytes.back() == 0) bytes = bytes.first(bytes.size() - 1);
  if (bytes.size() > 2048) throw std::invalid_argument("now-playing text too long");
  std::string result;
  result.reserve(bytes.size());
  for (std::size_t i = 0; i < bytes.size();) {
    const auto first = bytes[i++];
    if (first == 0) throw std::invalid_argument("NUL in now-playing text");
    if (first < 0x80) {
      result.push_back(first < 32 || first == 127 ? ' ' : char(first));
      continue;
    }
    const unsigned remaining = first >= 0xc2 && first <= 0xdf   ? 1
                               : first >= 0xe0 && first <= 0xef ? 2
                               : first >= 0xf0 && first <= 0xf4 ? 3
                                                                : 0;
    if (!remaining || i + remaining > bytes.size())
      throw std::invalid_argument("invalid now-playing UTF-8");
    std::uint32_t scalar = first & ((1u << (6 - remaining)) - 1);
    for (unsigned j = 0; j < remaining; ++j) {
      const auto continuation = bytes[i++];
      if ((continuation & 0xc0) != 0x80) throw std::invalid_argument("invalid now-playing UTF-8");
      scalar = (scalar << 6) | (continuation & 0x3f);
    }
    if ((remaining == 2 && scalar < 0x800) || (remaining == 3 && scalar < 0x10000) ||
        scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
      throw std::invalid_argument("invalid now-playing UTF-8");
    result.append(reinterpret_cast<const char*>(bytes.data() + i - remaining - 1), remaining + 1);
  }
  return result;
}

std::vector<Iap2Parameter> group(const Iap2Parameter& field) {
  auto result = decode_iap2_parameters(field.value);
  if (result.size() > 32) throw std::invalid_argument("too many now-playing fields");
  std::set<std::uint16_t> seen;
  for (const auto& nested : result)
    if (!seen.insert(nested.identifier).second)
      throw std::invalid_argument("duplicate now-playing field");
  return result;
}

}  // namespace

std::string now_playing_json(const headunit::NowPlayingSnapshot& state,
                             std::string_view artwork_hash, NowPlayingPublicationScope scope) {
  if (!state.available) return "{\"available\":false}\n";
  auto value = std::string{"{\"available\":true,\"controls\":[],\"title\":"} +
               json_text(state.title) + ",\"artist\":" + json_text(state.artist) +
               ",\"album\":" + json_text(state.album) + ",\"genre\":" + json_text(state.genre) +
               ",\"player\":" + json_text(state.player) + ",\"status\":" + json_text(state.status);
  if (state.track_id) value += ",\"track_id\":" + json_text(std::to_string(*state.track_id));
  if (state.duration_ms) value += ",\"duration_ms\":" + std::to_string(*state.duration_ms);
  if (state.position_ms) value += ",\"position_ms\":" + std::to_string(*state.position_ms);
  if (!artwork_hash.empty()) value += ",\"iap_artwork_sha256\":" + json_text(artwork_hash);
  const auto metadata = value;
  const bool observed =
      state.observation.message_accepted_ns.has_value() ||
      std::any_of(state.observation.fields.begin(), state.observation.fields.end(),
                  [](const auto& stamp) { return stamp.has_value(); });
  if (observed && scope.launch_nonce.size() == 32 && scope.engine_generation &&
      scope.connection_generation) {
    value += ",\"iap_observation\":{\"version\":1,\"clock\":\"CLOCK_MONOTONIC\",\"launchNonce\":" +
             json_text(scope.launch_nonce) +
             ",\"engineGeneration\":" + json_text(std::to_string(scope.engine_generation)) +
             ",\"connectionGeneration\":" + json_text(std::to_string(scope.connection_generation));
    const auto date = [](const auto& stamp) {
      return stamp ? json_text(std::to_string(*stamp)) : "null";
    };
    value += ",\"messageAcceptedMonotonicNs\":" + date(state.observation.message_accepted_ns);
    value += ",\"artworkTransferId\":" +
             (state.artwork_transfer_id ? std::to_string(*state.artwork_transfer_id) : "null");
    value += ",\"fields\":{";
    constexpr std::array<std::string_view, index(NowPlayingField::count)> names{
        "track_id",    "title",       "artist", "album",  "genre",
        "duration_ms", "position_ms", "status", "player", "artwork_transfer_id",
        "artwork"};
    for (std::size_t field = 0; field < names.size(); ++field) {
      if (field) value += ',';
      value += json_text(names[field]) + ':' + date(state.observation.fields[field]);
    }
    value += "}}";
  }
  if (value.size() + 2 > 8192) return metadata + "}\n";
  return value + "}\n";
}

ControlMessage Iap2NowPlaying::subscribe() {
  constexpr std::array<std::uint16_t, 7> item_fields{0, 1, 4, 6, 12, 16, 26};
  constexpr std::array<std::uint16_t, 3> playback_fields{0, 1, 7};
  std::vector<Iap2Parameter> item, playback;
  for (auto id : item_fields)
    item.push_back({id, {}});
  for (auto id : playback_fields)
    playback.push_back({id, {}});
  const std::array<Iap2Parameter, 2> top{{
      {0, encode_iap2_parameters(item)},
      {1, encode_iap2_parameters(playback)},
  }};
  return {0x5000, encode_iap2_parameters(top)};
}

bool Iap2NowPlaying::receive(const ControlMessage& message) {
  if (message.identifier != 0x5001 || message.parameters.size() > 16384) return false;
  try {
    auto top = decode_iap2_parameters(message.parameters);
    if (top.empty() || top.size() > 2) return false;
    std::set<std::uint16_t> seen;
    for (const auto& field : top)
      if (field.identifier > 1 || !seen.insert(field.identifier).second) return false;
    // A track transition clears the previous position. Apply its item group
    // before this message's playback position regardless of wire field order.
    std::sort(top.begin(), top.end(), [](const auto& left, const auto& right) {
      return left.identifier < right.identifier;
    });
    std::lock_guard lock(mutex_);
    auto next = state_;
    std::array<bool, index(NowPlayingField::count)> admitted{};
    const auto observe = [&](NowPlayingField field) {
      admitted[index(field)] = true;
    };
    for (const auto& field : top) {
      const auto values = group(field);
      if (field.identifier == 0) {
        for (const auto& value : values) {
          if (value.identifier != 0) continue;
          const auto id = unsigned_number(value, 8);
          if (next.track_id && next.track_id != id) {
            next.title.clear();
            next.observation.fields[index(NowPlayingField::title)].reset();
            retire_item_dates(next.observation);
            next.artist.clear();
            next.album.clear();
            next.genre.clear();
            next.duration_ms.reset();
            next.position_ms.reset();
            next.artwork_transfer_id.reset();
            next.artwork.reset();
          }
          next.track_id = id;
          observe(NowPlayingField::track_id);
        }
        for (const auto& value : values) {
          if (value.identifier != 1) continue;
          const auto title = text(value);
          if (!next.title.empty() && next.title != title) {
            retire_item_dates(next.observation);
            next.artist.clear();
            next.album.clear();
            next.genre.clear();
            next.duration_ms.reset();
            next.position_ms.reset();
            next.artwork_transfer_id.reset();
            next.artwork.reset();
          }
          next.title = title;
          observe(NowPlayingField::title);
        }
      }
      for (const auto& value : values) {
        if (field.identifier == 0) {
          switch (value.identifier) {
            case 0:
              break;
            case 1:
              break;
            case 4:
              next.duration_ms = unsigned_number(value, 4);
              observe(NowPlayingField::duration_ms);
              break;
            case 6:
              next.album = text(value);
              observe(NowPlayingField::album);
              break;
            case 12:
              next.artist = text(value);
              observe(NowPlayingField::artist);
              break;
            case 16:
              next.genre = text(value);
              observe(NowPlayingField::genre);
              break;
            case 26: {
              const auto id = unsigned_number(value, 1);
              if (next.artwork_transfer_id != id) {
                next.artwork.reset();
                next.observation.fields[index(NowPlayingField::artwork)].reset();
              }
              next.artwork_transfer_id = id;
              observe(NowPlayingField::artwork_transfer_id);
              break;
            }
            default:
              break;
          }
        } else {
          switch (value.identifier) {
            case 0: {
              const auto status = unsigned_number(value, 1);
              if (status > 4) throw std::invalid_argument("invalid playback state");
              constexpr std::array<const char*, 5> states{"stopped", "playing", "paused",
                                                          "forward-seek", "reverse-seek"};
              next.status = states[status];
              observe(NowPlayingField::status);
              break;
            }
            case 1:
              next.position_ms = unsigned_number(value, 4);
              observe(NowPlayingField::position_ms);
              break;
            case 7:
              next.player = text(value);
              observe(NowPlayingField::player);
              break;
            case 16:
              (void)text(value);
              break;
            default:
              break;
          }
        }
      }
    }
    const auto accepted = accepted_time_ns();
    next.observation.message_accepted_ns = accepted;
    for (std::size_t field = 0; field < admitted.size(); ++field)
      if (admitted[field]) next.observation.fields[field] = accepted;
    next.available = true;
    state_ = std::move(next);
    return true;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

bool Iap2NowPlaying::accept_artwork(std::uint8_t transfer_id, std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 16 || bytes.size() > 1024 * 1024) return false;
  const bool png =
      bytes.size() >= 24 &&
      std::equal(bytes.begin(), bytes.begin() + 8,
                 std::array<std::uint8_t, 8>{0x89, 'P', 'N', 'G', 13, 10, 26, 10}.begin());
  const bool jpeg = bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[bytes.size() - 2] == 0xff &&
                    bytes.back() == 0xd9;
  if (!png && !jpeg) return false;
  std::lock_guard lock(mutex_);
  if (!state_.available || state_.artwork_transfer_id != transfer_id) return false;
  state_.artwork = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
  state_.observation.fields[index(NowPlayingField::artwork)] = accepted_time_ns();
  return true;
}

NowPlayingSnapshot Iap2NowPlaying::snapshot() const {
  std::lock_guard lock(mutex_);
  return state_;
}

void Iap2NowPlaying::clear() {
  std::lock_guard lock(mutex_);
  state_ = {};
}

}  // namespace headunit
