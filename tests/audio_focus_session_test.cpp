// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/airplay_session_setup.hpp>
#include <headunit/audio_playback.hpp>
#include "audio_pipeline_fixture.hpp"
#include <atomic>
#include <headunit/binary_plist.hpp>
#include <headunit/hap_crypto.hpp>
#include <array>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

using namespace headunit;
using Dictionary = PlistValue::Dictionary;
using Array = PlistValue::Array;
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
auto body(Dictionary value) {
  return encode_binary_plist(PlistValue{std::move(value)});
}
void event_request(AirplaySessionSetup& session, int fd, std::span<const std::uint8_t> secret,
                   std::span<const std::uint8_t> value, unsigned turn, const char* expected) {
  const auto write = hap_hkdf_sha512(secret, "Events-Salt", "Events-Read-Encryption-Key");
  const auto read = hap_hkdf_sha512(secret, "Events-Salt", "Events-Write-Encryption-Key");
  std::array<char, 8> nonce{};
  nonce[0] = char(turn);
  std::string plain = "POST /command RTSP/1.0\r\nCSeq: " + std::to_string(turn + 1) +
                      "\r\nContent-Length: " + std::to_string(value.size()) + "\r\n\r\n";
  plain.append(reinterpret_cast<const char*>(value.data()), value.size());
  check(plain.size() <= 1024, "test event exceeds one authenticated frame");
  std::array<std::uint8_t, 2> length{std::uint8_t(plain.size()), std::uint8_t(plain.size() >> 8)};
  auto sealed =
      hap_encrypt(write, {nonce.data(), nonce.size()},
                  {reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()}, length);
  std::vector<std::uint8_t> wire(length.begin(), length.end());
  wire.insert(wire.end(), sealed.begin(), sealed.end());
  check(send(fd, wire.data(), wire.size(), 0) == static_cast<ssize_t>(wire.size()),
        "event test send failed");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  pollfd waiting{fd, POLLIN, 0};
  do {
    session.drain();
    if (poll(&waiting, 1, 0) > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  check(waiting.revents & POLLIN, "event owner did not reply within bounded wait");
  check(recv(fd, length.data(), length.size(), MSG_WAITALL) == 2, "event reply header missing");
  sealed.resize(std::size_t(length[0]) + (std::size_t(length[1]) << 8) + 16);
  check(recv(fd, sealed.data(), sealed.size(), MSG_WAITALL) == static_cast<ssize_t>(sealed.size()),
        "event reply truncated");
  const auto opened = hap_decrypt(read, {nonce.data(), nonce.size()}, sealed, length);
  check(opened.has_value(), "event reply did not authenticate");
  const std::string reply(opened->begin(), opened->end());
  check(reply.find(expected) != std::string::npos &&
            reply.find("CSeq: " + std::to_string(turn + 1) + "\r\n") != std::string::npos,
        "event focus status/CSeq wrong");
}
template <class Predicate>
void wait_audio(Predicate ready, const char* why) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  check(ready(), why);
}
void queued_voice_retirement_preserves_focus_and_master() {
  // Real shared playback and Gst bus/pad; synthetic decoded PCM and producer
  // calls. This is neither phone TEARDOWN nor an audible/device receipt.
  audio_fixture::OutputBus sink;
  std::atomic<std::uint64_t> received{};
  std::atomic<unsigned> peak{};
  AudioPlayback playback("fakesink", "",
                         [&](const auto& receipt) {
                           if (receipt.peak_sample) peak.store(*receipt.peak_sample);
                           received.fetch_add(1);
                         },
                         {40, true});
  auto master = playback.audio_control();
  auto media = playback.attach(true);
  auto alternate = playback.attach(false);
  auto old_voice = playback.attach(false);
  AudioMixer::Block music{};
  AudioMixer::Block guidance{};
  AudioMixer::Block speech{};
  music.fill(5000);
  guidance.fill(2000);
  speech.fill(12000);
  playback.configure(media, true, AudioFocusCommand{.5, 0});
  sink.block_sink();
  struct Unblock {
    audio_fixture::OutputBus& sink;
    ~Unblock() { sink.unblock_sink(); }
  } unblock{sink};
  // Require physical appsrc saturation AND all three channel queues full.
  // The blocked pad prevents consumption between this observation and detach.
  wait_audio(
      [&] {
        const bool media_full = !playback.submit(media, music, true, {});
        const bool alternate_full = !playback.submit(alternate, guidance, true, {});
        const bool voice_full = !playback.submit(old_voice, speech, true, {});
        return media_full && alternate_full && voice_full &&
               sink.queued_input_bytes() == 8 * sizeof(AudioMixer::Block);
      },
      "shared retirement fixture did not retain pending PCM on all three channels");
  check(master->request({1, {40, true}}) == audio_control::RequestResult::accepted,
        "backpressured master request rejected");
  wait_audio([&] { return master->snapshot().applied.sequence == 1; },
             "backpressured master command did not apply");
  const auto blocked = playback.status();
  const auto original = master->snapshot();
  check(blocked.duck_gain == .5 && original.available && original.output &&
            original.applied == audio_control::Command{1, {40, true}},
        "shared focus/master configuration changed before retirement");
  playback.detach(old_voice);
  auto successor_voice = playback.attach(false);
  // Model a producer's late old-generation completion after a new slot exists.
  playback.configure(old_voice, true, AudioFocusCommand{0, 0});
  bool retired_rejected = false;
  try {
    (void)playback.submit(old_voice, speech, true, AudioFocusCommand{0, 0});
  } catch (const std::runtime_error& error) {
    retired_rejected = std::string(error.what()) == "audio channel retired";
  }
  check(retired_rejected && master == playback.audio_control() && master->snapshot().available &&
            master->snapshot().requested == original.requested &&
            master->snapshot().applied == original.applied &&
            playback.status().accepted == blocked.accepted && playback.status().duck_gain == .5,
        "late voice completion revived PCM or changed surviving focus/master state");
  sink.unblock_sink();
  wait_audio(
      [&] {
        const auto current = playback.status();
        return current.starved && received.load() == current.accepted &&
               sink.queued_input_bytes() == 0;
      },
      "pre-retirement sink PCM did not drain");
  // Each isolated post-drain block has an exact sink-pad peak under the existing
  // policy. Already accepted old mixed PCM may drain first; it is not revoked.
  const auto accepts_peak = [&](const auto& channel, const auto& block, unsigned expected) {
    const auto before = received.load();
    check(playback.submit(channel, block, true, {}), "live survivor PCM rejected");
    wait_audio([&] { return received.load() == before + 1; }, "live survivor PCM not observed");
    check(peak.load() == expected, "retired voice contaminated surviving sink PCM");
  };
  accepts_peak(successor_voice, speech, 0);
  check(master->request({2, {40, false}}) == audio_control::RequestResult::accepted,
        "surviving master unmute rejected");
  wait_audio([&] { return master->snapshot().applied.sequence == 2; },
             "surviving master unmute not applied");
  accepts_peak(media, music, 1000);
  accepts_peak(alternate, guidance, 800);
  accepts_peak(successor_voice, speech, 4800);
  const auto final_level = master->snapshot().applied;
  sink.error("combined focus retirement output fault");
  wait_audio([&] { return !master->snapshot().available; },
             "shared output fault retained controls");
  const auto retired = master->snapshot();
  check(!retired.output && retired.applied == final_level &&
            playback.status().fault == "combined focus retirement output fault" &&
            master->request({3, {100, false}}) == audio_control::RequestResult::unavailable,
        "terminal output changed history or retained live progress/control");
  playback.detach(media);
  playback.detach(alternate);
  playback.detach(successor_voice);
  AudioPlayback fresh("fakesink", "", {}, {73, false});
  auto fresh_master = fresh.audio_control();
  auto fresh_media = fresh.attach(true);
  check(fresh_master != master && fresh_master->snapshot().available &&
            fresh_master->snapshot().applied == audio_control::Command{0, {73, false}} &&
            fresh.status().accepted == 0 && fresh.status().duck_gain == 1.,
        "fresh shared output inherited retired focus/master/PCM");
  check(fresh_master->request({1, {25, true}}) == audio_control::RequestResult::accepted,
        "fresh master rejected its own sequence");
  playback.configure(old_voice, true, AudioFocusCommand{1, 0});
  bool old_ack_rejected = false;
  try {
    master->applied({1, {40, true}});
  } catch (const std::invalid_argument&) {
    old_ack_rejected = true;
  }
  wait_audio([&] { return fresh_master->snapshot().applied.sequence == 1; },
             "fresh master command consumed by old completion");
  check(old_ack_rejected && master->snapshot() == retired &&
            fresh_master->snapshot().applied == audio_control::Command{1, {25, true}} &&
            fresh.status().accepted == 0 && fresh.status().duck_gain == 1.,
        "old completion changed retired history or successor control/focus/progress");
  fresh.detach(fresh_media);
}

int main() try {
  queued_voice_retirement_preserves_focus_and_master();
  const int timing = socket(AF_INET6, SOCK_DGRAM, 0);
  check(timing >= 0, "timing socket failed");
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } close_timing{timing};
  sockaddr_in6 phone{};
  phone.sin6_family = AF_INET6;
  phone.sin6_addr = in6addr_loopback;
  check(bind(timing, reinterpret_cast<const sockaddr*>(&phone), sizeof(phone)) == 0,
        "timing bind failed");
  socklen_t phone_size = sizeof(phone);
  check(getsockname(timing, reinterpret_cast<sockaddr*>(&phone), &phone_size) == 0,
        "timing address failed");
  std::array<std::uint8_t, 32> secret{};
  const auto duck = body({{"type", PlistValue{std::string("duckAudio")}}});
  const auto setup = body({{"timingPort", PlistValue{std::int64_t(ntohs(phone.sin6_port))}}});
  const auto stream = PlistValue{Dictionary{{"type", PlistValue{std::int64_t(102)}},
                                            {"streamConnectionID", PlistValue{std::int64_t(91)}},
                                            {"audioFormat", PlistValue{std::int64_t(0x800000)}},
                                            {"audioType", PlistValue{std::string("media")}},
                                            {"audioLatencyMs", PlistValue{std::int64_t(20)}}}};
  const auto audio = body({{"streams", PlistValue{Array{stream}}}});
  // Exercise production admission without private phone inputs. Compatibility
  // /info declarations must never create an unimplemented downlink/uplink.
  const auto rejects = [](AirplaySessionSetup& session, Array streams) {
    bool rejected = false;
    try {
      session.respond(body({{"streams", PlistValue{std::move(streams)}}}));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "unsupported audio SETUP was admitted");
  };
  {
    AirplaySessionSetup disabled;
    disabled.set_peer(phone);
    disabled.set_secret(secret);
    disabled.respond(setup);
    rejects(disabled, {stream});
    check(disabled.media_controls().empty() && disabled.media_status().empty(),
          "disabled sink created a media owner");
  }
  {
    AirplaySessionSetup voice_first;
    voice_first.set_peer(phone);
    voice_first.set_secret(secret);
    voice_first.set_audio_sink("fakesink");
    (void)voice_first.respond(setup);
    const auto voice =
        PlistValue{Dictionary{{"type", PlistValue{std::int64_t(100)}},
                              {"streamConnectionID", PlistValue{std::int64_t(92)}},
                              {"audioFormat", PlistValue{std::int64_t(0x40000000)}},
                              {"audioType", PlistValue{std::string("speechRecognition")}}}};
    (void)voice_first.respond(body({{"streams", PlistValue{Array{voice}}}}));
    check(
        voice_first.request_audio_focus(duck) == AirplaySessionSetup::AudioFocusResult::unavailable,
        "voice-only route accepted a media duck command");
    (void)voice_first.respond(audio);
    check(voice_first.request_audio_focus(duck) == AirplaySessionSetup::AudioFocusResult::accepted,
          "earlier voice stream blocked media focus");
    const auto state = voice_first.media_status();
    check(state.size() == 2 && state[0].voice && state[0].focus_requested == 0 && !state[1].voice &&
              state[1].focus_requested == 1,
          "media focus was routed to the earlier voice stream");
  }
  std::shared_ptr<audio_control::Mailbox> retired;
  {
    AirplaySessionSetup session;
    session.set_peer(phone);
    session.set_secret(secret);
    session.set_audio_sink("fakesink");
    const auto reply = decode_binary_plist(session.respond(setup));
    const auto port =
        std::get<std::int64_t>(std::get<Dictionary>(reply.value).at("eventPort").value);
    const int event = socket(AF_INET6, SOCK_STREAM, 0);
    check(event >= 0, "event socket failed");
    Close close_event{event};
    sockaddr_in6 address = phone;
    address.sin6_port = htons(port);
    check(connect(event, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
          "event connection failed");
    timeval timeout{1, 0};
    check(setsockopt(event, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
          "event timeout failed");
    unsigned turn = 0;
    const auto malformed =
        body({{"type", PlistValue{std::string("duckAudio")}},
              {"params", PlistValue{Dictionary{{"durationMs", PlistValue{std::string("500")}}}}}});
    const auto unknown = body({{"type", PlistValue{std::string("modesChanged")}}});
    event_request(session, event, secret, duck, turn++, "501 Not Implemented");
    event_request(session, event, secret, malformed, turn++, "400 Bad Request");
    event_request(session, event, secret, unknown, turn++, "400 Bad Request");
    const auto observed =
        body({{"type", PlistValue{std::string("modesChanged")}},
              {"params", PlistValue{Dictionary{{"resources", PlistValue{Array{}}}}}}});
    event_request(session, event, secret, observed, turn++, "200 OK");
    const auto unsupported = body({{"type", PlistValue{std::string("futureCommand")}}});
    event_request(session, event, secret, unsupported, turn++, "501 Not Implemented");
    check(session.request_audio_focus(duck) == AirplaySessionSetup::AudioFocusResult::unavailable,
          "missing audio route accepted focus");
    for (const auto& [field, value] :
         Dictionary{{"type", PlistValue{std::int64_t(100)}},
                    {"audioFormat", PlistValue{std::int64_t(0x400000)}},
                    {"audioType", PlistValue{std::string("speech")}},
                    {"audioLatencyMs", PlistValue{std::int64_t(19)}}}) {
      auto invalid_stream = stream;
      std::get<Dictionary>(invalid_stream.value)[field] = value;
      rejects(session, {invalid_stream});
      // A valid leading video offer must also remain unpublished when the
      // later audio offer fails preflight, including unsupported type 100.
      const auto video =
          PlistValue{Dictionary{{"type", PlistValue{std::int64_t(110)}},
                                {"streamConnectionID", PlistValue{std::int64_t(90)}}}};
      rejects(session, {video, invalid_stream});
      check(session.video_status().empty() && session.media_status().empty(),
            "invalid SETUP batch allocated an owner before validation completed");
    }
    for (const auto type : {100, 101}) {
      auto invalid_stream = stream;
      std::get<Dictionary>(invalid_stream.value)["type"] = PlistValue{std::int64_t(type)};
      rejects(session, {invalid_stream});
    }
    for (const auto* category : {"default", "alert", "telephony", "guidance"}) {
      auto invalid_stream = stream;
      std::get<Dictionary>(invalid_stream.value)["audioType"] = PlistValue{std::string(category)};
      rejects(session, {invalid_stream});
    }
    for (const auto& value : {PlistValue{std::int64_t(2001)}, PlistValue{std::string("20")}}) {
      auto invalid_stream = stream;
      std::get<Dictionary>(invalid_stream.value)["audioLatencyMs"] = value;
      rejects(session, {invalid_stream});
    }
    const auto mode = [](std::int64_t owner, std::int64_t permanent) {
      const PlistValue resource{Dictionary{{"resourceID", PlistValue{std::int64_t(2)}},
                                           {"entity", PlistValue{owner}},
                                           {"permanentEntity", PlistValue{permanent}}}};
      const PlistValue speech{Dictionary{{"appStateID", PlistValue{std::int64_t(1)}},
                                         {"entity", PlistValue{std::int64_t(0)}},
                                         {"speechMode", PlistValue{std::int64_t(-1)}}}};
      return body({{"type", PlistValue{std::string("modesChanged")}},
                   {"params", PlistValue{Dictionary{{"resources", PlistValue{Array{resource}}},
                                                    {"appStates", PlistValue{Array{speech}}}}}}});
    };
    check(session.dispatch_command(mode(2, 1)) == 200, "pre-SETUP mainAudio mode rejected");
    session.respond(audio);
    check(!session.media_status()[0].media_resource_enabled &&
              session.media_status()[0].resource_requested == 0,
          "new audio worker briefly started with the wrong current owner");
    event_request(session, event, secret, mode(1, 2), turn++, "200 OK");
    check(session.media_status()[0].media_resource_enabled &&
              session.media_status()[0].resource_requested == 1,
          "current controller borrow did not override permanent accessory owner");
    event_request(session, event, secret, mode(2, 1), turn++, "200 OK");
    event_request(session, event, secret, mode(2, 1), turn++, "200 OK");
    check(!session.media_status()[0].media_resource_enabled &&
              session.media_status()[0].resource_requested == 2,
          "duplicate resource observation restarted permission");
    check(session.dispatch_command(mode(77, 1)) == 200 &&
              !session.media_status()[0].media_resource_enabled,
          "unknown owner or permanent controller bypassed denial");
    check(session.dispatch_command(observed) == 200 &&
              !session.media_status()[0].media_resource_enabled,
          "empty delta released resource denial");
    const auto malformed_mode =
        body({{"type", PlistValue{std::string("modesChanged")}},
              {"params", PlistValue{Dictionary{{"resources", PlistValue{true}}}}}});
    check(session.dispatch_command(malformed_mode) == 400 &&
              !session.media_status()[0].media_resource_enabled,
          "malformed mode partially enabled media");
    retired = session.media_controls()[0];
    auto duplicate = stream;
    std::get<Dictionary>(duplicate.value)["streamConnectionID"] = PlistValue{std::int64_t(92)};
    rejects(session, {duplicate});
    rejects(session, {duplicate, stream});
    check(session.media_controls().size() == 1 && session.media_controls()[0] == retired &&
              retired->snapshot().available,
          "rejected duplicate SETUP disturbed the existing audio owner");
    event_request(session, event, secret, duck, turn++, "200 OK");
    check(session.request_audio_focus(duck) == AirplaySessionSetup::AudioFocusResult::accepted,
          "current audio route rejected focus");
    check(session.request_audio_focus(duck) == AirplaySessionSetup::AudioFocusResult::accepted &&
              session.media_status()[0].focus_requested == 1,
          "duplicate target restarted focus");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (session.media_status()[0].focus_configured != 1 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto current = session.media_status()[0];
    check(current.resource_configured == 2 && !current.media_resource_enabled &&
              current.focus_configured == 1 && current.decoded_blocks == 0 &&
              current.phone_duck_gain == 1.,
          "accepted/configured focus manufactured PCM progress");
    // Repeat after the worker has handed the command to shared playback.
    // Scheduling must not turn the same target into a new request/ramp.
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
      check(session.request_audio_focus(duck) == AirplaySessionSetup::AudioFocusResult::accepted,
            "configured duplicate target was rejected");
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(session.media_status()[0].focus_requested == 1 &&
              session.media_status()[0].focus_configured == 1,
          "configured duplicate target restarted focus after handoff");
    bool invalid = false;
    try {
      session.request_audio_focus(malformed);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    check(invalid && session.media_status()[0].focus_requested == 1,
          "invalid request changed accepted focus");
    check(session.request_audio_focus(body({{"type", PlistValue{std::string("modesChanged")}}})) ==
              AirplaySessionSetup::AudioFocusResult::unhandled,
          "diagnostic command became phone focus");
    event_request(session, event, secret, malformed, turn++, "400 Bad Request");
    event_request(session, event, secret, duck, turn++, "200 OK");
    check(session.event_requests() == turn, "event command counter changed after rejection");
    check(session.start_record(), "media retirement fixture cannot RECORD");
    session.teardown_services(
        body({{"streams", PlistValue{Array{PlistValue{Dictionary{
                              {"type", PlistValue{std::int64_t(102)}},
                              {"streamConnectionID", PlistValue{std::int64_t(91)}}}}}}}}));
    check(!retired->snapshot().available && session.media_controls().empty() && session.recording(),
          "selective audio close retained a mixer or retired RECORD");
    event_request(session, event, secret, duck, turn++, "501 Not Implemented");
    bool reused = false;
    try {
      session.respond(audio);
    } catch (const std::invalid_argument&) {
      reused = true;
    }
    check(reused, "selective audio reopen reused its old key");
    auto fresh = stream;
    std::get<Dictionary>(fresh.value)["streamConnectionID"] = PlistValue{std::int64_t(92)};
    session.respond(body({{"streams", PlistValue{Array{fresh}}}}));
    check(!session.media_status()[0].media_resource_enabled,
          "same-session audio reopen discarded retained resource observation");
    event_request(session, event, secret, mode(1, 2), turn++, "200 OK");
    check(session.media_status()[0].media_resource_enabled,
          "fresh current resource grant did not restore reopened media");
    event_request(session, event, secret, duck, turn++, "200 OK");
    check(session.media_controls().size() == 1 && session.event_requests() == turn,
          "fresh audio reopen lost the existing event owner");
    close(event);
    close_event.fd = -1;
    bool event_retired = false;
    const auto retirement = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!event_retired && std::chrono::steady_clock::now() < retirement) {
      try {
        session.drain();
      } catch (const std::runtime_error&) {
        event_retired = true;
      }
    }
    check(
        event_retired && !session.recording() && session.modes().revision == 0 &&
            !session.media_status()[0].media_resource_enabled &&
            session.request_audio_focus(duck) == AirplaySessionSetup::AudioFocusResult::unavailable,
        "event EOF left resource permission in a surviving media worker");
  }
  check(!retired->snapshot().available, "session teardown retained prior mixer route");
  secret.fill(7);
  AirplaySessionSetup replacement;
  replacement.set_peer(phone);
  replacement.set_secret(secret);
  replacement.set_audio_sink("fakesink");
  replacement.respond(setup);
  check(replacement.request_audio_focus(duck) == AirplaySessionSetup::AudioFocusResult::unavailable,
        "new session inherited retired audio route");
  replacement.respond(audio);
  const auto current = replacement.media_status()[0];
  check(current.resource_requested == 0 && current.resource_configured == 0 &&
            current.media_resource_enabled && current.focus_requested == 0 &&
            current.focus_configured == 0 && current.phone_duck_gain == 1.,
        "new stream inherited old focus target/envelope");
  std::cout << "Actual SETUP/encrypted event focus owner: "
               "strict admission, atomic rejection, "
               "missing/accepted/repeated/malformed/no-PCM/teardown/new-generation checks passed "
               "(synthetic peer)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
