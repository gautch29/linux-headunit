// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_now_playing.hpp"
#include "headunit/iap2_file_transfer.hpp"
#include "headunit/iap2_parameter.hpp"

#include <array>
#include <stdexcept>
#include <source_location>
#include <vector>
#include <iostream>
#include <string_view>
#include <time.h>

namespace {
void check(bool value, const std::source_location location = std::source_location::current()) {
  if (!value)
    throw std::runtime_error("iAP2 now-playing test failed at " + std::to_string(location.line()));
}
headunit::Iap2Parameter string(std::uint16_t id, const char* value) {
  std::vector<std::uint8_t> bytes;
  for (const char* at = value; *at; ++at)
    bytes.push_back(*at);
  bytes.push_back(0);
  return {id, std::move(bytes)};
}
headunit::ControlMessage update(const std::vector<headunit::Iap2Parameter>& item,
                                const std::vector<headunit::Iap2Parameter>& playback = {}) {
  std::vector<headunit::Iap2Parameter> top;
  if (!item.empty()) top.push_back({0, headunit::encode_iap2_parameters(item)});
  if (!playback.empty()) top.push_back({1, headunit::encode_iap2_parameters(playback)});
  return {0x5001, headunit::encode_iap2_parameters(top)};
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--publication-fixture") {
    // Actual producer admission/encoder, synthetic content, no phone inputs.
    headunit::Iap2NowPlaying producer;
    const headunit::NowPlayingPublicationScope scope{"0123456789abcdef0123456789abcdef", 17, 9};
    check(producer.receive(
        update({{0, {0, 0, 0, 0, 0, 0, 0, 7}}, string(1, "Fixture")}, {{0, {2}}})));
    std::cout << headunit::now_playing_json(producer.snapshot(), "", scope);
    check(producer.receive(update({}, {{1, {0, 0, 0, 10}}})));
    std::cout << headunit::now_playing_json(producer.snapshot(), "", scope);
    std::cout << headunit::now_playing_json(producer.snapshot(), "", scope);
    producer.clear();
    std::cout << headunit::now_playing_json(producer.snapshot(), "", scope);
    return 0;
  }
  headunit::Iap2NowPlaying media;
  const auto request = media.subscribe();
  check(request.identifier == 0x5000);
  const auto fields = headunit::decode_iap2_parameters(request.parameters);
  check(fields.size() == 2 && fields[0].identifier == 0 && fields[1].identifier == 1);
  check(!media.snapshot().available);
  check(!media.snapshot().observation.message_accepted_ns);
  check(media.receive(update({{0, {0, 0, 0, 0, 0, 0, 0, 7}},
                              string(1, "Track A"),
                              string(12, "Artist A"),
                              {4, {0, 1, 0x86, 0xa0}}},
                             {{0, {1}}, {1, {0, 0, 0x13, 0x88}}, string(7, "Music")})));
  auto state = media.snapshot();
  check(state.available && state.title == "Track A" && state.artist == "Artist A" &&
        state.status == "playing" && state.player == "Music" && state.duration_ms == 100000 &&
        state.position_ms == 5000);
  const auto first_observation = state.observation;
  const auto field = [](const auto& snapshot, headunit::NowPlayingField key) {
    return snapshot.observation.fields[static_cast<std::size_t>(key)];
  };
  using Field = headunit::NowPlayingField;
  check(first_observation.message_accepted_ns.has_value());
  check(field(state, Field::title) == first_observation.message_accepted_ns);
  check(field(state, Field::artist) == field(state, Field::title));
  check(!field(state, Field::album) && !field(state, Field::artwork));
  check(media.receive(update({}, {{0, {2}}})));
  check(media.snapshot().title == "Track A" && media.snapshot().status == "paused");
  check(field(media.snapshot(), Field::title) == field(state, Field::title));
  check(field(media.snapshot(), Field::artist) == field(state, Field::artist));
  check(field(media.snapshot(), Field::status) == media.snapshot().observation.message_accepted_ns);
  const auto before_bad = media.snapshot().observation;
  check(!media.receive(update({{1, {0xff}}})));
  check(media.snapshot().title == "Track A");
  check(media.snapshot().observation == before_bad);
  // Unknown fields and unconsumed known playback16 are valid protocol input;
  // neither grants a new date to any retained exposed field.
  check(media.receive(update({{99, {1}}}, {string(16, "Ignored")})));
  check(media.snapshot().observation.fields == before_bad.fields);
  check(media.receive(update({{0, {0, 0, 0, 0, 0, 0, 0, 8}}})));
  state = media.snapshot();
  check(state.track_id == 8 && state.title.empty() && state.artist.empty() && !state.duration_ms &&
        !state.position_ms);
  check(!field(state, Field::title) && !field(state, Field::artist) &&
        !field(state, Field::duration_ms) && !field(state, Field::position_ms));
  check(field(state, Field::track_id) == state.observation.message_accepted_ns);
  check(field(state, Field::status) == before_bad.fields[static_cast<std::size_t>(Field::status)]);
  check(media.receive(update({{26, {7}}})));
  headunit::Iap2FileTransfer files;
  const auto setup = files.receive({std::array<std::uint8_t, 2>{7, 4}});
  check(setup.reply == std::vector<std::uint8_t>({7, 1}));
  constexpr std::array<std::uint8_t, 16> picture{7,  0xc0, 0x89, 'P', 'N', 'G', 13, 10,
                                                 26, 10,   0,    0,   0,   0,   0,  0};
  auto completed = files.receive(picture);
  check(completed.reply == std::vector<std::uint8_t>({7, 5}) && completed.completed &&
        completed.completed->first == 7);
  // Truncated image data is not accepted as a cover by the metadata model.
  check(!media.accept_artwork(7, completed.completed->second));
  std::array<std::uint8_t, 24> png{0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13,
                                   'I',  'H', 'D', 'R', 0,  0,  0,  1,  0, 0, 0, 1};
  check(media.accept_artwork(7, png));
  check(media.snapshot().artwork &&
        *media.snapshot().artwork == std::vector<std::uint8_t>(png.begin(), png.end()));
  const auto art_date = field(media.snapshot(), Field::artwork);
  check(art_date.has_value());
  check(!media.accept_artwork(8, png));
  check(field(media.snapshot(), Field::artwork) == art_date);
  const auto complete_observation = media.snapshot().observation;
  // Snapshot reads and the real publisher encoder never sample observation clocks.
  const headunit::NowPlayingPublicationScope scope{"0123456789abcdef0123456789abcdef", 17, 9};
  const auto encoded = headunit::now_playing_json(media.snapshot(), "", scope);
  check(headunit::now_playing_json(media.snapshot(), "", scope) == encoded);
  check(media.snapshot().observation == complete_observation);
  check(headunit::now_playing_json(media.snapshot(), "", {scope.launch_nonce, 0, 9}) ==
        headunit::now_playing_json(media.snapshot(), ""));
  check(media.receive(update({{26, {8}}})));
  check(!field(media.snapshot(), Field::artwork));
  check(!media.accept_artwork(7, png));
  check(!field(media.snapshot(), Field::artwork));
  check(files.receive({std::array<std::uint8_t, 2>{8, 0x40}}).reply.empty());
  media.clear();
  check(!media.snapshot().available);
  check(!media.accept_artwork(7, png));
  check(media.snapshot().observation == headunit::NowPlayingObservation{});
  // Playback and item are independent groups in a partial update. A new
  // item's fresh position must survive either wire ordering.
  for (const bool playback_first : {false, true}) {
    check(media.receive(
        update({{0, {0, 0, 0, 0, 0, 0, 0, 1}}, string(1, "Before")}, {{1, {0, 0, 0, 10}}})));
    std::vector<headunit::Iap2Parameter> top{
        {0, headunit::encode_iap2_parameters(std::vector<headunit::Iap2Parameter>{
                {0, {0, 0, 0, 0, 0, 0, 0, 2}}, string(1, "After"), {26, {7}}})},
        {1, headunit::encode_iap2_parameters(
                std::vector<headunit::Iap2Parameter>{{1, {0, 0, 0, 20}}, {0, {1}}})}};
    if (playback_first) std::swap(top[0], top[1]);
    check(media.receive({0x5001, headunit::encode_iap2_parameters(top)}));
    check(media.snapshot().track_id == 2 && media.snapshot().title == "After" &&
          media.snapshot().position_ms == 20 && media.snapshot().status == "playing");
    media.clear();
  }
  headunit::NowPlayingSnapshot near_limit;
  near_limit.available = true;
  near_limit.observation = complete_observation;
  near_limit.title.assign(2048, 'x');
  near_limit.artist.assign(2048, 'x');
  near_limit.album.assign(2048, 'x');
  near_limit.genre.assign(1800, 'x');
  const auto legacy = headunit::now_playing_json(near_limit, "");
  check(legacy.size() <= 8192);
  check(headunit::now_playing_json(near_limit, "", scope) == legacy);
  // An oversized continuation retires prior bytes rather than allowing a
  // later final fragment to acknowledge an incomplete file.
  check(files.receive(std::array<std::uint8_t, 3>{7, 0x80, 1}).reply.empty());
  std::vector<std::uint8_t> oversized(1024 * 1024 + 3, 1);
  oversized[0] = 7;
  oversized[1] = 0;
  check(!files.receive(oversized).completed);
  const auto invalid_final = files.receive(std::array<std::uint8_t, 3>{7, 0x40, 2});
  check(!invalid_final.completed && invalid_final.reply.empty());
  // Another transfer remains independent, and explicit retirement forgets
  // every partial assembly without manufacturing a completion receipt.
  files.receive(std::array<std::uint8_t, 3>{8, 0x80, 1});
  files.receive(std::array<std::uint8_t, 3>{9, 0x80, 2});
  files.receive(oversized);
  const auto independent = files.receive(std::array<std::uint8_t, 3>{8, 0x40, 3});
  check(independent.completed &&
        independent.completed->second == std::vector<std::uint8_t>({1, 3}));
  files.clear();
  check(!files.receive(std::array<std::uint8_t, 3>{9, 0x40, 3}).completed);
}
