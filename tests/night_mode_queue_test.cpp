// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Exercise the existing internal caller without adding a production API or
// copying its admission/dedup logic. The receiver executable entry is not run.
#define main headunit_control_probe_entry_for_night_mode_test
#include "../src/control_probe.cpp"
#undef main

#include "headunit/binary_plist.hpp"

namespace appearance_test {
using Value = headunit::PlistValue;
using Dict = Value::Dictionary;

void require(bool valid, const char* reason) {
  if (!valid) throw std::runtime_error(reason);
}

struct Peer {
  int fd = -1;

  explicit Peer(std::uint16_t port) : fd(socket(AF_INET, SOCK_STREAM, 0)) {
    require(fd >= 0, "cannot create loopback event peer");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      close(fd);
      fd = -1;
      throw std::runtime_error("cannot connect loopback event peer");
    }
    const timeval timeout{2, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
      close(fd);
      fd = -1;
      throw std::runtime_error("cannot bound loopback event read");
    }
  }

  ~Peer() {
    if (fd >= 0) close(fd);
  }

  void expect(std::span<const std::uint8_t> expected) const {
    require(!expected.empty(), "empty expected event command");
    std::vector<std::uint8_t> actual(expected.size());
    require(
        recv(fd, actual.data(), actual.size(), MSG_WAITALL) == static_cast<ssize_t>(actual.size()),
        "missing encrypted event command");
    require(std::equal(actual.begin(), actual.end(), expected.begin()),
            "event command, order, CSeq or encryption counter differs");
  }

  void expect_idle() const {
    std::uint8_t byte = 0;
    require(recv(fd, &byte, 1, MSG_DONTWAIT) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
            "duplicate or rejected choice reached event transport");
  }
};

std::uint16_t initialize(Connection& connection, std::uint8_t secret_byte) {
  connection.session_setup = std::make_unique<headunit::AirplaySessionSetup>();
  std::array<std::uint8_t, 32> secret{};
  secret.fill(secret_byte);  // Synthetic fixture identity, never a phone key.
  connection.session_setup->set_secret(secret);
  sockaddr_in6 peer{};
  peer.sin6_family = AF_INET6;
  peer.sin6_addr = in6addr_loopback;
  peer.sin6_port = htons(9);
  connection.session_setup->set_peer(peer);
  const auto request = headunit::encode_binary_plist(Value{Dict{{"timingPort", Value{9}}}});
  const auto response = headunit::decode_binary_plist(connection.session_setup->respond(request));
  return static_cast<std::uint16_t>(
      std::get<std::int64_t>(std::get<Dict>(response.value).at("eventPort").value));
}

void lamp(bool on) {
  headunit::VehicleFacts facts;
  facts.low_beam = on;
  facts.simulated = true;
  facts.observed_at = headunit::Clock::now();
  headunit::publish_vehicle_facts(facts);
}

void reject_first_command(const Peer& peer, std::span<const std::uint8_t> secret) {
  const std::string response = "RTSP/1.0 501 Rejected\r\nCSeq: 1\r\nContent-Length: 0\r\n\r\n";
  const auto key = headunit::hap_hkdf_sha512(secret, "Events-Salt", "Events-Read-Encryption-Key");
  const std::array<char, 8> nonce{};
  const std::array<std::uint8_t, 2> length{static_cast<std::uint8_t>(response.size()), 0};
  const auto sealed = headunit::hap_encrypt(
      key, {nonce.data(), nonce.size()},
      {reinterpret_cast<const std::uint8_t*>(response.data()), response.size()}, length);
  std::vector<std::uint8_t> wire(length.begin(), length.end());
  wire.insert(wire.end(), sealed.begin(), sealed.end());
  require(
      send(peer.fd, wire.data(), wire.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(wire.size()),
      "cannot send synthetic event rejection");
}

void queue_and_retire(bool record_first, bool bad_tag) {
  Connection connection;
  const auto port = initialize(connection, 1);
  auto& session = *connection.session_setup;
  std::array<std::uint8_t, 32> secret{};
  secret.fill(1);
  headunit::AirplayEventChannel expected(secret);

  headunit::set_receiver_night_mode(std::nullopt);
  pump_night_mode(connection);
  require(!connection.night_mode_sent.has_value(), "unconfigured policy remembered a choice");
  headunit::set_receiver_night_mode(headunit::parse_night_mode_policy("on"));
  pump_night_mode(connection);
  require(!connection.night_mode_sent.has_value(), "unavailable channel remembered a choice");
  if (record_first) {
    require(session.start_record(), "RECORD before event accept failed");
    pump_night_mode(connection);
    require(!connection.night_mode_sent.has_value(), "RECORD without channel remembered a choice");
  }
  Peer peer(port);
  session.drain();
  if (!record_first) {
    pump_night_mode(connection);
    require(!connection.night_mode_sent.has_value(), "pre-RECORD choice was remembered");
    require(session.start_record(), "RECORD after event accept failed");
  }
  // Admission uses the current policy, not the previously refused On choice.
  headunit::set_receiver_night_mode(headunit::parse_night_mode_policy("off"));
  pump_night_mode(connection);
  require(connection.night_mode_sent == false, "current Off choice was not queued");
  pump_night_mode(connection);
  session.drain();
  peer.expect(expected.set_night_mode(false));
  peer.expect_idle();

  // Queue success is intent. A synthetic 501 is diagnostic, not applied
  // appearance or a reason to add an automatic retry policy.
  reject_first_command(peer, secret);
  session.drain();
  pump_night_mode(connection);
  session.drain();
  require(connection.night_mode_sent == false, "response changed queued-choice bookkeeping");
  peer.expect_idle();

  const std::vector<std::uint8_t> payload(33000, 0x55);
  require(session.send_iap_message(payload), "cannot saturate existing event queue");
  require(!session.send_night_mode(true), "over-limit event queue admitted night command");
  headunit::set_receiver_night_mode(headunit::parse_night_mode_policy("headlights"));
  lamp(true);
  pump_night_mode(connection);
  require(connection.night_mode_sent == false, "refused On choice was remembered as queued");
  lamp(false);
  session.drain();
  peer.expect(expected.send_iap_message(payload));
  pump_night_mode(connection);
  session.drain();
  peer.expect_idle();
  lamp(true);
  pump_night_mode(connection);
  pump_night_mode(connection);
  session.drain();
  peer.expect(expected.set_night_mode(true));
  peer.expect_idle();
  require(connection.night_mode_sent == true, "fresh current lamp fact was not queued");

  // Leave a different choice queued when the event owner fails. Its bytes and
  // choice cannot authorize a replacement control connection.
  lamp(false);
  pump_night_mode(connection);
  require(connection.night_mode_sent == false, "terminal fixture did not leave Off queued");
  if (bad_tag) {
    const std::array<std::uint8_t, 19> invalid{1};
    require(send(peer.fd, invalid.data(), invalid.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(invalid.size()),
            "cannot send synthetic bad event tag");
  } else {
    require(shutdown(peer.fd, SHUT_RDWR) == 0, "cannot terminate loopback event peer");
  }
  UltraRuntime ultra;
  require(!pump_session(connection, nullptr, ultra, nullptr),
          "event failure kept control owner live");
  require(!session.recording() && !session.start_record() && !session.send_night_mode(true),
          "retired event owner regained night-mode admission");
  lamp(true);
  pump_night_mode(connection);
  require(connection.night_mode_sent == false, "retired owner remembered a new choice");
  require(!pump_session(connection, nullptr, ultra, nullptr), "repeated pump revived event owner");
  const int retry = socket(AF_INET, SOCK_STREAM, 0);
  require(retry >= 0, "cannot create retired-port probe");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  const auto retry_result = connect(retry, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  close(retry);
  require(retry_result < 0, "retired event port accepted replacement with old keys");

  Connection successor;
  const auto successor_port = initialize(successor, 2);
  require(!successor.night_mode_sent.has_value(), "successor inherited old queued choice");
  require(successor.session_setup->start_record(), "successor RECORD failed");
  Peer successor_peer(successor_port);
  successor.session_setup->drain();
  pump_night_mode(successor);
  pump_night_mode(successor);
  successor.session_setup->drain();
  std::array<std::uint8_t, 32> successor_secret{};
  successor_secret.fill(2);
  headunit::AirplayEventChannel successor_expected(successor_secret);
  successor_peer.expect(successor_expected.set_night_mode(true));
  successor_peer.expect_idle();
  require(successor.night_mode_sent == true && connection.night_mode_sent == false,
          "old connection choice contaminated successor current-policy admission");
  std::cout
      << "loopback night queue/rejection/current-facts/retirement/successor passed RECORD-first="
      << record_first << " bad-tag=" << bad_tag << '\n';
}

struct ClearPolicy {
  ~ClearPolicy() {
    headunit::set_receiver_night_mode(std::nullopt);
    headunit::clear_vehicle_facts();
  }
};
}  // namespace appearance_test

int main() try {
  appearance_test::ClearPolicy clear;
  for (const bool record_first : {false, true}) {
    for (const bool bad_tag : {false, true}) {
      appearance_test::queue_and_retire(record_first, bad_tag);
    }
  }
  std::cout
      << "four synthetic loopback cases; queued intent only, no phone/applied/presentation claim\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
