// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_control_file.hpp>
#include "theme/vehicle_json.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace headunit::audio_control {
namespace {
using JsonValue = theme::opack::Value;
using JsonObject = theme::opack::Dictionary;
constexpr std::size_t maximum_bytes = 2048;
constexpr unsigned maximum_io_turns = 64;
std::atomic<std::uint64_t> temporary_sequence{0};

void require(bool valid, const char* why) {
  if (!valid) throw std::invalid_argument(why);
}
void io(bool valid, const char* why) {
  if (!valid) throw std::runtime_error(why);
}
bool valid_route(std::string_view route) {
  return route.size() == 32 && std::all_of(route.begin(), route.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
const JsonObject& object(const JsonValue& value, std::initializer_list<std::string_view> keys) {
  const auto* fields = std::get_if<JsonObject>(&value.data);
  require(fields && fields->size() == keys.size(), "invalid audio control request fields");
  for (const auto& [key, child] : *fields) {
    (void)child;
    const auto* text = std::get_if<std::string>(&key.data);
    require(text && std::find(keys.begin(), keys.end(), *text) != keys.end(),
            "unknown audio control request field");
  }
  return *fields;
}
const JsonValue& field(const JsonObject& fields, std::string_view key) {
  for (const auto& [name, value] : fields)
    if (std::get<std::string>(name.data) == key) return value;
  throw std::invalid_argument("missing audio control request field");
}
std::string_view text(const JsonValue& value) {
  const auto* result = std::get_if<std::string>(&value.data);
  require(result != nullptr, "audio control field must be a JSON string");
  return *result;
}
std::int64_t integer(const JsonValue& value) {
  const auto* result = std::get_if<std::int64_t>(&value.data);
  require(result != nullptr, "audio control field must be a JSON integer");
  return *result;
}
bool boolean(const JsonValue& value) {
  const auto* result = std::get_if<bool>(&value.data);
  require(result != nullptr, "audio control field must be a JSON boolean");
  return *result;
}
std::uint64_t sequence(const JsonValue& value) {
  const auto number = text(value);
  require(
      !number.empty() && number.size() <= 20 && number[0] >= '1' && number[0] <= '9' &&
          std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }),
      "audio control request sequence must be a canonical positive decimal string");
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result);
  require(parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size(),
          "audio control request sequence exceeds uint64");
  return result;
}
std::string quote_ascii(std::string_view value) {
  std::string result = "\"";
  for (const char c : value) {
    if (c == '"' || c == '\\') result += '\\';
    result += c;
  }
  result += '"';
  return result;
}

struct File {
  int fd = -1;
  explicit File(int value = -1) : fd(value) {}
  ~File() {
    if (fd >= 0) ::close(fd);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  int release() { return std::exchange(fd, -1); }
};
bool same_inode(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
bool private_directory(const struct stat& info, unsigned uid) {
  return S_ISDIR(info.st_mode) && info.st_uid == uid && (info.st_mode & 07777) == 0700;
}
bool private_file(const struct stat& info, unsigned uid) {
  return S_ISREG(info.st_mode) && info.st_uid == uid && (info.st_mode & 07777) == 0600 &&
         info.st_nlink == 1 && info.st_size >= 0 &&
         info.st_size <= static_cast<off_t>(maximum_bytes);
}
bool stable_file(const struct stat& before, const struct stat& after) {
#ifdef __APPLE__
  const auto& before_m = before.st_mtimespec;
  const auto& after_m = after.st_mtimespec;
  const auto& before_c = before.st_ctimespec;
  const auto& after_c = after.st_ctimespec;
#else
  const auto& before_m = before.st_mtim;
  const auto& after_m = after.st_mtim;
  const auto& before_c = before.st_ctim;
  const auto& after_c = after.st_ctim;
#endif
  return same_inode(before, after) && before.st_size == after.st_size &&
         before_m.tv_sec == after_m.tv_sec && before_m.tv_nsec == after_m.tv_nsec &&
         before_c.tv_sec == after_c.tv_sec && before_c.tv_nsec == after_c.tv_nsec;
}
class PrivatePath {
 public:
  std::string parent, name;
  File directory;
  struct stat identity{};
  const unsigned uid;
  PrivatePath(const std::string& path, unsigned owner) : uid(owner) {
    const std::filesystem::path requested(path);
    require(!path.empty() && path.size() <= 4000 && path.find('\0') == path.npos &&
                requested.is_absolute(),
            "audio control path must be a bounded absolute path");
    for (const auto& component : requested)
      require(component != "." && component != "..",
              "audio control path cannot contain dot traversal");
    parent = requested.parent_path().string();
    name = requested.filename().string();
    require(!name.empty() && name.size() <= 255, "audio control path must name one bounded file");
    struct stat observed{};
    require(::lstat(parent.c_str(), &observed) == 0 && private_directory(observed, uid),
            "audio control parent must be a real owned 0700 directory");
    directory.fd =
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    io(directory.fd >= 0, "cannot open audio control parent directory");
    require(::fstat(directory.fd, &identity) == 0 && private_directory(identity, uid) &&
                same_inode(observed, identity),
            "audio control parent changed while opening");
  }
  void verify() const {
    struct stat named{}, opened{};
    require(::lstat(parent.c_str(), &named) == 0 && private_directory(named, uid) &&
                same_inode(identity, named) && ::fstat(directory.fd, &opened) == 0 &&
                private_directory(opened, uid) && same_inode(identity, opened),
            "audio control private parent changed");
  }
  std::optional<struct stat> inspect(const std::string& filename) const {
    struct stat result{};
    if (::fstatat(directory.fd, filename.c_str(), &result, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) return std::nullopt;
      throw std::runtime_error("cannot inspect private audio control file");
    }
    require(private_file(result, uid),
            "audio control file must be owned single-link 0600 regular data");
    return result;
  }
};
struct Temporary {
  const PrivatePath& path;
  std::string name;
  struct stat identity{};
  bool installed = false;
  ~Temporary() {
    if (!installed) {
      struct stat current{};
      if (::fstatat(path.directory.fd, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
          same_inode(identity, current))
        ::unlinkat(path.directory.fd, name.c_str(), 0);
    }
  }
};
void write_all(int fd, std::string_view bytes) {
  std::size_t offset = 0;
  for (unsigned turns = 0; offset < bytes.size() && turns < maximum_io_turns; ++turns) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    io(count > 0, "cannot write private audio control state");
    offset += static_cast<std::size_t>(count);
  }
  io(offset == bytes.size(), "private audio control state write exceeded bounded turns");
}
}  // namespace

Request decode_request(std::string_view json) {
  require(!json.empty() && json.size() <= maximum_bytes && json.find('\0') == json.npos,
          "audio control request must be nonempty and at most 2048 bytes");
  const auto parsed = theme::vehicle::detail::Json(json).parse();
  const auto& fields = object(parsed, {"version", "route", "sequence", "volumePercent", "muted"});
  require(integer(field(fields, "version")) == 1, "unsupported audio control request version");
  const auto route = text(field(fields, "route"));
  require(valid_route(route), "audio control route must be 32 lowercase hexadecimal characters");
  const auto volume = integer(field(fields, "volumePercent"));
  require(volume >= 0 && volume <= 100, "audio control volume must be 0..100 percent");
  return {std::string(route),
          {sequence(field(fields, "sequence")),
           {static_cast<unsigned>(volume), boolean(field(fields, "muted"))}}};
}

std::string encode_state(const State& state, std::uint64_t monotonic_ns) {
  require(monotonic_ns != 0, "audio control state update must have a positive monotonic date");
  require(state.control.available ? valid_route(state.route) : state.route.empty(),
          "audio control state route must match availability");
  require(state.control.requested.level.volume_percent <= 100 &&
              state.control.applied.level.volume_percent <= 100 &&
              state.control.applied.sequence <= state.control.requested.sequence &&
              (state.control.applied.sequence != state.control.requested.sequence ||
               state.control.applied.level == state.control.requested.level),
          "incoherent audio control requested/applied state");
  require(state.error.size() <= 128 &&
              std::all_of(state.error.begin(), state.error.end(),
                          [](unsigned char c) { return c >= 0x20 && c <= 0x7e; }),
          "audio control state error must be printable ASCII up to 128 bytes");
  const auto flag = [](bool value) {
    return value ? "true" : "false";
  };
  std::string json =
      "{\"version\":1,\"updatedMonotonicNs\":\"" + std::to_string(monotonic_ns) +
      "\",\"available\":" + flag(state.control.available) +
      ",\"route\":" + quote_ascii(state.route) + ",\"acceptedSequence\":\"" +
      std::to_string(state.control.requested.sequence) + "\",\"appliedSequence\":\"" +
      std::to_string(state.control.applied.sequence) + "\",\"requestedVolumePercent\":" +
      std::to_string(state.control.requested.level.volume_percent) +
      ",\"requestedMuted\":" + flag(state.control.requested.level.muted) +
      ",\"volumePercent\":" + std::to_string(state.control.applied.level.volume_percent) +
      ",\"muted\":" + flag(state.control.applied.level.muted) +
      ",\"error\":" + quote_ascii(state.error);
  if (const auto& output = state.control.output) {
    require(state.control.available && output->sampled_ns > 0 && output->sampled_ns <= monotonic_ns,
            "output observation requires a current route and original nonfuture date");
    json += ",\"output\":{\"sampledMonotonicNs\":\"" + std::to_string(output->sampled_ns) +
            "\",\"acceptedBlocks\":\"" + std::to_string(output->accepted_blocks) +
            "\",\"discontinuities\":\"" + std::to_string(output->discontinuities) +
            "\",\"underruns\":\"" + std::to_string(output->underruns) +
            "\",\"starved\":" + flag(output->starved);
    if (const auto& timing = output->timing) {
      json += ",\"timing\":{\"scheduledAheadNs\":\"" + std::to_string(timing->scheduled_ahead_ns) +
              "\",\"maximumOwnerGapNs\":\"" + std::to_string(timing->maximum_owner_gap_ns) +
              "\",\"lastUnderrunOwnerGapNs\":\"" +
              std::to_string(timing->last_underrun_owner_gap_ns) + "\",\"lastUnderrunLateNs\":\"" +
              std::to_string(timing->last_underrun_late_ns) + "\",\"queuedBytes\":\"" +
              std::to_string(timing->queued_bytes) + "\",\"lastUnderrunQueuedBytes\":\"" +
              std::to_string(timing->last_underrun_queued_bytes) + "\"}";
    }
    json += "}";
  }
  json += "}";
  require(json.size() <= maximum_bytes, "audio control state exceeds 2048 bytes");
  return json;
}

std::optional<Request> read_private_request(const std::string& path, unsigned owner_uid) {
  PrivatePath requested(path, owner_uid);
  requested.verify();
  const auto observed = requested.inspect(requested.name);
  if (!observed) return std::nullopt;
  File input(::openat(requested.directory.fd, requested.name.c_str(),
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  io(input.fd >= 0, "cannot open private audio control request");
  struct stat opened{};
  require(::fstat(input.fd, &opened) == 0 && private_file(opened, owner_uid) &&
              stable_file(*observed, opened),
          "private audio control request changed while opening");
  std::array<char, maximum_bytes + 1> bytes{};
  std::size_t size = 0;
  bool eof = false;
  for (unsigned turns = 0; size < bytes.size() && turns < maximum_io_turns; ++turns) {
    const auto count = ::read(input.fd, bytes.data() + size, bytes.size() - size);
    if (count < 0 && errno == EINTR) continue;
    io(count >= 0, "cannot read private audio control request");
    if (count == 0) {
      eof = true;
      break;
    }
    size += static_cast<std::size_t>(count);
  }
  struct stat after{};
  require(eof && size == static_cast<std::size_t>(opened.st_size) &&
              ::fstat(input.fd, &after) == 0 && private_file(after, owner_uid) &&
              stable_file(opened, after),
          "private audio control request was oversized, changed or incomplete");
  requested.verify();
  return decode_request({bytes.data(), size});
}

void write_private_state(const std::string& path, unsigned owner_uid, const State& state,
                         std::uint64_t monotonic_ns) {
  const auto bytes = encode_state(state, monotonic_ns);
  PrivatePath target(path, owner_uid);
  target.verify();
  (void)target.inspect(target.name);
  File output;
  std::string temporary_name;
  for (unsigned attempts = 0; attempts < maximum_io_turns; ++attempts) {
    temporary_name = ".audio-state-" + std::to_string(::getpid()) + "-" +
                     std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed)) +
                     ".tmp";
    if (temporary_name == target.name) continue;
    output.fd = ::openat(target.directory.fd, temporary_name.c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (output.fd >= 0) break;
    io(errno == EEXIST, "cannot create private audio control state temporary");
  }
  io(output.fd >= 0, "private audio control state temporary attempts exceeded bound");
  struct stat created{};
  // Without an inode identity, cleanup cannot prove a named file is still ours.
  // Never unlink a possible replacement merely because inspection failed.
  io(::fstat(output.fd, &created) == 0, "cannot inspect audio control state temporary");
  Temporary temporary{target, temporary_name, created};
  io(::fchmod(output.fd, 0600) == 0, "cannot secure audio control state temporary");
  struct stat private_created{};
  require(::fstat(output.fd, &private_created) == 0 && private_file(private_created, owner_uid) &&
              same_inode(created, private_created),
          "invalid owner or mode on audio control state temporary");
  write_all(output.fd, bytes);
  // State is an ephemeral heartbeat, not a durable preference. Atomic rename
  // gives readers complete bytes; forcing a disk flush on every control poll
  // would unnecessarily stall the receiver and cannot establish playback.
  struct stat written{};
  const auto named_temporary = target.inspect(temporary_name);
  require(::fstat(output.fd, &written) == 0 && private_file(written, owner_uid) &&
              written.st_size == static_cast<off_t>(bytes.size()) && same_inode(created, written) &&
              named_temporary && stable_file(written, *named_temporary),
          "private audio control state temporary changed");
  target.verify();
  (void)target.inspect(target.name);
  io(::close(output.release()) == 0, "cannot close private audio control state temporary");
  io(::renameat(target.directory.fd, temporary_name.c_str(), target.directory.fd,
                target.name.c_str()) == 0,
     "cannot atomically replace private audio control state");
  temporary.installed = true;
}

}  // namespace headunit::audio_control
