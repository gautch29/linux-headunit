// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_controller_io.hpp>
#include <headunit/camera_control.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
namespace c = headunit::camera;
namespace fs = std::filesystem;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F&& action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "unsafe camera controller file operation accepted");
}
struct Directory {
  fs::path path;
  Directory() {
    char name[] = "/tmp/headunit-camera-controller-XXXXXX";
    check(::mkdtemp(name) != nullptr, "test directory creation failed");
    path = name;
    check(::chmod(path.c_str(), 0700) == 0, "test directory permissions failed");
  }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  std::string file(std::string_view name) const { return (path / name).string(); }
};
struct File {
  int fd = -1;
  explicit File(int value) : fd(value) { check(fd >= 0, "test file open failed"); }
  ~File() {
    if (fd >= 0) ::close(fd);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
};
void write(const std::string& path, std::string_view bytes, mode_t mode = 0600) {
  // The fixture deliberately installs read-only modes in preceding cases.
  // Reset that test-owned regular file before creating the next bad snapshot.
  if (fs::exists(path))
    check(::chmod(path.c_str(), 0600) == 0, "test file write permissions reset failed");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), std::streamsize(bytes.size()));
    check(bool(out), "test file write failed");
  }
  check(::chmod(path.c_str(), mode) == 0, "test file mode failed");
}
std::string read_fd(int fd) {
  std::array<char, 256> bytes{};
  const auto size = ::read(fd, bytes.data(), bytes.size());
  check(size >= 0 && size < ssize_t(bytes.size()), "test file read failed");
  return {bytes.data(), std::size_t(size)};
}
std::string read(const std::string& path) {
  File file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  return read_fd(file.fd);
}
void private_endpoint(const std::string& path, std::size_t size) {
  struct stat s{};
  check(::lstat(path.c_str(), &s) == 0 && S_ISREG(s.st_mode) && s.st_uid == ::getuid() &&
            (s.st_mode & 07777) == 0600 && s.st_nlink == 1 && s.st_size == off_t(size),
        "published file is not bounded private single-link data");
}
void no_temporaries(const Directory& dir) {
  for (const auto& entry : fs::directory_iterator(dir.path))
    check(!entry.path().filename().string().starts_with(".headunit-camera-"),
          "publication left a temporary file");
}
int child(const auto& action) {
  const auto pid = ::fork();
  check(pid >= 0, "test fork failed");
  if (!pid) {
    try {
      action();
      ::_exit(0);
    } catch (...) {
      ::_exit(91);
    }
  }
  int status = 0;
  check(::waitpid(pid, &status, 0) == pid && WIFEXITED(status),
        "child operation did not terminate");
  return WEXITSTATUS(status);
}
std::uint64_t now_ns() {
  return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count());
}
struct RunningChild {
  pid_t pid = -1;
  ~RunningChild() {
    if (pid > 0) {
      (void)::kill(pid, SIGKILL);
      int code = 0;
      (void)::waitpid(pid, &code, 0);
    }
  }
  void finish() {
    int code = 0;
    const auto waited = ::waitpid(pid, &code, 0);
    pid = -1;
    check(waited > 0 && WIFEXITED(code) && WEXITSTATUS(code) == 0,
          "concurrent publisher did not finish successfully");
  }
};
void bounded_temporary_collisions() {
  // Run before the first publication: exhaust one bounded O_EXCL attempt set
  // with existing data and prove it neither truncates nor unlinks collisions.
  Directory dir;
  for (unsigned i = 0; i < 64; ++i)
    write(dir.file(".headunit-camera-" + std::to_string(::getpid()) + "-" + std::to_string(i) +
                   ".tmp"),
          "collision");
  const auto began = std::chrono::steady_clock::now();
  rejects([&] { c::RequestPublisher writer(dir.file("request"), dir.file("owner")); });
  check(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(500) &&
            !fs::exists(dir.file("request")),
        "temporary collisions blocked or installed an incomplete request");
  for (unsigned i = 0; i < 64; ++i)
    check(read(dir.file(".headunit-camera-" + std::to_string(::getpid()) + "-" + std::to_string(i) +
                        ".tmp")) == "collision",
          "temporary collision was truncated or removed");
  // The failed constructor released its lock; a later successful owner uses a
  // new temporary and starts OFF without touching those existing files.
  c::RequestPublisher writer(dir.file("request"), dir.file("owner"));
  check(read(dir.file("request")) == "off\n", "collision recovery failed");
}
void request_roundtrip_and_atomicity() {
  Directory dir;
  const auto request = dir.file("request"), owner = dir.file("owner");
  c::ActivationControl reader(request);
  {
    c::RequestPublisher writer(request, owner);
    check(read(request) == "off\n", "request did not start OFF");
    private_endpoint(request, 4);
    private_endpoint(owner, 0);
    File held(::open(request.c_str(), O_RDONLY | O_CLOEXEC));
    writer.publish(true, 1500000000);
    check(read(request) == "on 1500000000\n", "timed request wire format changed");
    check(read_fd(held.fd) == "off\n", "request changed a retained inode in place");
    check(reader.poll(1000000000), "real timed activation reader rejected the publisher");
    check(!reader.poll(1500000000), "request stayed active at its original deadline");
    rejects([&] { writer.publish(true); });
    writer.publish(true, std::numeric_limits<std::uint64_t>::max());
    check(read(request) == "on 18446744073709551615\n", "maximum uint64 request was not canonical");
    private_endpoint(request, 24);
    writer.publish(false);
    check(read(request) == "off\n", "explicit OFF did not replace ON");
    writer.publish(true, 2000000000);
  }
  check(read(request) == "off\n", "destruction did not withdraw a valid request");
  no_temporaries(dir);
  // A new owner can adopt an inspected legacy manual endpoint, then writes the
  // timed managed protocol exclusively; it never republishes untimed ON.
  write(request, "on\n");
  {
    c::RequestPublisher writer(request, owner);
    check(read(request) == "off\n", "legacy request was not safely withdrawn");
  }
}
void process_ownership() {
  Directory dir;
  const auto request = dir.file("request"), owner = dir.file("owner"), status = dir.file("status");
  {
    c::RequestPublisher writer(request, owner);
    writer.publish(true, 1000);
    check(child([&] { rejects([&] { c::RequestPublisher other(request, owner); }); }) == 0,
          "second process acquired an existing request writer lock");
    check(read(request) == "on 1000\n", "rejected writer changed the active request");
    c::StatusPublisher reporter(status);
    reporter.publish({100, 1, 200, c::ControlPhase::active});
    check(child([&] { rejects([&] { c::StatusPublisher other(status); }); }) == 0,
          "second process acquired an existing status writer lock");
    check(read(status) == "HCS1 100 1 200 active\n",
          "rejected status writer changed the current snapshot");
  }
  c::RequestPublisher resumed(request, owner);
  c::StatusPublisher reporter(status);
  reporter.publish({201, 1, 202, c::ControlPhase::idle});
  no_temporaries(dir);
}
void status_roundtrip_and_progression() {
  Directory dir;
  const auto path = dir.file("status");
  c::StatusReader reader(path);
  check(!reader.poll(100, 100), "missing status looked live");
  c::StatusPublisher writer(path);
  check(!fs::exists(path), "status constructor invented a heartbeat");
  c::ControlStatus value{100, 1, 200, c::ControlPhase::waiting};
  writer.publish(value);
  check(read(path) == "HCS1 100 1 200 waiting\n", "status wire format changed");
  private_endpoint(path, 23);
  private_endpoint(path + ".owner", 0);
  check(reader.poll(200, 1) == value && reader.poll(201, 1) == value,
        "fresh unchanged status was not retained");
  File held(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  struct stat original{};
  check(::fstat(held.fd, &original) == 0, "retained status stat failed");
  writer.publish({100, 2, 200, c::ControlPhase::active});
  struct stat retired{}, current{};
  check(::fstat(held.fd, &retired) == 0 && ::lstat(path.c_str(), &current) == 0 &&
            original.st_nlink == 1 && retired.st_nlink == 0 &&
            (retired.st_dev != current.st_dev || retired.st_ino != current.st_ino),
        "atomic status renewal did not retire its old inode");
  check(read_fd(held.fd) == "HCS1 100 1 200 waiting\n", "status rewrote a retained inode in place");
  check(reader.poll(202, 1) == (c::ControlStatus{100, 2, 200, c::ControlPhase::active}),
        "new status not read");
  rejects([&] { writer.publish({100, 2, 200, c::ControlPhase::fault}); });
  rejects([&] { writer.publish({100, 1, 201, c::ControlPhase::active}); });
  rejects([&] { writer.publish({100, 3, 199, c::ControlPhase::active}); });
  rejects([&] { writer.publish({101, 3, 201, c::ControlPhase::active}); });
  for (const auto invalid :
       std::array<c::ControlStatus, 5>{{{0, 1, 200, c::ControlPhase::idle},
                                        {100, 0, 200, c::ControlPhase::idle},
                                        {100, 1, 0, c::ControlPhase::idle},
                                        {201, 1, 200, c::ControlPhase::idle},
                                        {100, 3, 201, static_cast<c::ControlPhase>(99)}}})
    rejects([&] { writer.publish(invalid); });
  writer.publish({100, 3, 203, c::ControlPhase::stopped});
  check(reader.poll(203, 1)->phase == c::ControlPhase::stopped, "fresh stopped phase was hidden");
  no_temporaries(dir);
}
void status_age_and_loss() {
  Directory dir;
  const auto path = dir.file("status");
  c::StatusPublisher writer(path);
  c::StatusReader reader(path);
  writer.publish({100, 1, 200, c::ControlPhase::active});
  check(reader.poll(200, 1).has_value() && reader.poll(1000199, 1).has_value(),
        "fresh status expired before its limit");
  check(!reader.poll(1000200, 1), "status remained live at age equal to maximum");
  check(!reader.poll(1000201, 2),
        "larger age policy revived a previously expired unchanged snapshot");
  writer.publish({100, 2, 1000201, c::ControlPhase::active});
  check(reader.poll(1000201, 1).has_value(), "new heartbeat did not recover expiry");
  fs::remove(path);
  check(!reader.poll(1000202, 1), "removed status remained live");
  write(path, "HCS1 100 2 1000201 active\n");
  check(!reader.poll(1000203, 1), "unchanged status revived after a file loss");
  writer.publish({100, 3, 1000204, c::ControlPhase::active});
  check(reader.poll(1000204, 1).has_value(), "strictly newer status did not recover loss");
  check(!reader.poll(1000205, 0), "zero freshness policy accepted a status");
  check(!reader.poll(1000206, 1), "same snapshot revived after invalid freshness policy");
  writer.publish({100, 4, 1000207, c::ControlPhase::active});
  check(reader.poll(1000207, 1).has_value(), "valid policy/new snapshot did not recover");
}
void fresh_terminal_without_writer() {
  Directory dir;
  const auto path = dir.file("status");
  c::StatusReader reader(path);
  {
    c::StatusPublisher writer(path);
    writer.publish({100, 1, 200, c::ControlPhase::stopped});
  }
  const auto value = reader.poll(200, 1);
  check(value && value->phase == c::ControlPhase::stopped,
        "reader required a live owner for a fresh terminal status");
  check(!reader.poll(1000200, 1), "terminal snapshot age renewed after its writer exited");
}
void replay_and_clock_rules() {
  Directory dir;
  const auto path = dir.file("status");
  c::StatusReader reader(path);
  write(path, "HCS1 100 10 200 active\n");
  check(reader.poll(200, 10).has_value(), "initial replay fixture rejected");
  write(path, "HCS1 100 10 200 waiting\n");
  check(!reader.poll(201, 10), "conflicting equal sequence accepted");
  write(path, "HCS1 100 10 200 active\n");
  check(!reader.poll(202, 10), "unchanged status revived after conflict");
  for (const auto input :
       {"HCS1 100 9 203 active\n", "HCS1 99 100 204 active\n", "HCS1 100 11 199 active\n"}) {
    write(path, input);
    check(!reader.poll(205, 10), "replayed instance/sequence/date accepted");
  }
  write(path, "HCS1 101 1 206 waiting\n");
  check(reader.poll(206, 10)->sequence == 1, "genuine newer instance did not reset its sequence");
  write(path, "HCS1 100 50 207 active\n");
  check(!reader.poll(207, 10), "old instance revived after restart");
  write(path, "HCS1 102 1 205 active\n");
  check(!reader.poll(207, 10), "restart regressed original observation time");
  write(path, "HCS1 102 1 208 active\n");
  check(reader.poll(208, 10).has_value(), "newer instance could not recover rollback");
  check(!reader.poll(207, 10), "reversed reader clock accepted status");
  write(path, "HCS1 103 1 209 active\n");
  check(!reader.poll(209, 10), "retired reference clock silently recovered");
  c::StatusReader new_clock(path);
  check(new_clock.poll(209, 10).has_value(), "new reader could not adopt a valid clock domain");
  c::StatusReader future(path);
  write(path, "HCS1 103 2 300 active\n");
  check(!future.poll(250, 10), "future original observation accepted");
  write(path, "HCS1 103 3 250 active\n");
  check(future.poll(250, 10).has_value(),
        "genuine current status rejected after an unaccepted future file");
}
void malformed_content() {
  Directory dir;
  const auto request = dir.file("request"), owner = dir.file("owner"), status = dir.file("status");
  const std::array<std::string, 15> requests{{"", "on", "ON\n", "on \n", "on 0\n", "on 01\n",
                                              "on +1\n", "on -1\n", "on 1 \n", "on 1\r\n",
                                              "on 18446744073709551616\n", "off", "off\n\n",
                                              std::string("on\0", 3), std::string(25, 'x')}};
  for (const auto& bad : requests) {
    write(request, bad);
    rejects([&] { c::RequestPublisher writer(request, owner); });
    check(read(request) == bad, "malformed request was overwritten");
  }
  const std::array<std::string, 17> statuses{
      {"", "HCS1 1 1 1 active", "HCS1 1 1 1 active\n\n", "HCS1 1 1 1 Active\n",
       "HCS1 1 1 1 active extra\n", "HCS1 1 1 1\n", "HCS1 1 1 0 active\n", "HCS1 0 1 1 active\n",
       "HCS1 2 1 1 active\n", "HCS1 1 0 1 active\n", "HCS1 01 1 1 active\n", "HCS1 1 +1 1 active\n",
       "HCS1 1 18446744073709551616 1 active\n", "HCS1 1  1 1 active\n", "HCS1 1 1 1 active\r\n",
       std::string("HCS1 1 1 1 active\0\n", 19), std::string(129, 'x')}};
  c::StatusReader reader(status);
  std::uint64_t now = 100;
  for (const auto& bad : statuses) {
    write(status, bad);
    check(!reader.poll(now++, 100), "malformed status accepted");
    rejects([&] { c::StatusPublisher writer(status); });
    check(read(status) == bad, "malformed status was overwritten");
  }
  write(status, "HCS1 18446744073709551615 18446744073709551615 18446744073709551615 fault\n");
  c::StatusReader maximum(status);
  check(maximum.poll(std::numeric_limits<std::uint64_t>::max(), 1).has_value(),
        "valid maximum uint64 status rejected");
  no_temporaries(dir);
}
void unsafe_endpoints() {
  Directory dir;
  const auto request = dir.file("request"), owner = dir.file("owner"), status = dir.file("status"),
             sentinel = dir.file("sentinel");
  write(sentinel, "sentinel");
  for (const auto mode : {0400, 0640, 0660, 0700, 04600}) {
    write(request, "off\n", mode);
    rejects([&] { c::RequestPublisher writer(request, owner); });
    check(read(request) == "off\n", "unsafe request was overwritten");
    write(status, "HCS1 1 1 1 idle\n", mode);
    c::StatusReader reader(status);
    check(!reader.poll(1, 1), "unsafe status mode accepted");
    rejects([&] { c::StatusPublisher writer(status); });
  }
  fs::remove(request);
  fs::remove(status);
  for (const auto& path : {request, status}) {
    fs::create_symlink(sentinel, path);
    rejects([&] {
      if (path == request) {
        c::RequestPublisher writer(request, owner);
      } else {
        c::StatusPublisher writer(status);
      }
    });
    if (path == status) {
      c::StatusReader reader(status);
      check(!reader.poll(1, 1), "status symlink followed");
    }
    check(read(sentinel) == "sentinel" && fs::is_symlink(path), "redirected endpoint was modified");
    fs::remove(path);
    fs::create_directory(path);
    rejects([&] {
      if (path == request) {
        c::RequestPublisher writer(request, owner);
      } else {
        c::StatusPublisher writer(status);
      }
    });
    fs::remove(path);
    check(::mkfifo(path.c_str(), 0600) == 0, "FIFO fixture creation failed");
    const auto began = std::chrono::steady_clock::now();
    rejects([&] {
      if (path == request) {
        c::RequestPublisher writer(request, owner);
      } else {
        c::StatusPublisher writer(status);
      }
    });
    if (path == status) {
      c::StatusReader reader(status);
      check(!reader.poll(1, 1), "FIFO status accepted");
    }
    check(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(500),
          "known FIFO endpoint blocked file operations");
    fs::remove(path);
    File socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    check(path.size() < sizeof(address.sun_path), "socket fixture path too long");
    std::copy(path.begin(), path.end(), address.sun_path);
    check(::bind(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
              ::chmod(path.c_str(), 0600) == 0,
          "socket endpoint fixture creation failed");
    rejects([&] {
      if (path == request) {
        c::RequestPublisher writer(request, owner);
      } else {
        c::StatusPublisher writer(status);
      }
    });
    if (path == status) {
      c::StatusReader reader(status);
      check(!reader.poll(1, 1), "socket status accepted");
    }
    fs::remove(path);
  }
  write(request, "off\n");
  fs::create_hard_link(request, dir.file("linked-request"));
  rejects([&] { c::RequestPublisher writer(request, owner); });
  check(read(request) == "off\n", "linked request overwritten");
  write(status, "HCS1 1 1 1 active\n");
  fs::create_hard_link(status, dir.file("linked-status"));
  c::StatusReader reader(status);
  check(!reader.poll(1, 1), "linked status accepted");
  rejects([&] { c::StatusPublisher writer(status); });
  no_temporaries(dir);
}
void owner_replacement_and_aliases() {
  Directory dir;
  const auto request = dir.file("request"), owner = dir.file("owner");
  rejects([&] { c::RequestPublisher bad(request, request); });
  check(!fs::exists(request), "aliased missing endpoint was created");
  // A symlink in an ancestor can name the same real final parent. Detect its
  // inode/basename alias instead of comparing only strings.
  const auto real = dir.path / "real";
  fs::create_directory(real);
  fs::permissions(real, fs::perms::owner_all);
  const auto alias = dir.path / "ancestor";
  fs::create_directory_symlink(dir.path, alias);
  rejects([&] {
    c::RequestPublisher bad((real / "same").string(), (alias / "real" / "same").string());
  });
  check(!fs::exists(real / "same"), "ancestor alias created an owner endpoint");
  {
    c::RequestPublisher old(request, owner);
    old.publish(true, 1000);
    fs::remove(owner);
    write(owner, "");
    rejects([&] { old.publish(false); });
    {
      c::RequestPublisher current(request, owner);
      current.publish(true, 2000);
      rejects([&] { old.publish(true, 3000); });
    }
    check(read(request) == "off\n", "current writer destructor did not withdraw");
    write(request, "on 4000\n");
  }
  check(read(request) == "on 4000\n",
        "invalidated old owner destructor overwrote replacement generation");
  fs::remove(owner);
  fs::create_hard_link(request, owner);
  rejects([&] { c::RequestPublisher bad(request, owner); });
}
void owner_shapes() {
  Directory dir;
  const auto request = dir.file("request"), owner = dir.file("owner"), status = dir.file("status"),
             status_owner = status + ".owner";
  for (const auto& lock : {owner, status_owner}) {
    write(lock, "nonempty");
    rejects([&] {
      if (lock == owner) {
        c::RequestPublisher writer(request, owner);
      } else {
        c::StatusPublisher writer(status);
      }
    });
    check(read(lock) == "nonempty", "malformed owner file was truncated");
    write(lock, "", 0644);
    rejects([&] {
      if (lock == owner) {
        c::RequestPublisher writer(request, owner);
      } else {
        c::StatusPublisher writer(status);
      }
    });
    fs::remove(lock);
    fs::create_symlink(dir.file("missing"), lock);
    rejects([&] {
      if (lock == owner) {
        c::RequestPublisher writer(request, owner);
      } else {
        c::StatusPublisher writer(status);
      }
    });
    fs::remove(lock);
    check(::mkfifo(lock.c_str(), 0600) == 0, "owner FIFO fixture failed");
    rejects([&] {
      if (lock == owner) {
        c::RequestPublisher writer(request, owner);
      } else {
        c::StatusPublisher writer(status);
      }
    });
    fs::remove(lock);
  }
  no_temporaries(dir);
}
void pinned_parent_and_destructor() {
  Directory dir;
  const auto original = dir.path / "private", moved = dir.path / "moved";
  fs::create_directory(original);
  fs::permissions(original, fs::perms::owner_all);
  const auto request = (original / "request").string(), owner = (original / "owner").string(),
             status = (original / "status").string();
  {
    c::RequestPublisher writer(request, owner);
    writer.publish(true, 1000);
    c::StatusPublisher reporter(status);
    reporter.publish({1, 1, 2, c::ControlPhase::active});
    c::StatusReader reader(status);
    check(reader.poll(2, 1).has_value(), "pinned-parent fixture rejected");
    fs::rename(original, moved);
    fs::create_directory(original);
    fs::permissions(original, fs::perms::owner_all);
    write(request, "on 2000\n");
    write(status, "HCS1 2 1 3 active\n");
    rejects([&] { writer.publish(false); });
    rejects([&] { reporter.publish({1, 2, 3, c::ControlPhase::active}); });
    check(!reader.poll(3, 1), "replacement parent revived a status reader");
  }
  check(read(request) == "on 2000\n" && read((moved / "request").string()) == "on 1000\n",
        "destructor changed an invalid parent or its replacement");
  const auto link = dir.path / "parent-link";
  fs::create_directory_symlink(original, link);
  rejects([&] { c::StatusReader reader((link / "status").string()); });
  rejects(
      [&] { c::RequestPublisher writer((link / "request").string(), (link / "owner").string()); });
}
void loss_from_unsafe_mode() {
  Directory dir;
  const auto path = dir.file("status");
  c::StatusPublisher publisher(path);
  c::StatusReader reader(path);
  publisher.publish({1, 1, 2, c::ControlPhase::active});
  check(reader.poll(2, 1).has_value(), "mode-loss fixture rejected");
  check(::chmod(path.c_str(), 0644) == 0, "mode-loss change failed");
  check(!reader.poll(3, 1), "unsafe current mode stayed live");
  rejects([&] { publisher.publish({1, 2, 3, c::ControlPhase::active}); });
  check(::chmod(path.c_str(), 0600) == 0, "mode restoration failed");
  check(!reader.poll(4, 1), "unchanged status revived after unsafe mode loss");
  publisher.publish({1, 2, 5, c::ControlPhase::active});
  check(reader.poll(5, 1).has_value(), "new heartbeat did not recover mode loss");
}
void invalid_paths_and_owners() {
  Directory dir;
  const auto owner = dir.file("owner");
  for (const auto& path :
       {std::string("relative"), dir.file(".") + "/status", dir.file("..") + "/status",
        dir.file("missing/status"), std::string("/tmp/camera\0status", 18),
        dir.file(std::string(256, 'x'))}) {
    rejects([&] { c::StatusReader reader(path); });
    rejects([&] { c::StatusPublisher writer(path); });
    rejects([&] { c::RequestPublisher writer(path, owner); });
  }
  check(::chmod(dir.path.c_str(), 0750) == 0, "unsafe parent fixture failed");
  rejects([&] { c::StatusReader reader(dir.file("status")); });
  rejects([&] { c::RequestPublisher writer(dir.file("request"), owner); });
  check(::chmod(dir.path.c_str(), 0700) == 0, "parent permission restoration failed");
  if (::geteuid() == 0) {
    const auto status = dir.file("status");
    write(status, "HCS1 1 1 1 active\n");
    check(::chown(status.c_str(), 1, 1) == 0, "foreign owner fixture failed");
    c::StatusReader reader(status);
    check(!reader.poll(1, 1), "foreign-owned status accepted");
    rejects([&] { c::StatusPublisher publisher(status); });
  }
}
void dead_process_liveness() {
  Directory dir;
  const auto request = dir.file("request"), owner = dir.file("owner"), status = dir.file("status");
  int ready[2]{};
  check(::pipe(ready) == 0, "liveness pipe failed");
  const auto pid = ::fork();
  check(pid >= 0, "liveness fork failed");
  if (!pid) {
    ::close(ready[0]);
    try {
      c::RequestPublisher writer(request, owner);
      c::StatusPublisher publisher(status);
      writer.publish(true, 1500000000);
      publisher.publish({1000000000, 1, 1000000000, c::ControlPhase::active});
      const char token = 'r';
      if (::write(ready[1], &token, 1) != 1) ::_exit(92);
      for (;;)
        ::pause();
    } catch (...) {
      ::_exit(93);
    }
  }
  ::close(ready[1]);
  char token = 0;
  const auto got = ::read(ready[0], &token, 1);
  ::close(ready[0]);
  if (got != 1 || token != 'r') {
    int code = 0;
    ::waitpid(pid, &code, 0);
    throw std::runtime_error("liveness child failed startup");
  }
  c::ActivationControl activation(request);
  c::StatusReader reader(status);
  check(activation.poll(1000000000) && reader.poll(1000000000, 100).has_value(),
        "live child files not accepted");
  check(::kill(pid, SIGKILL) == 0, "liveness child kill failed");
  int code = 0;
  check(::waitpid(pid, &code, 0) == pid && WIFSIGNALED(code), "liveness child did not exit");
  // Killing skips C++ destructors. Only original timed lease/heartbeat dates
  // establish freshness; rereading persistent files never renews either.
  check(!activation.poll(1500000000) && !reader.poll(1100000000, 100),
        "dead producer stayed live past its original limits");
  {
    c::RequestPublisher replacement(request, owner);
    check(read(request) == "off\n", "dead request writer lock did not release");
  }
  c::StatusPublisher replacement(status);
  replacement.publish({1500000001, 1, 1500000002, c::ControlPhase::idle});
  check(reader.poll(1500000002, 100).has_value(),
        "new producer instance did not recover a dead writer");
  no_temporaries(dir);
}
void real_clock_age_and_retirement() {
  Directory dir;
  const auto path = dir.file("status");
  c::StatusPublisher writer(path);
  c::StatusReader reader(path);
  const auto instance = now_ns();
  writer.publish({instance, 1, instance, c::ControlPhase::active});
  const auto first = reader.poll(100);
  check(first && first->observed_ns == instance,
        "real-clock polling replaced or rejected the original date");
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  check(!reader.poll(100), "real-clock polling renewed an expired heartbeat");
  check(!reader.poll(1000), "larger real-clock age revived unchanged expired data");
  const auto renewed = now_ns();
  writer.publish({instance, 2, renewed, c::ControlPhase::active});
  check(reader.poll(1000) == (c::ControlStatus{instance, 2, renewed, c::ControlPhase::active}),
        "new genuine heartbeat did not recover real-clock expiry");
  // Both overloads share one reference domain. An explicit future fixture
  // cannot silently reset to the machine clock on a later production poll.
  check(!reader.poll(std::numeric_limits<std::uint64_t>::max(), 1),
        "far-future reference accepted stale status");
  check(!reader.poll(1000), "real-clock overload ignored a reversed shared reference");
  writer.publish({instance, 3, now_ns(), c::ControlPhase::active});
  check(!reader.poll(1000), "retired real-clock reader revived with a new heartbeat");
}
void concurrent_atomic_renewal() {
  Directory dir;
  const auto status = dir.file("status"), request = dir.file("request"), owner = dir.file("owner");
  int ready[2]{}, stop[2]{};
  check(::pipe(ready) == 0 && ::pipe(stop) == 0, "renewal pipes failed");
  RunningChild writer;
  writer.pid = ::fork();
  check(writer.pid >= 0, "renewal fork failed");
  if (!writer.pid) {
    ::close(ready[0]);
    ::close(stop[1]);
    try {
      c::RequestPublisher activation(request, owner);
      c::StatusPublisher heartbeat(status);
      const auto instance = now_ns();
      // Renew every 10 ms until the reader closes the stop pipe. The bound only
      // limits an abandoned child; the reader finishes long before it.
      for (unsigned sequence = 1; sequence <= 2000; ++sequence) {
        if (sequence > 1) {
          pollfd done{stop[0], POLLIN, 0};
          if (::poll(&done, 1, 10) != 0) break;
        }
        activation.publish(true, now_ns() + 900000000);
        heartbeat.publish({instance, sequence, now_ns(), c::ControlPhase::active});
        if (sequence == 1) {
          const char token = 'r';
          if (::write(ready[1], &token, 1) != 1) ::_exit(92);
          ::close(ready[1]);
        }
      }
      ::_exit(0);
    } catch (...) {
      ::_exit(93);
    }
  }
  ::close(ready[1]);
  ::close(stop[0]);
  char token = 0;
  const auto got = ::read(ready[0], &token, 1);
  ::close(ready[0]);
  check(got == 1 && token == 'r', "concurrent publisher failed startup");
  c::StatusReader heartbeat(status);
  c::ActivationControl activation(request);
  const auto began = std::chrono::steady_clock::now();
  unsigned polls = 0;
  std::uint64_t sequence = 0, instance = 0;
  // Realistic control cadence, actual child-process atomic publishers, and
  // genuine dates. No synthetic clock advancement or accepted heartbeat loss.
  // Every poll must succeed for at least 450 ms and until meaningful progress
  // is observed; a loaded host may slow progress but never excuses a loss.
  while (std::chrono::steady_clock::now() - began < std::chrono::milliseconds(450) || polls < 30 ||
         sequence < 10) {
    check(std::chrono::steady_clock::now() - began < std::chrono::seconds(10),
          "renewal check did not observe meaningful concurrent progress");
    const auto value = heartbeat.poll(1000);
    check(value && value->phase == c::ControlPhase::active && value->observed_ns <= now_ns(),
          "valid atomic heartbeat renewal became an apparent status loss");
    check(!instance || value->instance_started_ns == instance,
          "renewal unexpectedly changed publisher instance");
    check(value->sequence >= sequence, "concurrent renewal regressed status sequence");
    instance = value->instance_started_ns;
    sequence = value->sequence;
    ++polls;
    check(activation.read() == c::ActivationRequest::on,
          "valid atomic activation renewal became unavailable or OFF");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ::close(stop[1]);
  writer.finish();
  no_temporaries(dir);
}
}  // namespace
int main() try {
  bounded_temporary_collisions();
  request_roundtrip_and_atomicity();
  process_ownership();
  status_roundtrip_and_progression();
  status_age_and_loss();
  fresh_terminal_without_writer();
  replay_and_clock_rules();
  malformed_content();
  unsafe_endpoints();
  owner_replacement_and_aliases();
  owner_shapes();
  pinned_parent_and_destructor();
  loss_from_unsafe_mode();
  invalid_paths_and_owners();
  dead_process_liveness();
  real_clock_age_and_retirement();
  concurrent_atomic_renewal();
  std::cout << "camera controller I/O: 17 actual-filesystem groups passed (explicit/real clocks, "
               "cross-process ownership, renewal and liveness)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
