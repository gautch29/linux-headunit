// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_control_file.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
namespace audio = headunit::audio_control;
using namespace std::chrono_literals;
#define CHECK(condition)                                                                        \
  do {                                                                                          \
    if (!(condition)) throw std::runtime_error("audio control file check failed: " #condition); \
  } while (false)
const std::string route = "0123456789abcdef0123456789abcdef";
const unsigned uid = static_cast<unsigned>(::getuid());

void rejects(const auto& operation) {
  try {
    operation();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error("invalid audio control exchange was accepted");
}
std::string request(std::string_view number = "1", unsigned volume = 50, bool muted = false) {
  return "{\"version\":1,\"route\":\"" + route + "\",\"sequence\":\"" + std::string(number) +
         "\",\"volumePercent\":" + std::to_string(volume) +
         ",\"muted\":" + (muted ? "true" : "false") + "}";
}
std::string replace(std::string json, std::string_view from, std::string_view to) {
  const auto at = json.find(from);
  CHECK(at != json.npos);
  json.replace(at, from.size(), to);
  return json;
}
audio::State state(std::uint64_t accepted = 2, std::uint64_t applied = 1) {
  return {route, {true, {accepted, {25, true}}, {applied, {60, false}}}, {}};
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "headunit-audio-file-XXXXXX").string();
    CHECK(::mkdtemp(pattern.data()) != nullptr);
    path = pattern;
    CHECK(::chmod(path.c_str(), 0700) == 0);
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::string file(std::string_view name = "request.json") const { return (path / name).string(); }
};
struct File {
  int fd = -1;
  explicit File(int value) : fd(value) { CHECK(fd >= 0); }
  ~File() { ::close(fd); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
};
void write(const std::string& path, std::string_view bytes, mode_t mode = 0600) {
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
  }
  CHECK(::chmod(path.c_str(), mode) == 0);
}
std::string read_fd(int fd) {
  struct stat info{};
  CHECK(::fstat(fd, &info) == 0 && S_ISREG(info.st_mode));
  CHECK(info.st_size >= 0 && info.st_size <= 2048);
  std::string bytes;
  std::array<char, 2049> block{};
  for (unsigned turn = 0; turn < 64; ++turn) {
    const auto count = ::read(fd, block.data(), block.size());
    CHECK(count >= 0);
    if (!count) return bytes;
    bytes.append(block.data(), static_cast<std::size_t>(count));
    CHECK(bytes.size() <= 2048);
  }
  throw std::runtime_error("test file read exceeded bound");
}
std::string read(const std::string& path) {
  File input(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  return read_fd(input.fd);
}
void assert_no_temporaries(const Directory& directory, unsigned expected = 1) {
  unsigned entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
    CHECK(!entry.path().filename().string().starts_with(".audio-state-"));
    ++entries;
  }
  CHECK(entries == expected);
}

void request_wire_and_protocol_independence() {
  const auto parsed = audio::decode_request(request());
  CHECK(parsed.route == route && parsed.command.sequence == 1 &&
        parsed.command.level.volume_percent == 50 && !parsed.command.level.muted);
  const auto maximum = audio::decode_request(request("18446744073709551615", 100, true));
  CHECK(maximum.command.sequence == std::numeric_limits<std::uint64_t>::max() &&
        maximum.command.level == (audio::Level{100, true}));
  CHECK(audio::decode_request(request("1", 0)).command.level.volume_percent == 0);
  // A codec has no implicit lease/clock/replay state. Parent route ownership
  // and Mailbox sequence progression decide whether an otherwise valid request
  // is stale; rereading an unchanged request cannot apply or renew anything.
  CHECK(audio::decode_request(request("10")) == audio::decode_request(request("10")));
  CHECK(audio::decode_request(request("1")).command.sequence == 1);
  audio::Mailbox mailbox;
  mailbox.open();
  CHECK(mailbox.request(parsed.command) == audio::RequestResult::accepted);
  CHECK(mailbox.request(parsed.command) == audio::RequestResult::stale);
  CHECK(mailbox.snapshot().applied.sequence == 0);
  mailbox.close();
}
void malformed_request_shape_and_types() {
  const auto valid = request();
  for (const auto& input :
       {std::string{}, std::string("[]"), std::string("null"), std::string("{}"), valid + "{}",
        valid + "null", replace(valid, "\"version\":1", "\"version\":1,\"extra\":0"),
        replace(valid, "\"version\":1", "\"version\":1,\"version\":1"),
        replace(valid, "\"version\":1", "\"version\":1,\"vers\\u0069on\":1"),
        replace(valid, "\"muted\":false", "\"muted\":false,\"mut\\u0065d\":true"),
        replace(valid, "\"muted\":false", "\"missing\":false"),
        replace(valid, "\"version\":1", "\"version\":01"), valid.substr(0, valid.size() - 1)})
    rejects([&] { audio::decode_request(input); });
  for (const auto version : {"0", "2", "true", "1.0", "1e0", "\"1\"", "null"})
    rejects([&] {
      audio::decode_request(replace(valid, "\"version\":1", "\"version\":" + std::string(version)));
    });
  for (const auto number : {"", "0", "00", "01", "+1", "-1", "1.0", "1e0", " 1", "1 ",
                            "18446744073709551616", "99999999999999999999999"})
    rejects([&] { audio::decode_request(request(number)); });
  for (const auto number : {"1", "1.0", "true", "null", "[]"})
    rejects([&] {
      audio::decode_request(
          replace(valid, "\"sequence\":\"1\"", "\"sequence\":" + std::string(number)));
    });
  for (const auto volume : {"-1", "101", "true", "50.0", "5e1", "\"50\"", "null", "1e400"})
    rejects([&] {
      audio::decode_request(
          replace(valid, "\"volumePercent\":50", "\"volumePercent\":" + std::string(volume)));
    });
  for (const auto muted : {"0", "1", "\"false\"", "null", "[]"})
    rejects([&] {
      audio::decode_request(replace(valid, "\"muted\":false", "\"muted\":" + std::string(muted)));
    });
  for (const auto& bad_route :
       {std::string{}, route.substr(1), route + '0', replace(route, "a", "A"),
        replace(route, "a", "g"), replace(route, "a", " "), replace(route, "a", "\\u0000"),
        replace(route, "a", "\\u00e9")})
    rejects([&] { audio::decode_request(replace(valid, route, bad_route)); });
  rejects([&] { audio::decode_request(replace(valid, "\"" + route + "\"", "null")); });
}
void request_size_and_atomic_validation() {
  auto full = request();
  full.append(2048 - full.size(), ' ');
  CHECK(audio::decode_request(full).command.sequence == 1);
  full += ' ';
  rejects([&] { audio::decode_request(full); });
  auto nul = request();
  nul += '\0';
  rejects([&] { audio::decode_request(nul); });
  auto invalid_utf8 = request();
  invalid_utf8[invalid_utf8.find(route)] = char(0xff);
  rejects([&] { audio::decode_request(invalid_utf8); });
  auto last = audio::decode_request(request("10"));
  rejects([&] {
    last = audio::decode_request(replace(request("11"), "\"muted\":false", "\"muted\":null"));
  });
  CHECK(last.command.sequence == 10 && !last.command.level.muted);
}
void state_wire_keeps_requested_and_applied_distinct() {
  const auto value = state();
  const auto encoded = audio::encode_state(value, 123);
  CHECK(
      encoded ==
      "{\"version\":1,\"updatedMonotonicNs\":\"123\",\"available\":true,\"route\":\"" + route +
          "\",\"acceptedSequence\":\"2\",\"appliedSequence\":\"1\",\"requestedVolumePercent\":25," +
          "\"requestedMuted\":true,\"volumePercent\":60,\"muted\":false,\"error\":\"\"}");
  const audio::State initial{route, {true, {}, {}}, {}};
  CHECK(audio::encode_state(initial, 1)
            .find("\"acceptedSequence\":\"0\",\"appliedSequence\":\"0\"") != std::string::npos);
  auto unavailable = value;
  unavailable.control.available = false;
  unavailable.route.clear();
  const auto closed = audio::encode_state(unavailable, std::numeric_limits<std::uint64_t>::max());
  CHECK(closed.find("\"updatedMonotonicNs\":\"18446744073709551615\"") != closed.npos &&
        closed.find("\"available\":false,\"route\":\"\"") != closed.npos &&
        closed.find("\"acceptedSequence\":\"2\",\"appliedSequence\":\"1\"") != closed.npos);
  auto escaped = value;
  escaped.error = "Sink said \"not ready\" \\ retry";
  CHECK(audio::encode_state(escaped, 1)
            .find("\"error\":\"Sink said \\\"not ready\\\" \\\\ retry\"") != std::string::npos);
  escaped.error = std::string(128, '~');
  CHECK(audio::encode_state(escaped, 1).size() <= 2048);
  auto progress = value;
  progress.control.output =
      audio::OutputProgress{100, std::numeric_limits<std::uint64_t>::max(), 3, 2, true};
  CHECK(audio::encode_state(progress, 123)
            .find("\"sampledMonotonicNs\":\"100\",\"acceptedBlocks\":\"18446744073709551615\"") !=
        std::string::npos);
  progress.control.output->timing =
      audio::OutputTiming{70000000, 50000000, 40000000, 1000000, 1920, 3840};
  CHECK(
      audio::encode_state(progress, 123)
          .find("\"timing\":{\"scheduledAheadNs\":\"70000000\",\"maximumOwnerGapNs\":\"50000000\","
                "\"lastUnderrunOwnerGapNs\":\"40000000\",\"lastUnderrunLateNs\":\"1000000\","
                "\"queuedBytes\":\"1920\",\"lastUnderrunQueuedBytes\":\"3840\"}") !=
      std::string::npos);
  rejects([&] { audio::encode_state(progress, 99); });
  progress.control.output->sampled_ns = 0;
  rejects([&] { audio::encode_state(progress, 123); });
  progress.control.output->sampled_ns = 100;
  progress.control.available = false;
  progress.route.clear();
  rejects([&] { audio::encode_state(progress, 123); });
}
void invalid_state_cannot_replace_valid_state() {
  const auto good = state();
  std::array<audio::State, 8> invalid;
  invalid.fill(good);
  invalid[0].route.clear();
  invalid[1].control.available = false;
  invalid[2].control.requested.level.volume_percent = 101;
  invalid[3].control.applied.level.volume_percent = std::numeric_limits<unsigned>::max();
  invalid[4].control.applied.sequence = 3;
  invalid[5].control.applied.sequence = 2;  // Equal sequence must describe the same command.
  invalid[6].error = std::string(129, 'x');
  invalid[7].error = "not\nprintable";
  Directory directory;
  const auto path = directory.file("state.json");
  audio::write_private_state(path, uid, good, 100);
  const auto original = read(path);
  for (const auto& bad : invalid) {
    rejects([&] { audio::encode_state(bad, 1); });
    rejects([&] { audio::write_private_state(path, uid, bad, 101); });
    CHECK(read(path) == original);
  }
  for (const auto& error :
       {std::string(1, '\0'), std::string(1, '\x7f'), std::string(1, char(0xff))}) {
    auto bad = good;
    bad.error = error;
    rejects([&] { audio::encode_state(bad, 1); });
  }
  rejects([&] { audio::write_private_state(path, uid, good, 0); });
  CHECK(read(path) == original);
  assert_no_temporaries(directory);
}
void private_request_read_and_bounds() {
  Directory directory;
  const auto path = directory.file();
  CHECK(!audio::read_private_request(path, uid));
  write(path, request("10"));
  const auto first = audio::read_private_request(path, uid);
  CHECK(first && first->command.sequence == 10);
  CHECK(audio::read_private_request(path, uid) == first);
  const std::array<timespec, 2> ancient{{{1, 0}, {1, 0}}};
  CHECK(::utimensat(AT_FDCWD, path.c_str(), ancient.data(), 0) == 0);
  CHECK(audio::read_private_request(path, uid) ==
        first);  // Parent route/sequence, not file age, decides acceptance.
  auto bounded = request("1");
  bounded.append(2048 - bounded.size(), ' ');
  write(path, bounded);
  CHECK(audio::read_private_request(path, uid)->command.sequence ==
        1);  // Codec has no hidden replay state.
  bounded += ' ';
  write(path, bounded);
  rejects([&] { audio::read_private_request(path, uid); });
  write(path, "");
  rejects([&] { audio::read_private_request(path, uid); });
  write(path, request() + "{}");
  rejects([&] { audio::read_private_request(path, uid); });
}
void malicious_files_are_rejected_without_following_or_waiting() {
  Directory directory;
  const auto path = directory.file();
  write(path, request(), 0644);
  rejects([&] { audio::read_private_request(path, uid); });
  for (const auto mode : {0400, 0700, 0660, 0604, 04600}) {
    CHECK(::chmod(path.c_str(), mode) == 0);
    rejects([&] { audio::read_private_request(path, uid); });
    rejects([&] { audio::write_private_state(path, uid, state(), 1); });
  }
  CHECK(::chmod(path.c_str(), 0600) == 0);
  const auto wrong_uid = uid == std::numeric_limits<unsigned>::max() ? uid - 1 : uid + 1;
  rejects([&] { audio::read_private_request(path, wrong_uid); });
  rejects([&] { audio::write_private_state(path, wrong_uid, state(), 1); });
  const auto linked = directory.file("hardlink.json");
  CHECK(::link(path.c_str(), linked.c_str()) == 0);
  rejects([&] { audio::read_private_request(path, uid); });
  rejects([&] { audio::write_private_state(path, uid, state(), 1); });
  CHECK(read(linked) == request());
  CHECK(::unlink(linked.c_str()) == 0 && ::unlink(path.c_str()) == 0);
  const auto victim = directory.file("victim.json");
  write(victim, request("7"));
  CHECK(::symlink(victim.c_str(), path.c_str()) == 0);
  rejects([&] { audio::read_private_request(path, uid); });
  rejects([&] { audio::write_private_state(path, uid, state(), 1); });
  CHECK(read(victim) == request("7"));
  CHECK(::unlink(path.c_str()) == 0);
  CHECK(::mkdir(path.c_str(), 0700) == 0);
  rejects([&] { audio::read_private_request(path, uid); });
  rejects([&] { audio::write_private_state(path, uid, state(), 1); });
  CHECK(::rmdir(path.c_str()) == 0);
  CHECK(::mkfifo(path.c_str(), 0600) == 0);
  const auto began = std::chrono::steady_clock::now();
  rejects([&] { audio::read_private_request(path, uid); });
  rejects([&] { audio::write_private_state(path, uid, state(), 1); });
  CHECK(std::chrono::steady_clock::now() - began < 1s);
  CHECK(::unlink(path.c_str()) == 0);
  File socket(::socket(AF_UNIX, SOCK_DGRAM, 0));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  CHECK(path.size() < sizeof(address.sun_path));
  std::copy(path.begin(), path.end(), address.sun_path);
  CHECK(::bind(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
  CHECK(::chmod(path.c_str(), 0600) == 0);
  rejects([&] { audio::read_private_request(path, uid); });
  rejects([&] { audio::write_private_state(path, uid, state(), 1); });
  CHECK(read(victim) == request("7"));
}
void private_parent_and_path_contract() {
  Directory directory;
  const auto path = directory.file();
  write(path, request());
  for (const auto mode : {0755, 0750, 0707, 01700}) {
    CHECK(::chmod(directory.path.c_str(), mode) == 0);
    rejects([&] { audio::read_private_request(path, uid); });
    rejects([&] { audio::write_private_state(path, uid, state(), 1); });
  }
  CHECK(::chmod(directory.path.c_str(), 0700) == 0);
  Directory outside;
  const auto alias = outside.file("alias");
  CHECK(::symlink(directory.path.c_str(), alias.c_str()) == 0);
  rejects([&] { audio::read_private_request(alias + "/request.json", uid); });
  rejects([&] { audio::write_private_state(alias + "/state.json", uid, state(), 1); });
  const auto missing_parent = outside.file("missing/request.json");
  rejects([&] { audio::read_private_request(missing_parent, uid); });
  for (const auto& bad :
       {std::string{}, std::string("relative.json"), directory.path.string() + '/',
        directory.path.string() + "/./request.json", directory.path.string() + "/../request.json",
        path + std::string(1, '\0'), directory.path.string() + '/' + std::string(256, 'x'),
        std::string(4001, '/')}) {
    rejects([&] { audio::read_private_request(bad, uid); });
    rejects([&] { audio::write_private_state(bad, uid, state(), 1); });
  }
  CHECK(read(path) == request());
  CHECK(!std::filesystem::exists(directory.file("state.json")));
}
void atomic_replacement_preserves_open_reader() {
  Directory directory;
  const auto path = directory.file("state.json");
  audio::write_private_state(path, uid, state(), 100);
  File original(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  struct stat before{};
  CHECK(::fstat(original.fd, &before) == 0);
  const auto old_bytes = audio::encode_state(state(), 100);
  auto next = state(3, 3);
  next.control.applied.level = next.control.requested.level;
  audio::write_private_state(path, uid, next, 101);
  struct stat after{};
  CHECK(::lstat(path.c_str(), &after) == 0);
  CHECK(S_ISREG(after.st_mode) && (after.st_mode & 07777) == 0600 && after.st_uid == uid &&
        after.st_nlink == 1);
  CHECK(before.st_dev != after.st_dev || before.st_ino != after.st_ino);
  CHECK(read_fd(original.fd) == old_bytes && read(path) == audio::encode_state(next, 101));
  assert_no_temporaries(directory);
  // Replacement does not inherit a permissive umask and never truncates the
  // old inode, even when the destination already exists.
  const auto previous_mask = ::umask(0000);
  try {
    audio::write_private_state(path, uid, next, 102);
  } catch (...) {
    ::umask(previous_mask);
    throw;
  }
  ::umask(previous_mask);
  CHECK(::lstat(path.c_str(), &after) == 0 && (after.st_mode & 07777) == 0600);
}
void concurrent_readers_observe_only_complete_states() {
  Directory directory;
  const auto path = directory.file("state.json");
  auto a = state(1, 1);
  a.control.applied.level = a.control.requested.level;
  auto b = a;
  b.control.requested = {2, {90, false}};
  b.control.applied = b.control.requested;
  const auto first = audio::encode_state(a, 10), second = audio::encode_state(b, 20);
  audio::write_private_state(path, uid, a, 10);
  struct Readers {
    std::atomic<bool> stop{false}, failed{false};
    std::array<std::atomic<unsigned>, 2> counts{};
    std::atomic<unsigned> seen{0};
    std::array<std::thread, 2> threads;
    ~Readers() {
      stop = true;
      for (auto& thread : threads)
        if (thread.joinable()) thread.join();
    }
  } readers;
  for (unsigned i = 0; i < readers.threads.size(); ++i)
    readers.threads[i] = std::thread([&, i] {
      try {
        while (!readers.stop) {
          const auto bytes = read(path);
          if (bytes == first)
            readers.seen.fetch_or(1);
          else if (bytes == second)
            readers.seen.fetch_or(2);
          else
            throw std::runtime_error("reader observed a partial or unexpected state");
          ++readers.counts[i];
          std::this_thread::yield();
        }
      } catch (...) {
        readers.failed = true;
      }
    });
  const auto until = [&](const auto& predicate) {
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
      CHECK(!readers.failed && std::chrono::steady_clock::now() < end);
      std::this_thread::yield();
    }
  };
  until([&] { return readers.counts[0] >= 2 && readers.counts[1] >= 2; });
  audio::write_private_state(path, uid, b, 20);
  until([&] { return readers.seen == 3; });
  for (unsigned turn = 0; turn < 64; ++turn)
    audio::write_private_state(path, uid, turn % 2 ? a : b, turn % 2 ? 10 : 20);
  readers.stop = true;
  for (auto& thread : readers.threads)
    thread.join();
  CHECK(!readers.failed && readers.seen == 3 && readers.counts[0] >= 2 && readers.counts[1] >= 2);
  assert_no_temporaries(directory);
}
}  // namespace

int main() try {
  request_wire_and_protocol_independence();
  malformed_request_shape_and_types();
  request_size_and_atomic_validation();
  state_wire_keeps_requested_and_applied_distinct();
  invalid_state_cannot_replace_valid_state();
  private_request_read_and_bounds();
  malicious_files_are_rejected_without_following_or_waiting();
  private_parent_and_path_contract();
  atomic_replacement_preserves_open_reader();
  concurrent_readers_observe_only_complete_states();
  std::cout << "Audio control files: 10 codec/private-file/atomic-replacement cases passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
