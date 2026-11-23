// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/mfi_coprocessor.hpp"
#include "headunit/iap2_session_engine.hpp"
#include "headunit/iap2_parameter.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <source_location>
#include <vector>

namespace {
void check(bool condition, const std::source_location location = std::source_location::current()) {
  if (!condition)
    throw std::runtime_error("iAP2 session engine test failed at " +
                             std::to_string(location.line()));
}

headunit::LinkFrame decode(const std::vector<std::uint8_t>& bytes) {
  headunit::LinkFrameReader reader;
  const auto frames = reader.feed(bytes);
  check(frames.size() == 1);
  return frames[0];
}
}  // namespace

int main() {
  using namespace std::chrono_literals;
  const auto now = headunit::Iap2SessionEngine::Clock::now();
  headunit::Iap2Identity identity;
  identity.bluetooth_address = {1, 2, 3, 4, 5, 6};
  headunit::Iap2SessionEngine session(99, identity);
  auto outbound = session.receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(), now);
  check(outbound.bluetooth.size() == 1 && decode(outbound.bluetooth[0]).control == 0x80);
  const headunit::LinkFrame syn_ack{0xc0, 135, 99, 0, {1}};
  outbound = session.receive_bluetooth(headunit::encode_link_frame(syn_ack), now + 1ms);
  check(session.established() && outbound.bluetooth.size() == 1 &&
        decode(outbound.bluetooth[0]).control == 0x40);

  const auto identify = headunit::encode_control_message({0x1d00, {}});
  const headunit::LinkFrame request{0x40, 136, 99, 10, identify};
  outbound = session.receive_bluetooth(headunit::encode_link_frame(request), now + 2ms);
  check(outbound.bluetooth.size() == 2 && outbound.wifi.empty());
  auto reply_frame = decode(outbound.bluetooth[1]);
  check(reply_frame.sequence == 100 && reply_frame.session == 10);
  headunit::ControlMessageReader message_reader;
  const auto replies = message_reader.feed(reply_frame.payload);
  check(replies.size() == 1 && replies[0].identifier == 0x1d01);

  const auto next = headunit::encode_control_message({0x4300, {1, 2}});
  const headunit::LinkFrame tunnel_frame{0x40, 137, 100, 10, next};
  const auto tunnel_bytes = headunit::encode_link_frame(tunnel_frame);
  outbound = session.receive_wifi({tunnel_bytes.data(), 4}, now + 3ms);
  check(outbound.wifi.empty());
  outbound = session.receive_wifi({tunnel_bytes.data() + 4, tunnel_bytes.size() - 4}, now + 4ms);
  check(session.wifi_active() && outbound.bluetooth.empty() && outbound.wifi.size() == 1 &&
        outbound.unhandled.size() == 1 && outbound.unhandled[0].identifier == 0x4300);
  check(outbound.unhandled[0].parameters.empty());
  check(decode(outbound.wifi[0]).acknowledgement == 137);
  outbound = session.send_control({0x4301, {3}}, now + 5ms);
  check(outbound.bluetooth.empty() && outbound.wifi.size() == 1);
  reply_frame = decode(outbound.wifi[0]);
  check(reply_frame.sequence == 101 && reply_frame.acknowledgement == 137);
  const auto retry = session.tick(now + 505ms);
  check(retry.wifi.size() == 1 && decode(retry.wifi[0]).sequence == 101);
  // The owner must retire a failed handshake/reliable link, not keep an
  // occupied "established" session that can revive on a late acknowledgement.
  auto rejects = [](const auto& action) {
    bool rejected = false;
    try {
      action();
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected);
  };
  headunit::Iap2SessionEngine missing_syn(99, identity);
  missing_syn.receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(), now);
  for (int attempt = 1; attempt < 5; ++attempt)
    check(missing_syn.tick(now + attempt * 500ms).bluetooth.size() == 1);
  rejects([&] { missing_syn.tick(now + 2500ms); });
  check(missing_syn.failed() && !missing_syn.established());
  rejects([&] { missing_syn.receive_bluetooth(headunit::encode_link_frame(syn_ack), now + 3s); });
  rejects([&] { missing_syn.receive_wifi(tunnel_bytes, now + 3s); });
  rejects([&] { missing_syn.send_control({0x4301, {3}}, now + 3s); });
  // The pending Wi-Fi send exhausts its own bound. Late data and ACKs may
  // never revive the prior engine, while an independent new engine is usable.
  for (int attempt = 2; attempt < 5; ++attempt)
    check(session.tick(now + 5ms + attempt * 500ms).wifi.size() == 1);
  rejects([&] { session.tick(now + 2505ms); });
  check(session.failed() && !session.established());
  rejects([&] {
    session.receive_wifi(headunit::encode_link_frame({0x40, 138, 101, 0, {}}), now + 3s);
  });
  rejects([&] { session.tick(now + 4s); });
  struct UnusedBus final : headunit::MfiRegisterBus {
    void read(std::uint8_t, std::span<std::uint8_t>) override {
      throw std::runtime_error("Unexpected hardware access in failure fixture");
    }
    void write(std::uint8_t, std::span<const std::uint8_t>) override {
      throw std::runtime_error("Unexpected hardware access in failure fixture");
    }
  } bus;
  headunit::MfiCoprocessor chip(bus);
  headunit::Iap2SessionEngine authentication_failure(99, identity, &chip);
  authentication_failure.receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(), now);
  authentication_failure.receive_bluetooth(headunit::encode_link_frame(syn_ack), now + 1ms);
  rejects([&] {
    authentication_failure.receive_bluetooth(
        headunit::encode_link_frame(
            {0x40, 136, 99, 10, headunit::encode_control_message({0xaa04, {}})}),
        now + 2ms);
  });
  check(authentication_failure.failed() && !authentication_failure.established());
  rejects([&] { authentication_failure.tick(now + 3ms); });
  headunit::Iap2SessionEngine replacement(99, identity);
  replacement.receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(), now + 3s);
  replacement.receive_bluetooth(headunit::encode_link_frame(syn_ack), now + 3001ms);
  check(replacement.established() && !replacement.failed());
  // Explicit transport retirement also reaches retained IP consumers. Neither
  // a late SYN, data/ACK, send nor timer can restart this generation.
  replacement.receive_wifi(headunit::encode_link_frame({0x40, 136, 99, 0, {}}), now + 3002ms);
  check(replacement.wifi_active() && replacement.established());
  replacement.retire();
  replacement.retire();  // Idempotent; no new protocol state or retry owner.
  check(replacement.failed() && !replacement.established());
  rejects([&] { replacement.receive_wifi(tunnel_bytes, now + 4s); });
  rejects([&] { replacement.receive_bluetooth(headunit::encode_link_frame(syn_ack), now + 4s); });
  rejects([&] { replacement.send_control({0x4301, {3}}, now + 4s); });
  rejects([&] { replacement.tick(now + 4s); });
  // An optional metadata subscription waits for identification,
  // then traverses the same reliable link as every other iAP control message.
  identity.route_guidance = headunit::RouteGuidanceConfig{};
  headunit::Iap2SessionEngine navigation(99, identity);
  navigation.receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(), now);
  navigation.receive_bluetooth(headunit::encode_link_frame(syn_ack), now + 1ms);
  navigation.receive_bluetooth(headunit::encode_link_frame(request), now + 2ms);
  check(navigation.tick(now + 4ms).bluetooth.empty());
  navigation.receive_bluetooth(
      headunit::encode_link_frame(
          {0x40, 137, 100, 10, headunit::encode_control_message({0x1d02, {}})}),
      now + 5ms);
  auto out = navigation.tick(now + 6ms);
  check(out.bluetooth.size() == 1 && out.wifi.empty());
  headunit::ControlMessageReader nav_reader;
  check(nav_reader.feed(decode(out.bluetooth[0]).payload)[0].identifier == 0x5200);
  const std::array<headunit::Iap2Parameter, 3> route_fields{
      {{0, {0, 42}}, {1, {1}}, {10, {0, 0, 1, 44}}}};
  const auto route_payload =
      headunit::encode_control_message({0x5201, headunit::encode_iap2_parameters(route_fields)});
  navigation.receive_wifi(headunit::encode_link_frame({0x40, 138, 101, 10, route_payload}),
                          now + 7ms);
  check(navigation.navigation_updates() == 1 &&
        navigation.navigation()->snapshot(now + 7ms).distance_m == 300);
  check(navigation.tick(now + 6s).wifi.empty());
  const auto bad_route = headunit::encode_control_message({0x5201, {1}});
  navigation.receive_wifi(headunit::encode_link_frame({0x40, 139, 101, 10, bad_route}), now + 7s);
  check(navigation.navigation_rejections() == 1 && navigation.established());
  identity.route_guidance.reset();
  identity.supports_now_playing = true;
  headunit::Iap2SessionEngine media(99, identity, nullptr, std::nullopt, std::nullopt, 17);
  media.receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(), now);
  media.receive_bluetooth(headunit::encode_link_frame(syn_ack), now + 1ms);
  media.receive_bluetooth(headunit::encode_link_frame(request), now + 2ms);
  media.receive_bluetooth(headunit::encode_link_frame(
                              {0x40, 137, 100, 10, headunit::encode_control_message({0x1d02, {}})}),
                          now + 3ms);
  auto media_out = media.tick(now + 4ms);
  check(media_out.bluetooth.size() == 1 &&
        nav_reader.feed(decode(media_out.bluetooth[0]).payload)[0].identifier == 0x5000);
  const std::array<headunit::Iap2Parameter, 2> track_fields{
      {{1, {'S', 'o', 'n', 'g', 0}}, {26, {7}}}};
  const std::array<headunit::Iap2Parameter, 1> now_fields{
      {{0, headunit::encode_iap2_parameters(track_fields)}}};
  media.receive_wifi(
      headunit::encode_link_frame({0x40, 138, 101, 10,
                                   headunit::encode_control_message(
                                       {0x5001, headunit::encode_iap2_parameters(now_fields)})}),
      now + 5ms);
  check(media.now_playing_updates() == 1 && media.now_playing()->snapshot().title == "Song");
  const auto accepted_metadata = media.now_playing()->snapshot().observation;
  check(accepted_metadata.message_accepted_ns.has_value());
  // Reliable-link replay has no new producer admission or field dates.
  media.receive_wifi(
      headunit::encode_link_frame({0x40, 138, 101, 10,
                                   headunit::encode_control_message(
                                       {0x5001, headunit::encode_iap2_parameters(now_fields)})}),
      now + 5500us);
  check(media.now_playing()->snapshot().observation == accepted_metadata);
  const std::array<std::uint8_t, 26> image_transfer{7,  0xc0, 0x89, 'P', 'N', 'G', 13,  10,  26,
                                                    10, 0,    0,    0,   13,  'I', 'H', 'D', 'R',
                                                    0,  0,    0,    1,   0,   0,   0,   1};
  auto image_output =
      media.receive_wifi(headunit::encode_link_frame(
                             {0x40, 139, 101, 12, {image_transfer.begin(), image_transfer.end()}}),
                         now + 6ms);
  check(media.now_playing()->snapshot().artwork &&
        media.now_playing()->snapshot().artwork->size() == 24);
  bool acknowledged_artwork = false;
  for (const auto& bytes : image_output.wifi) {
    const auto frame = decode(bytes);
    if (frame.session == 12 && frame.payload == std::vector<std::uint8_t>({7, 5}))
      acknowledged_artwork = true;
  }
  check(acknowledged_artwork);
  const auto accepted_art = media.now_playing()->snapshot().observation;
  check(accepted_art.fields[static_cast<std::size_t>(headunit::NowPlayingField::artwork)]
            .has_value());
  check(accepted_art.message_accepted_ns == accepted_metadata.message_accepted_ns);
  // A replacement track may reuse the same eight-bit file ID. Its old
  // assembly is retired at the valid item update, before a tail can arrive.
  std::vector<std::uint8_t> first{7, 0x80};
  first.insert(first.end(), image_transfer.begin() + 2, image_transfer.begin() + 14);
  media.receive_wifi(headunit::encode_link_frame({0x40, 140, 102, 12, first}), now + 7ms);
  const std::array<headunit::Iap2Parameter, 2> replacement_item{
      {{1, {'N', 'e', 'w', 0}}, {26, {7}}}};
  const std::array<headunit::Iap2Parameter, 1> replacement_top{
      {{0, headunit::encode_iap2_parameters(replacement_item)}}};
  media.receive_wifi(headunit::encode_link_frame(
                         {0x40, 141, 102, 10,
                          headunit::encode_control_message(
                              {0x5001, headunit::encode_iap2_parameters(replacement_top)})}),
                     now + 8ms);
  std::vector<std::uint8_t> tail{7, 0x40};
  tail.insert(tail.end(), image_transfer.begin() + 14, image_transfer.end());
  const auto retired_output =
      media.receive_wifi(headunit::encode_link_frame({0x40, 142, 102, 12, tail}), now + 9ms);
  check(media.now_playing()->snapshot().title == "New" && !media.now_playing()->snapshot().artwork);
  check(!media.now_playing()
             ->snapshot()
             .observation.fields[static_cast<std::size_t>(headunit::NowPlayingField::artwork)]);
  for (const auto& bytes : retired_output.wifi)
    check(decode(bytes).session != 12);
  // A fresh complete transfer for that selected item remains usable.
  media.receive_wifi(headunit::encode_link_frame(
                         {0x40, 143, 102, 12, {image_transfer.begin(), image_transfer.end()}}),
                     now + 10ms);
  check(bool(media.now_playing()->snapshot().artwork));
  // Malformed metadata is transactional: it must not retire a still-valid
  // assembly, clear the previous item or accept the proposed replacement.
  const auto previous_art = media.now_playing()->snapshot().artwork;
  const auto previous_observation = media.now_playing()->snapshot().observation;
  media.receive_wifi(headunit::encode_link_frame({0x40, 144, 103, 12, first}), now + 11ms);
  const std::array<headunit::Iap2Parameter, 2> malformed_item{
      {{1, {'B', 'a', 'd', 0}}, {12, {0xff}}}};
  const std::array<headunit::Iap2Parameter, 1> malformed_top{
      {{0, headunit::encode_iap2_parameters(malformed_item)}}};
  media.receive_wifi(
      headunit::encode_link_frame({0x40, 145, 103, 10,
                                   headunit::encode_control_message(
                                       {0x5001, headunit::encode_iap2_parameters(malformed_top)})}),
      now + 12ms);
  check(media.now_playing_rejections() == 1 && media.now_playing()->snapshot().title == "New" &&
        media.now_playing()->snapshot().artwork == previous_art);
  check(media.now_playing()->snapshot().observation == previous_observation);
  media.receive_wifi(headunit::encode_link_frame({0x40, 146, 103, 12, tail}), now + 13ms);
  check(media.now_playing()->snapshot().artwork &&
        media.now_playing()->snapshot().artwork != previous_art);
  {
    headunit::Iap2SessionEngine observed(99, identity);
    observed.receive_bluetooth(headunit::Iap2SessionEngine::sync_preamble(), now);
    observed.receive_bluetooth(headunit::encode_link_frame(syn_ack), now + 1ms);
    std::vector<std::uint8_t> payload;
    for (unsigned id = 0; id < 70; ++id) {
      const auto bytes =
          headunit::encode_control_message({std::uint16_t(0x6000 + id), {0xde, 0xad, 0xbe, 0xef}});
      payload.insert(payload.end(), bytes.begin(), bytes.end());
    }
    const auto batch = headunit::encode_link_frame({0x40, 136, 99, 10, payload});
    const auto notices = observed.receive_wifi(batch, now + 2ms);
    check(notices.unhandled.size() == 64 && notices.wifi.size() == 1 &&
          observed.coverage().notice_drops == 6 && observed.coverage().unknown_evictions == 38);
    for (const auto& notice : notices.unhandled)
      check(notice.parameters.empty());
    // The reliability ACK covers transport receipt, not an unknown command reply.
    check(decode(notices.wifi.front()).session == 0 && observed.established());
    const auto before_duplicate = observed.coverage();
    observed.receive_wifi(batch, now + 3ms);
    check(observed.coverage() == before_duplicate);
    observed.receive_wifi(headunit::encode_link_frame({0x40, 137, 99, 200, {1, 2}}), now + 4ms);
    check(observed.coverage().delivered_sessions[200] == 1 &&
          observed.coverage().delivered_sessions[10] == 1);
    const auto accepted = observed.receive_wifi(
        headunit::encode_link_frame(
            {0x40, 138, 99, 10, headunit::encode_control_message({0x1d00, {}})}),
        now + 5ms);
    check(accepted.wifi.size() == 2 && accepted.unhandled.empty());
    const auto& known = observed.coverage().known_controls.front();
    check(known.identifier == 0x1d00 && known.received == 1 && known.dispatched == 1 &&
          !known.unhandled && !known.rejected);
    const auto before_retirement = observed.coverage();
    observed.retire();
    rejects([&] { observed.receive_wifi(batch, now + 6ms); });
    check(observed.coverage() == before_retirement);
    check(media.coverage().known_controls[4].rejected == 1);
  }
  const auto retired_observation = media.now_playing()->snapshot().observation;
  check(media.now_playing_snapshot().observation == retired_observation);
  media.retire();
  rejects([&] { media.receive_wifi(tunnel_bytes, now + 20ms); });
  check(media.now_playing()->snapshot().observation == retired_observation);
  const auto retired_copy = media.now_playing_snapshot();
  check(retired_copy.available && retired_copy.title == "New" && retired_copy.artwork);
  check(retired_copy.observation == headunit::NowPlayingObservation{});
  const auto retired_json = headunit::now_playing_json(
      retired_copy, "", {"0123456789abcdef0123456789abcdef", media.diagnostic_generation(), 9});
  check(retired_json.find("iap_observation") == std::string::npos);
  // Engine retirement cannot manufacture a new date; a successor starts unaged.
  headunit::Iap2SessionEngine next_media(99, identity, nullptr, std::nullopt, std::nullopt, 18);
  check(next_media.diagnostic_generation() != media.diagnostic_generation());
  check(next_media.now_playing()->snapshot().observation == headunit::NowPlayingObservation{});
  return 0;
}
