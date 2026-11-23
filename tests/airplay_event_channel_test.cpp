// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_event_channel.hpp"
#include "headunit/cluster_input.hpp"

#include <plist/plist.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition) {
  if (!condition) throw std::runtime_error("AirPlay event channel test failed");
}

std::vector<std::uint8_t> seal(const headunit::HapKey32& key, std::span<const std::uint8_t> plain,
                               std::uint64_t sequence = 0) {
  const std::array<std::uint8_t, 2> length{static_cast<std::uint8_t>(plain.size()),
                                           static_cast<std::uint8_t>(plain.size() >> 8)};
  std::array<char, 8> nonce{};
  for (unsigned i = 0; i < nonce.size(); ++i)
    nonce[i] = static_cast<char>(sequence >> (8 * i));
  auto body = headunit::hap_encrypt(key, {nonce.data(), nonce.size()}, plain, length);
  std::vector<std::uint8_t> result(length.begin(), length.end());
  result.insert(result.end(), body.begin(), body.end());
  return result;
}

std::vector<std::uint8_t> open(const headunit::HapKey32& key, std::span<const std::uint8_t> wire) {
  check(wire.size() >= 18);
  const auto length = static_cast<std::size_t>(wire[0]) | (static_cast<std::size_t>(wire[1]) << 8);
  check(wire.size() >= length + 18);
  const std::array<char, 8> nonce{};
  const auto clear = headunit::hap_decrypt(key, {nonce.data(), nonce.size()},
                                           wire.subspan(2, length + 16), wire.first(2));
  check(clear.has_value());
  return *clear;
}
void command_completions(const std::array<std::uint8_t, 32>& secret,
                         const headunit::HapKey32& phone_write) {
  using Channel = headunit::AirplayEventChannel;
  Channel channel(secret);
  const std::string uuid = "12345678-1234-1234-1234-123456789abc";
  (void)channel.show_ui(uuid, "maps:/car/instrumentcluster/map");
  (void)channel.force_key_frame(uuid);
  (void)channel.send_touch({true, 10, 20});
  std::uint64_t nonce = 0;
  const auto response = [&](unsigned sequence, unsigned status) {
    const std::string text = "RTSP/1.0 " + std::to_string(status) +
                             " Result\r\nCSeq: " + std::to_string(sequence) +
                             "\r\nContent-Length: 0\r\n\r\n";
    return channel.receive(seal(
        phone_write, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, nonce++));
  };
  (void)response(2, 501);
  (void)response(3, 100);
  check(channel.diagnostics().pending == 2);
  (void)response(99, 200);
  (void)response(1, 200);
  (void)response(3, 200);
  (void)response(1, 200);
  const auto completed = channel.take_completions();
  check(completed.size() == 3 && completed[0].cseq == 2 && completed[0].status == 501 &&
        completed[0].command == Channel::CommandKind::refresh &&
        completed[0].outcome == Channel::Outcome::rejected && completed[1].cseq == 1 &&
        completed[1].command == Channel::CommandKind::show_ui && completed[1].status == 200 &&
        completed[2].command == Channel::CommandKind::touch);
  const auto stats = channel.diagnostics();
  check(stats.accepted == 2 && stats.rejected == 1 && stats.unmatched == 2 && stats.pending == 0 &&
        channel.take_completions().empty());

  Channel multiword(secret);
  (void)multiword.show_ui(uuid, "maps:/car/instrumentcluster/map");
  const std::string bad_request = "RTSP/1.0 400 Bad Request\r\nCSeq: 1\r\n\r\n";
  (void)multiword.receive(
      seal(phone_write,
           {reinterpret_cast<const std::uint8_t*>(bad_request.data()), bad_request.size()}));
  const auto denied = multiword.take_completions();
  check(denied.size() == 1 && denied[0].status == 400 &&
        denied[0].outcome == Channel::Outcome::rejected);

  Channel expiring(secret);
  (void)expiring.force_key_frame(uuid);
  const auto expired = expiring.take_completions(Channel::Clock::now() + std::chrono::seconds(6));
  check(expired.size() == 1 && expired[0].outcome == Channel::Outcome::timed_out &&
        !expired[0].status && expiring.diagnostics().pending == 0);
  Channel bounded(secret);
  for (unsigned i = 0; i < 131; ++i)
    (void)bounded.send_touch({true, 10, 20});
  check(bounded.diagnostics().pending == 64 && bounded.diagnostics().evicted == 67 &&
        bounded.diagnostics().dropped_completions == 3);
  check(bounded.take_completions(Channel::Clock::now() + std::chrono::seconds(6)).size() == 64 &&
        bounded.diagnostics().pending == 0 && bounded.diagnostics().timed_out == 64);
  Channel successor(secret);
  check(successor.diagnostics().pending == 0 && successor.take_completions().empty());
  for (const std::string bad :
       {"RTSP/1.0 999 Invalid\r\nCSeq: 1\r\n\r\n", "RTSP/1.0 200 OK\r\nCSeq: 1\r\nCSeq: 2\r\n\r\n",
        "RTSP/1.0 200 OK\r\nCSeq: 4294967296\r\n\r\n",
        "POST /command RTSP/1.0 extra\r\nCSeq: 1\r\n\r\n",
        "POST\n/command RTSP/1.0\r\nCSeq: 1\r\n\r\n",
        "POST /command RTSP/1.0\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "POST /command RTSP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST /command RTSP/1.0\r\nContent-Length : 0\r\n\r\n",
        "POST /command RTSP/1.0\r\n CSeq: 1\r\n\r\n",
        "POST /command RTSP/1.0\r\nOpaque: value\r\r\n\r\n",
        "POST /command RTSP/1.0\r\nOpaque: value\x7f\r\n\r\n",
        "POST /command RTSP/1.0\r\nCSeq: 1\nContent-Length: 0\r\n\r\n"}) {
    Channel invalid(secret);
    bool rejected = false;
    try {
      (void)invalid.receive(
          seal(phone_write, {reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()}));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected);
  }
}
}  // namespace

int main() {
  std::array<std::uint8_t, 32> secret{};
  for (unsigned index = 0; index < secret.size(); ++index)
    secret[index] = index;
  const auto phone_write =
      headunit::hap_hkdf_sha512(secret, "Events-Salt", "Events-Read-Encryption-Key");
  const auto phone_read =
      headunit::hap_hkdf_sha512(secret, "Events-Salt", "Events-Write-Encryption-Key");
  headunit::AirplayEventChannel channel(secret);
  const std::string plain = "POST /command RTSP/1.0\r\nCSeq: 19\r\nContent-Length: 4\r\n\r\ntest";
  const auto wire =
      seal(phone_write, {reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()});
  check(channel.receive({wire.data(), 5}).empty());
  const auto messages = channel.receive({wire.data() + 5, wire.size() - 5});
  check(messages.size() == 1 && messages[0].method == "POST" && messages[0].path == "/command" &&
        messages[0].cseq == "19" && messages[0].body.size() == 4);
  const auto response = channel.reply(messages[0], 200);
  const auto response_plain = open(phone_read, response);
  const std::string response_text(response_plain.begin(), response_plain.end());
  check(response_text.find("200 OK") != std::string::npos &&
        response_text.find("CSeq: 19") != std::string::npos);
  for (const auto status : {400u, 501u}) {
    headunit::AirplayEventChannel rejected(secret);
    const auto bytes = open(phone_read, rejected.reply(messages[0], status));
    const std::string text(bytes.begin(), bytes.end());
    check(text.find(status == 400 ? "400 Bad Request" : "501 Not Implemented") !=
              std::string::npos &&
          text.find("CSeq: 19") != std::string::npos);
  }

  const std::array<std::uint8_t, 4> iap{0xff, 0x5a, 1, 2};
  const auto outbound = channel.send_iap_message(iap);
  // The command is one frame for this small test, with nonce 1 after reply.
  check(outbound.size() >= 18);
  const auto length =
      static_cast<std::size_t>(outbound[0]) | (static_cast<std::size_t>(outbound[1]) << 8);
  std::array<char, 8> nonce{};
  nonce[0] = 1;
  const auto command_plain =
      headunit::hap_decrypt(phone_read, {nonce.data(), nonce.size()},
                            {outbound.data() + 2, length + 16}, {outbound.data(), 2});
  check(command_plain.has_value());
  const std::string command(command_plain->begin(), command_plain->end());
  check(command.find("POST /command RTSP/1.0") == 0 &&
        command.find("CSeq: 1") != std::string::npos);
  const auto body_offset = command.find("\r\n\r\n");
  check(body_offset != std::string::npos);
  plist_t plist = nullptr;
  const auto* body = command.data() + body_offset + 4;
  const auto size = command.size() - body_offset - 4;
  check(plist_from_bin(body, static_cast<std::uint32_t>(size), &plist) == PLIST_ERR_SUCCESS &&
        plist != nullptr);
  auto params = plist_dict_get_item(plist, "params");
  auto payload = plist_dict_get_item(params, "data");
  char* data = nullptr;
  std::uint64_t data_size = 0;
  plist_get_data_val(payload, &data, &data_size);
  check(data_size == iap.size() &&
        std::equal(iap.begin(), iap.end(), reinterpret_cast<std::uint8_t*>(data)));
  plist_mem_free(data);
  plist_free(plist);

  headunit::AirplayEventChannel large_channel(secret);
  std::vector<std::uint8_t> large_iap(20000);
  for (std::size_t index = 0; index < large_iap.size(); ++index) {
    large_iap[index] = static_cast<std::uint8_t>(index);
  }
  const auto large_wire = large_channel.send_iap_message(large_iap);
  std::vector<std::uint8_t> large_clear;
  std::size_t cursor = 0;
  std::uint64_t frame_number = 0;
  while (cursor < large_wire.size()) {
    check(large_wire.size() - cursor >= 18);
    const auto fragment_length = static_cast<std::size_t>(large_wire[cursor]) |
                                 (static_cast<std::size_t>(large_wire[cursor + 1]) << 8);
    check(fragment_length <= 1024 && fragment_length + 18 <= large_wire.size() - cursor);
    std::array<char, 8> fragment_nonce{};
    for (unsigned index = 0; index < 8; ++index) {
      fragment_nonce[index] = static_cast<char>(frame_number >> (index * 8));
    }
    const auto fragment = headunit::hap_decrypt(
        phone_read, {fragment_nonce.data(), fragment_nonce.size()},
        {large_wire.data() + cursor + 2, fragment_length + 16}, {large_wire.data() + cursor, 2});
    check(fragment.has_value());
    large_clear.insert(large_clear.end(), fragment->begin(), fragment->end());
    cursor += fragment_length + 18;
    ++frame_number;
  }
  check(frame_number > 1);
  const std::string large_text(large_clear.begin(), large_clear.end());
  const auto large_body_offset = large_text.find("\r\n\r\n");
  check(large_body_offset != std::string::npos);
  plist_t large_plist = nullptr;
  check(plist_from_bin(large_text.data() + large_body_offset + 4,
                       static_cast<std::uint32_t>(large_text.size() - large_body_offset - 4),
                       &large_plist) == PLIST_ERR_SUCCESS &&
        large_plist);
  auto large_params = plist_dict_get_item(large_plist, "params");
  auto large_payload = plist_dict_get_item(large_params, "data");
  char* large_data = nullptr;
  std::uint64_t large_size = 0;
  plist_get_data_val(large_payload, &large_data, &large_size);
  check(large_size == large_iap.size() && std::equal(large_iap.begin(), large_iap.end(),
                                                     reinterpret_cast<std::uint8_t*>(large_data)));
  plist_mem_free(large_data);
  plist_free(large_plist);

  // Admission must use the actual plist/header/authentication cost. Repeated
  // capacity rejection cannot skip CSeq or encryption nonces before retry.
  headunit::AirplayEventChannel reference(secret);
  headunit::AirplayEventChannel pressured(secret);
  const std::vector<std::uint8_t> maximum_iap(60000, 0x5a);
  const auto expected_wire = reference.send_iap_message(maximum_iap);
  check(expected_wire.size() > maximum_iap.size() + 1024);
  for (const auto capacity : {std::size_t(0), expected_wire.size() - 1}) {
    check(pressured.send_iap_message(maximum_iap, capacity).empty());
    check(pressured.diagnostics().pending == 0 && pressured.take_completions().empty());
  }
  check(pressured.send_iap_message(maximum_iap, expected_wire.size()) == expected_wire);
  check(pressured.diagnostics().pending == 1);
  const std::string admitted = "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
  (void)pressured.receive(
      seal(phone_write, {reinterpret_cast<const std::uint8_t*>(admitted.data()), admitted.size()}));
  const auto admission_completion = pressured.take_completions();
  check(admission_completion.size() == 1 && admission_completion[0].cseq == 1 &&
        admission_completion[0].command == headunit::AirplayEventChannel::CommandKind::iap &&
        admission_completion[0].outcome == headunit::AirplayEventChannel::Outcome::accepted &&
        pressured.diagnostics().pending == 0);
  check(pressured.send_iap_message(iap) == reference.send_iap_message(iap));

  {
    headunit::set_multitouch(true);
    headunit::AirplayEventChannel pinch_channel(secret);
    headunit::TouchInput two{true, 0x123, 0x234, true, 0x345, 0x056};
    const auto pinch_plain = open(phone_read, pinch_channel.send_touch(two));
    headunit::set_multitouch(false);
    const std::string pinch_text(pinch_plain.begin(), pinch_plain.end());
    const auto start = pinch_text.find("\r\n\r\n");
    plist_t pinch = nullptr;
    check(start != std::string::npos &&
          plist_from_bin(pinch_text.data() + start + 4,
                         static_cast<std::uint32_t>(pinch_text.size() - start - 4),
                         &pinch) == PLIST_ERR_SUCCESS &&
          pinch);
    char* report = nullptr;
    std::uint64_t report_size = 0;
    plist_get_data_val(plist_dict_get_item(pinch, "hidReport"), &report, &report_size);
    const std::array<std::uint8_t, 12> expected{1, 1, 0x23, 0x01, 0x34, 0x02,
                                                2, 1, 0x45, 0x03, 0x56, 0x00};
    check(report_size == expected.size() &&
          std::equal(expected.begin(), expected.end(), reinterpret_cast<std::uint8_t*>(report)));
    plist_mem_free(report);
    plist_free(pinch);
  }
  for (bool pressed : {false, true}) {
    headunit::AirplayEventChannel touch_channel(secret);
    const auto touch_plain = open(phone_read, touch_channel.send_touch({pressed, 0x123, 0x234}));
    const std::string touch_text(touch_plain.begin(), touch_plain.end());
    const auto start = touch_text.find("\r\n\r\n");
    check(start != std::string::npos);
    plist_t touch_plist = nullptr;
    check(plist_from_bin(touch_text.data() + start + 4,
                         static_cast<std::uint32_t>(touch_text.size() - start - 4),
                         &touch_plist) == PLIST_ERR_SUCCESS &&
          touch_plist);
    char* report = nullptr;
    std::uint64_t report_size = 0;
    plist_get_data_val(plist_dict_get_item(touch_plist, "hidReport"), &report, &report_size);
    const std::array<std::uint8_t, 6> expected{
        1, static_cast<std::uint8_t>(pressed ? 1 : 0), 0x23, 0x01, 0x34, 0x02};
    check(report_size == expected.size() &&
          std::equal(expected.begin(), expected.end(), reinterpret_cast<std::uint8_t*>(report)));
    char* uuid = nullptr;
    plist_get_string_val(plist_dict_get_item(touch_plist, "uuid"), &uuid);
    check(uuid && std::string(uuid) == headunit::touchscreen_hid_uuid);
    plist_mem_free(uuid);
    plist_mem_free(report);
    plist_free(touch_plist);
  }

  headunit::AirplayEventChannel ui_channel(secret);
  const std::string display = "12345678-1234-1234-1234-123456789abc";
  const std::string map_url = "maps:/car/instrumentcluster/map";
  bool bad_target = false;
  try {
    (void)ui_channel.show_ui("invalid", map_url);
  } catch (const std::invalid_argument&) {
    bad_target = true;
  }
  check(bad_target);
  const auto ui_plain = open(phone_read, ui_channel.show_ui(display, map_url));
  const std::string ui_text(ui_plain.begin(), ui_plain.end());
  check(ui_text.find("CSeq: 1\r\n") != std::string::npos);
  const auto ui_start = ui_text.find("\r\n\r\n");
  check(ui_start != std::string::npos);
  plist_t ui = nullptr;
  check(plist_from_bin(ui_text.data() + ui_start + 4,
                       static_cast<std::uint32_t>(ui_text.size() - ui_start - 4),
                       &ui) == PLIST_ERR_SUCCESS &&
        ui);
  const auto string_is = [](plist_t node, const std::string& expected) {
    char* value = nullptr;
    plist_get_string_val(node, &value);
    const bool matches = value && expected == value;
    plist_mem_free(value);
    return matches;
  };
  check(string_is(plist_dict_get_item(ui, "type"), "showUI"));
  auto ui_params = plist_dict_get_item(ui, "params");
  check(string_is(plist_dict_get_item(ui_params, "uuid"), display));
  check(string_is(plist_dict_get_item(ui_params, "url"), map_url));
  plist_free(ui);
  headunit::AirplayEventChannel refresh_channel(secret);
  bool invalid_refresh = false;
  try {
    (void)refresh_channel.force_key_frame("Cluster_Display");
  } catch (const std::invalid_argument&) {
    invalid_refresh = true;
  }
  check(invalid_refresh);  // Refresh targets a video UUID, never a panel name.
  const auto refresh_plain = open(phone_read, refresh_channel.force_key_frame(display));
  const std::string refresh_text(refresh_plain.begin(), refresh_plain.end());
  const auto refresh_start = refresh_text.find("\r\n\r\n");
  check(refresh_start != std::string::npos &&
        refresh_text.find("CSeq: 1\r\n") != std::string::npos);
  plist_t refresh = nullptr;
  check(plist_from_bin(refresh_text.data() + refresh_start + 4,
                       refresh_text.size() - refresh_start - 4, &refresh) == PLIST_ERR_SUCCESS &&
        refresh);
  check(string_is(plist_dict_get_item(refresh, "type"), "forceKeyFrame"));
  const auto refresh_params = plist_dict_get_item(refresh, "params");
  check(plist_dict_get_size(refresh_params) == 1 &&
        string_is(plist_dict_get_item(refresh_params, "uuid"), display));
  plist_free(refresh);
  {
    headunit::AirplayEventChannel media_channel(secret);
    bool invalid_media = false;
    try {
      (void)media_channel.send_media_button(3);
    } catch (const std::invalid_argument&) {
      invalid_media = true;
    }
    check(invalid_media);  // One key at a time; chords are never sent.
    const auto media_plain = open(phone_read, media_channel.send_media_button(2));
    const std::string media_text(media_plain.begin(), media_plain.end());
    const auto media_start = media_text.find("\r\n\r\n");
    plist_t media = nullptr;
    check(media_start != std::string::npos &&
          plist_from_bin(media_text.data() + media_start + 4, media_text.size() - media_start - 4,
                         &media) == PLIST_ERR_SUCCESS &&
          media);
    check(string_is(plist_dict_get_item(media, "type"), "hidSendReport") &&
          string_is(plist_dict_get_item(media, "uuid"), headunit::media_hid_uuid));
    char* bytes = nullptr;
    std::uint64_t length = 0;
    plist_get_data_val(plist_dict_get_item(media, "hidReport"), &bytes, &length);
    check(length == 1 && bytes[0] == 2);
    plist_mem_free(bytes);
    plist_free(media);
  }
  {
    headunit::AirplayEventChannel open_channel(secret);
    bool invalid_url = false;
    try {
      (void)open_channel.request_ui("maps: bad");
    } catch (const std::invalid_argument&) {
      invalid_url = true;
    }
    check(invalid_url);
    const auto plain = open(phone_read, open_channel.request_ui("maps:"));
    const std::string text(plain.begin(), plain.end());
    const auto start = text.find("\r\n\r\n");
    plist_t command = nullptr;
    check(start != std::string::npos &&
          plist_from_bin(text.data() + start + 4, text.size() - start - 4, &command) ==
              PLIST_ERR_SUCCESS &&
          command);
    check(string_is(plist_dict_get_item(command, "type"), "requestUI") &&
          string_is(plist_dict_get_item(plist_dict_get_item(command, "params"), "url"), "maps:"));
    plist_free(command);
  }
  {
    headunit::AirplayEventChannel limited_channel(secret);
    const auto plain = open(phone_read, limited_channel.set_limited_ui(true));
    const std::string text(plain.begin(), plain.end());
    const auto start = text.find("\r\n\r\n");
    plist_t command = nullptr;
    check(start != std::string::npos &&
          plist_from_bin(text.data() + start + 4, text.size() - start - 4, &command) ==
              PLIST_ERR_SUCCESS &&
          command);
    check(string_is(plist_dict_get_item(command, "type"), "setLimitedUI"));
    const auto flag = plist_dict_get_item(plist_dict_get_item(command, "params"), "limitedUI");
    uint8_t value = 0;
    check(plist_get_node_type(flag) == PLIST_BOOLEAN);
    plist_get_bool_val(flag, &value);
    check(value == 1);
    plist_free(command);
  }
  for (const bool night : {true, false}) {
    headunit::AirplayEventChannel night_channel(secret);
    const auto night_plain = open(phone_read, night_channel.set_night_mode(night));
    const std::string night_text(night_plain.begin(), night_plain.end());
    const auto night_start = night_text.find("\r\n\r\n");
    check(night_start != std::string::npos && night_text.starts_with("POST /command RTSP/1.0\r\n"));
    plist_t command = nullptr;
    check(plist_from_bin(night_text.data() + night_start + 4, night_text.size() - night_start - 4,
                         &command) == PLIST_ERR_SUCCESS &&
          command);
    check(string_is(plist_dict_get_item(command, "type"), "setNightMode"));
    const auto night_params = plist_dict_get_item(command, "params");
    uint8_t value = 2;
    const auto flag = plist_dict_get_item(night_params, "nightMode");
    check(plist_dict_get_size(night_params) == 1 && plist_get_node_type(flag) == PLIST_BOOLEAN);
    plist_get_bool_val(flag, &value);
    check(value == (night ? 1 : 0));
    plist_free(command);
  }
  const std::string ui_reply = "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
  const auto replies = ui_channel.receive(
      seal(phone_write, {reinterpret_cast<const std::uint8_t*>(ui_reply.data()), ui_reply.size()}));
  check(replies.size() == 1 && replies[0].response && replies[0].path == "200" &&
        replies[0].cseq == "1");
  const auto ui_completed = ui_channel.take_completions();
  check(ui_completed.size() == 1 && ui_completed[0].status == 200);
  command_completions(secret, phone_write);
  for (std::uint8_t report : {0, 1, 2, 4, 8, 16, 32}) {
    headunit::AirplayEventChannel buttons(secret);
    bool invalid = false;
    try {
      (void)buttons.send_cluster_button(3);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    check(invalid);
    const auto plain = open(phone_read, buttons.send_cluster_button(report));
    const std::string text(plain.begin(), plain.end());
    const auto start = text.find("\r\n\r\n");
    check(start != std::string::npos && text.find("CSeq: 1\r\n") != std::string::npos);
    plist_t command = nullptr;
    check(plist_from_bin(text.data() + start + 4, text.size() - start - 4, &command) ==
          PLIST_ERR_SUCCESS);
    check(string_is(plist_dict_get_item(command, "type"), "hidSendReport") &&
          string_is(plist_dict_get_item(command, "uuid"), headunit::cluster_hid_uuid));
    char* bytes = nullptr;
    std::uint64_t size = 0;
    plist_get_data_val(plist_dict_get_item(command, "hidReport"), &bytes, &size);
    check(size == 1 && static_cast<std::uint8_t>(bytes[0]) == report);
    plist_mem_free(bytes);
    plist_free(command);
  }
  headunit::AirplayEventMessage injected{"POST", "/command", "RTSP/1.0", "1\r\nInjected: header",
                                         {},     false};
  bool injection_rejected = false;
  try {
    (void)ui_channel.reply(injected, 200);
  } catch (const std::invalid_argument&) {
    injection_rejected = true;
  }
  check(injection_rejected);

  // A complete maximum-size event and its successor may share a TCP read.
  // Only the unfinished message belongs to the retained-message budget.
  headunit::AirplayEventChannel boundary(secret);
  std::size_t body_bytes = 65000;
  std::string maximum_header;
  for (;;) {
    maximum_header =
        "POST /command RTSP/1.0\r\nCSeq: 20\r\nContent-Length: " + std::to_string(body_bytes) +
        "\r\n\r\n";
    const auto adjusted = 65536 - maximum_header.size();
    if (adjusted == body_bytes) break;
    body_bytes = adjusted;
  }
  const std::string maximum_message = maximum_header + std::string(body_bytes, 'x');
  const std::string following = "POST /command RTSP/1.0\r\nCSeq: 21\r\nContent-Length: 0\r\n\r\n";
  const std::string burst_plain = maximum_message + following;
  std::vector<std::uint8_t> burst_wire;
  std::uint64_t burst_nonce = 0;
  for (std::size_t offset = 0; offset < burst_plain.size(); offset += 1024) {
    const auto count = std::min<std::size_t>(1024, burst_plain.size() - offset);
    const auto next = seal(
        phone_write, {reinterpret_cast<const std::uint8_t*>(burst_plain.data() + offset), count},
        burst_nonce++);
    burst_wire.insert(burst_wire.end(), next.begin(), next.end());
  }
  check(boundary.receive(std::span(burst_wire).first(65000)).empty());
  const auto boundary_messages = boundary.receive(std::span(burst_wire).subspan(65000));
  check(boundary_messages.size() == 2 && boundary_messages[0].cseq == "20" &&
        boundary_messages[0].body == std::vector<std::uint8_t>(body_bytes, 'x') &&
        boundary_messages[1].cseq == "21" && boundary_messages[1].body.empty());

  headunit::AirplayEventChannel oversized_event(secret);
  const std::string oversized_header = "POST /command RTSP/1.0\r\nContent-Length: 65536\r\n\r\n";
  bool oversized_rejected = false;
  try {
    (void)oversized_event.receive(seal(
        phone_write,
        {reinterpret_cast<const std::uint8_t*>(oversized_header.data()), oversized_header.size()}));
  } catch (const std::invalid_argument&) {
    oversized_rejected = true;
  }
  check(oversized_rejected);  // The message limit includes its header.

  auto tampered = wire;
  tampered.back() ^= 1;
  headunit::AirplayEventChannel invalid(secret);
  bool rejected = false;
  try {
    (void)invalid.receive(tampered);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected);
  return 0;
}
