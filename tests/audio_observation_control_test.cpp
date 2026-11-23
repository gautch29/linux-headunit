// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "audio_observation_storage_fixture.hpp"
#include "headunit/hap_crypto.hpp"
#include "headunit/hap_pair_store.hpp"
#include "headunit/hap_tlv.hpp"
#include "headunit/binary_plist.hpp"
#include <openssl/pem.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <span>
#include <spawn.h>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
using namespace headunit;
namespace {
using Clock = std::chrono::steady_clock;
void check(bool ok, const char* error) {
  if (!ok) throw std::runtime_error(error);
}
struct Fixture {
  std::filesystem::path root;
  pid_t receiver = -1, collector = -1;
  bool storage = false, retired = false;
  Fixture() {
    char path[] = "/tmp/hu-pcm-control-XXXXXX";
    check(mkdtemp(path), "private fixture failed");
    root = path;
  }
  ~Fixture() {
    if (receiver > 0) {
      kill(receiver, SIGKILL);
      waitpid(receiver, nullptr, 0);
    }
    if (collector > 0) kill(collector, SIGKILL);
    if (storage && !retired) {
      try {
        const auto fallback =
            audio_retention_fixture::retain(root, "/tmp", {"receiver.log", "state.json"}, false);
        std::cerr << "PCM_RETENTION_FALLBACK=" << fallback.string() << '\n';
      } catch (const std::exception& error) {
        std::cerr << "PCM_RETENTION_FAILED_ORIGINAL=" << root.string() << " reason=" << error.what()
                  << '\n';
      }
      return;
    }
    std::filesystem::remove_all(root);
  }
  void retire(const std::filesystem::path& evidence) {
    (void)audio_retention_fixture::retain(root, evidence, {"receiver.log", "state.json"}, true);
    retired = true;
  }
  void write(const char* name, const std::string& bytes) {
    std::ofstream(root / name) << bytes;
    chmod((root / name).c_str(), 0600);
  }
  std::string key(const char* name) {
    auto* raw = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    check(raw, "test key generation failed");
    const auto path = (root / name).string();
    FILE* output = fopen(path.c_str(), "w");
    const bool ok =
        output && PEM_write_PrivateKey(output, raw, nullptr, nullptr, 0, nullptr, nullptr) == 1;
    if (output) fclose(output);
    EVP_PKEY_free(raw);
    check(ok, "test key write failed");
    chmod(path.c_str(), 0600);
    return path;
  }
};
std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::string bytes((std::istreambuf_iterator<char>(file)), {});
  check(bytes.size() < 1024 * 1024, "fixture evidence grew beyond bound");
  return bytes;
}
std::uint64_t number(const std::string& bytes, const char* key) {
  const auto at = bytes.find(std::string("\"") + key + "\":");
  if (at == std::string::npos) return 0;
  auto begin = bytes.data() + at + std::string(key).size() + 3;
  if (*begin == '"') ++begin;
  std::uint64_t value = 0;
  check(std::from_chars(begin, bytes.data() + bytes.size(), value).ec == std::errc{},
        "fixture numeric receipt malformed");
  return value;
}
void receive_exact(int fd, std::span<std::uint8_t> bytes) {
  check(recv(fd, bytes.data(), bytes.size(), MSG_WAITALL) == static_cast<ssize_t>(bytes.size()),
        "actual control response stalled/truncated");
}
struct Client {
  int fd = -1;
  std::optional<HapKey32> read_key, write_key;
  std::uint64_t received = 0, sent = 0;
  ~Client() {
    if (fd >= 0) close(fd);
  }
  static std::string nonce(std::uint64_t value) {
    std::string bytes(8, 0);
    for (unsigned i = 0; i < 8; ++i)
      bytes[i] = value >> (i * 8);
    return bytes;
  }
  std::vector<std::uint8_t> request(const char* method, const char* path, unsigned sequence,
                                    std::span<const std::uint8_t> body = {}) {
    std::string plain =
        std::string(method) + " " + path + " RTSP/1.0\r\nCSeq: " + std::to_string(sequence) +
        "\r\nX-Apple-HKP: 0\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    if (!body.empty()) plain.append(reinterpret_cast<const char*>(body.data()), body.size());
    std::vector<std::uint8_t> wire;
    if (write_key) {
      check(plain.size() <= 1024, "fixture request exceeds frame bound");
      const std::array<std::uint8_t, 2> size{std::uint8_t(plain.size()),
                                             std::uint8_t(plain.size() >> 8)};
      const auto encrypted =
          hap_encrypt(*write_key, nonce(sent++),
                      {reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()}, size);
      wire.assign(size.begin(), size.end());
      wire.insert(wire.end(), encrypted.begin(), encrypted.end());
    } else
      wire.assign(plain.begin(), plain.end());
    check(send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(wire.size()),
          "actual control send failed");
    std::string response;
    std::size_t header = std::string::npos, length = 0;
    do {
      if (read_key) {
        std::array<std::uint8_t, 2> size;
        receive_exact(fd, size);
        const auto count = std::size_t(size[0]) | (std::size_t(size[1]) << 8);
        check(count && count <= 1024, "fixture response frame malformed");
        std::vector<std::uint8_t> encrypted(count + 16);
        receive_exact(fd, encrypted);
        const auto opened = hap_decrypt(*read_key, nonce(received++), encrypted, size);
        check(opened.has_value(), "actual response authentication failed");
        response.append(opened->begin(), opened->end());
      } else {
        std::uint8_t byte;
        receive_exact(fd, {&byte, 1});
        response.push_back(byte);
      }
      check(response.size() <= 65536, "fixture response exceeds bound");
      header = response.find("\r\n\r\n");
      if (header != std::string::npos) {
        const auto field = response.find("Content-Length: ");
        check(field != std::string::npos, "response missing actual length");
        length = std::stoul(response.substr(field + 16));
      }
    } while (header == std::string::npos || response.size() < header + 4 + length);
    check(response.starts_with("RTSP/1.0 200 OK"), "actual control request rejected");
    return {response.begin() + header + 4, response.end()};
  }
};
std::vector<std::uint8_t> field(const std::vector<HapTlv>& values, unsigned tag) {
  for (const auto& item : values)
    if (item.type == tag) return item.value;
  throw std::runtime_error("pair verification fixture missing field");
}
auto plist(PlistValue::Dictionary value) {
  return encode_binary_plist(PlistValue{std::move(value)});
}
}  // namespace
int main(int argc, char** argv) try {
  check(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--storage-enospc"),
        "expected actual receiver executable and optional storage fixture");
  const bool storage = argc == 3;
  if (storage && !std::getenv("HEADUNIT_PCM_STORAGE_FIXTURE")) {
    std::cout << "SKIP: explicit private <=1MiB Docker tmpfs required\n";
    return 77;
  }
  std::unique_ptr<audio_storage_fixture::Scratch> scratch;
  if (storage) scratch = std::make_unique<audio_storage_fixture::Scratch>();
  Fixture fixture;
  fixture.storage = storage;
  const auto accessory_path = fixture.key("accessory.pem");
  HapEd25519Identity accessory(accessory_path), controller(fixture.key("controller.pem"));
  std::string public_hex;
  constexpr char digits[] = "0123456789abcdef";
  for (const auto byte : accessory.public_key()) {
    public_hex += digits[byte >> 4];
    public_hex += digits[byte & 15];
  }
  fixture.write(
      "identity.conf",
      "name=PCM "
      "Fixture\ndeviceid=02:00:00:00:00:01\nfeatures=0x44540380,0x61\nflags=0x4\nmodel="
      "Fixture\nprotovers=1.1\nsrcvers=950.7.1\npi=00000000-0000-0000-0000-000000000001\npk=" +
          public_hex +
          "\nmain_display_id=00000000-0000-0000-0000-000000000002\ncluster_display_id=00000000-"
          "0000-0000-0000-000000000003\n");
  fixture.write("secret", "123-45-678");
  HapPairStore store((fixture.root / "pairs").string());
  const std::string controller_id = "fixture-controller";
  store.save({controller_id, controller.public_key()});
  int reserve = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in6 target{};
  target.sin6_family = AF_INET6;
  target.sin6_addr = in6addr_loopback;
  check(bind(reserve, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0,
        "fixture port failed");
  socklen_t size = sizeof(target);
  check(getsockname(reserve, reinterpret_cast<sockaddr*>(&target), &size) == 0,
        "fixture port unavailable");
  close(reserve);
  const auto port = std::to_string(ntohs(target.sin6_port));
  const auto private_dir = fixture.root.string(),
             identity = (fixture.root / "identity.conf").string();
  const auto secret = (fixture.root / "secret").string(), pairs = (fixture.root / "pairs").string();
  const auto trace = ((storage ? scratch->root : fixture.root) / "trace.jsonl").string(),
             state = (fixture.root / "state.json").string();
  setenv("HEADUNIT_AUDIO_OBSERVATION_FILE", trace.c_str(), 1);
  setenv("HEADUNIT_AUDIO_SINK", "fakesink", 1);
  setenv("HEADUNIT_AUDIO_STATE_FILE", state.c_str(), 1);
  const auto request_path = (fixture.root / "request.json").string();
  setenv("HEADUNIT_AUDIO_CONTROL_FILE", request_path.c_str(), 1);
  const int log =
      open((fixture.root / "receiver.log").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, log, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, log, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, log);
  std::array<char*, 9> arguments{argv[1],
                                 const_cast<char*>(port.c_str()),
                                 const_cast<char*>("0"),
                                 const_cast<char*>(private_dir.c_str()),
                                 const_cast<char*>(identity.c_str()),
                                 const_cast<char*>(secret.c_str()),
                                 const_cast<char*>(accessory_path.c_str()),
                                 const_cast<char*>(pairs.c_str()),
                                 nullptr};
  check(posix_spawn(&fixture.receiver, argv[1], &actions, nullptr, arguments.data(), environ) == 0,
        "actual receiver spawn failed");
  posix_spawn_file_actions_destroy(&actions);
  close(log);
  Client client;
  const auto ready = Clock::now() + std::chrono::seconds(3);
  do {
    client.fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(client.fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) break;
    close(client.fd);
    client.fd = -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (Clock::now() < ready);
  check(client.fd >= 0, "actual receiver listener unavailable");
  timeval timeout{1, 0};
  setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  HapX25519Ephemeral ephemeral;
  const auto peer = ephemeral.public_key();
  const auto pv2 = decode_hap_tlv(client.request(
      "POST", "/pair-verify", 1,
      encode_hap_tlv(std::vector<HapTlv>{{6, {1}}, {3, {peer.begin(), peer.end()}}})));
  const auto other = field(pv2, 3);
  HapKey32 other_key;
  std::copy(other.begin(), other.end(), other_key.begin());
  const auto shared = ephemeral.shared_secret(other_key);
  check(shared.has_value(), "fixture shared secret missing");
  std::vector<std::uint8_t> signed_bytes(peer.begin(), peer.end());
  signed_bytes.insert(signed_bytes.end(), controller_id.begin(), controller_id.end());
  signed_bytes.insert(signed_bytes.end(), other.begin(), other.end());
  const auto signature = controller.sign(signed_bytes);
  const auto proof =
      encode_hap_tlv(std::vector<HapTlv>{{1, {controller_id.begin(), controller_id.end()}},
                                         {10, {signature.begin(), signature.end()}}});
  const auto verify_key =
      hap_hkdf_sha512(*shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info");
  (void)client.request("POST", "/pair-verify", 2,
                       encode_hap_tlv(std::vector<HapTlv>{
                           {6, {3}}, {5, hap_encrypt(verify_key, "PV-Msg03", proof)}}));
  client.read_key = hap_hkdf_sha512(*shared, "Control-Salt", "Control-Read-Encryption-Key");
  client.write_key = hap_hkdf_sha512(*shared, "Control-Salt", "Control-Write-Encryption-Key");
  (void)client.request("SETUP", "/stream", 3,
                       plist({{"timingPort", PlistValue{std::int64_t(12345)}}}));
  const auto offer =
      PlistValue{PlistValue::Dictionary{{"type", PlistValue{std::int64_t(100)}},
                                        {"streamConnectionID", PlistValue{std::int64_t(91)}},
                                        {"audioFormat", PlistValue{std::int64_t(0x8000)}},
                                        {"audioType", PlistValue{std::string("media")}},
                                        {"audioLatencyMs", PlistValue{std::int64_t(200)}}}};
  const auto audio = decode_binary_plist(client.request(
      "SETUP", "/stream", 4, plist({{"streams", PlistValue{PlistValue::Array{offer}}}})));
  const auto streams = std::get<PlistValue::Array>(
      std::get<PlistValue::Dictionary>(audio.value).at("streams").value);
  const auto data_port = std::get<std::int64_t>(
      std::get<PlistValue::Dictionary>(streams[0].value).at("dataPort").value);
  fixture.collector =
      number(storage ? audio_retention_fixture::header(trace) : read_file(trace), "collectorPid");
  check(fixture.collector > 1, "actual collector missing");
  if (!storage) check(kill(fixture.collector, SIGSTOP) == 0, "actual collector not stoppable");
  const auto audio_key =
      hap_hkdf_sha512(*shared, "DataStream-Salt91", "DataStream-Output-Encryption-Key");
  const int udp = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  target.sin6_port = htons(data_port);
  const auto start = Clock::now();
  std::uint64_t slowest = 0;
  std::string before_exhaustion;
  for (unsigned n = 0; n < 150; ++n) {
    if (storage && n == 50) {
      before_exhaustion = audio_retention_fixture::read(state);
      check(number(before_exhaustion, "acceptedBlocks") >= 20,
            "exhaustion preceded actual accepted PCM baseline");
      check(
          audio_retention_fixture::header(trace).find("18446744073709551615") != std::string::npos,
          "original unavailable values missing before exhaustion");
      scratch->fill();
    }
    const auto time = n * 480;
    std::array<std::uint8_t, 12> header{0x80,
                                        100,
                                        std::uint8_t(n >> 8),
                                        std::uint8_t(n),
                                        std::uint8_t(time >> 24),
                                        std::uint8_t(time >> 16),
                                        std::uint8_t(time >> 8),
                                        std::uint8_t(time),
                                        1,
                                        2,
                                        3,
                                        4};
    const std::vector<std::uint8_t> pcm(1920, 1);
    const auto iv = Client::nonce(n);
    const auto encrypted = hap_encrypt(audio_key, iv, pcm, std::span(header).subspan(4));
    std::vector<std::uint8_t> packet(header.begin(), header.end());
    packet.insert(packet.end(), encrypted.begin(), encrypted.end());
    packet.insert(packet.end(), iv.begin(), iv.end());
    check(sendto(udp, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&target),
                 sizeof(target)) == static_cast<ssize_t>(packet.size()),
          "actual PCM UDP failed");
    if (n % 10 == 0) {
      const auto dispatched = Clock::now();
      (void)client.request("POST", "/feedback", 10 + n);
      slowest =
          std::max(slowest, std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              Clock::now() - dispatched)
                                              .count()));
    }
    std::this_thread::sleep_until(start + std::chrono::milliseconds((n + 1) * 10));
  }
  close(udp);
  const auto progress = storage ? audio_retention_fixture::read(state) : read_file(state);
  check(number(progress, "acceptedBlocks") >= 100 && number(progress, "sampledMonotonicNs") > 0,
        "stalled exporter prevented actual PCM admission/control publication");
  if (storage) {
    check(number(progress, "acceptedBlocks") >= number(before_exhaustion, "acceptedBlocks") + 50 &&
              number(progress, "sampledMonotonicNs") >
                  number(before_exhaustion, "sampledMonotonicNs") &&
              kill(fixture.receiver, 0) == 0,
          "disk exhaustion stalled or replaced original PCM/control owner");
    check(kill(fixture.collector, 0) == -1 && errno == ESRCH,
          "receiver did not retire failed collector while controls remained alive");
  } else {
    check(kill(fixture.collector, SIGCONT) == 0, "actual collector resume failed");
  }
  const auto stopped = fixture.collector;
  (void)client.request("TEARDOWN", "/stream", 200);
  kill(fixture.receiver, SIGTERM);
  int exit = -1;
  check(waitpid(fixture.receiver, &exit, 0) == fixture.receiver && WIFEXITED(exit) &&
            WEXITSTATUS(exit) == 0,
        "actual receiver/collector normal retirement failed");
  fixture.receiver = -1;
  check(kill(stopped, 0) == -1 && errno == ESRCH,
        "actual receiver left a collector after shutdown");
  fixture.collector = -1;
  const auto receipt = storage ? audio_retention_fixture::read(trace) : read_file(trace);
  if (storage) {
    check(receipt.find("\"finished\":true") == std::string::npos,
          "disk failure published a false successful footer");
    const auto log_bytes = audio_retention_fixture::read(fixture.root / "receiver.log");
    check(log_bytes.find("Private audio collector failed error=28") != std::string::npos ||
              (log_bytes.find("Private audio collector failed error=5") != std::string::npos &&
               log_bytes.find("partialBytes=0") == std::string::npos),
          "actual collector lacks exhausted write error or partial-write receipt");
    const auto evidence = audio_storage_fixture::receipts();
    audio_retention_fixture::write_new(evidence / "control.trace.jsonl", receipt);
    audio_retention_fixture::write_new(evidence / "control.receiver.log", log_bytes);
    audio_retention_fixture::write_new(evidence / "control.before.json", before_exhaustion);
    audio_retention_fixture::write_new(evidence / "control.after.json", progress);
    const auto summary =
        "{\"case\":\"encrypted-owner-enospc\",\"finished\":false,"
        "\"coverageComplete\":null,\"probeErrno\":28,\"feedbackReplies\":15,"
        "\"collectorAbsentBeforeTeardown\":true,\"receiverExitedZero\":true,"
        "\"acceptedBefore\":" +
        std::to_string(number(before_exhaustion, "acceptedBlocks")) +
        ",\"acceptedAfter\":" + std::to_string(number(progress, "acceptedBlocks")) +
        ",\"maximumReplyNs\":" + std::to_string(slowest) + "}";
    audio_retention_fixture::write_new(evidence / "control.json", summary + '\n');
    fixture.retire(evidence);
    scratch->retire(evidence);
    check(!std::filesystem::exists(fixture.root) && !std::filesystem::exists(scratch->root),
          "verified control originals survived retirement");
    std::cout << summary << '\n';
  } else {
    check(receipt.find("\"finished\":true") != std::string::npos &&
              number(receipt, "transportDroppedEvents") > 0,
          "actual control-loop diagnostic loss not reported on completed export");
    std::cout << "actual_stopped_collector_feedback_replies=15 accepted_pcm="
              << number(progress, "acceptedBlocks") << " maximum_reply_ns=" << slowest
              << " ipc_dropped=" << number(receipt, "transportDroppedEvents") << '\n';
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
