// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_control.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using headunit::camera::ActivationControl;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template <class F>
void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "unsafe camera control constructor was accepted");
}
struct Fixture {
  fs::path folder;
  Fixture() {
    char name[] = "/tmp/headunit-camera-control-XXXXXX";
    const auto* path = ::mkdtemp(name);
    check(path, "fixture creation failed");
    folder = path;
  }
  ~Fixture() { fs::remove_all(folder); }
  void replace(const fs::path& path, std::string_view text) {
    const auto next = fs::path(path.string() + ".next");
    {
      std::ofstream output(next, std::ios::binary);
      output.write(text.data(), std::streamsize(text.size()));
      check(bool(output), "request write failed");
    }
    fs::permissions(next, fs::perms::owner_read | fs::perms::owner_write);
    fs::rename(next, path);
  }
};
int main() try {
  Fixture f;
  const auto parent = f.folder / "private";
  fs::create_directory(parent);
  fs::permissions(parent, fs::perms::owner_all);
  const auto path = parent / "activation";
  ActivationControl control(path.string());
  check(!control.poll(), "missing request activated camera");
  check(control.read() == headunit::camera::ActivationRequest::unavailable,
        "missing input became an intended off edge");
  f.replace(path, "on\n");
  check(control.poll(), "exact on request did not activate camera");
  f.replace(path, "off\n");
  check(!control.poll(), "exact off request retained activation");
  check(control.read() == headunit::camera::ActivationRequest::off, "valid off intent was lost");
  for (const auto text : {"", "on", "ON\n", " on\n", "on\r\n", "on\n\n", "off", "no\n", "on\nx"}) {
    f.replace(path, text);
    check(!control.poll(), "malformed request activated camera");
    check(control.read() == headunit::camera::ActivationRequest::unavailable,
          "malformed input became an intended off edge");
  }
  f.replace(path, std::string_view("on\0", 3));
  check(!control.poll(), "NUL request activated camera");
  f.replace(path, std::string(1024 * 1024, 'x'));
  check(!control.poll(), "oversized request activated camera");
  f.replace(path, "on\n");
  fs::remove(path);
  check(!control.poll(), "removed request retained activation");
  // Every atomic replacement is observed; no cached inode or previous boolean.
  for (unsigned i = 0; i < 20; ++i) {
    f.replace(path, i % 2 ? "off\n" : "on\n");
    check(control.poll() == !(i % 2), "atomic request replacement was not observed");
  }
  {
    ActivationControl timed(path.string());
    constexpr std::uint64_t date = 10'000'000'000;
    f.replace(path, "on 10250000000\n");
    check(timed.poll(date), "fresh timed request rejected");
    check(timed.poll(date + 249'999'999), "request expired early");
    check(!timed.poll(date + 250'000'000), "request retained activation at expiry");
    check(!timed.poll(date), "clock reversal revived an expired request");
    f.replace(path, "on 11000000000\n");
    ActivationControl boundary(path.string());
    check(boundary.poll(date), "one-second deadline rejected");
    for (const auto text : {"on 11000000001\n", "on 10000000000\n", "on 0\n", "on 010250000000\n",
                            "on +10250000000\n", "on -1\n", "on 10250000000 \n", "on 1e10\n",
                            "on 18446744073709551616\n", "on 10250000000\r\n"}) {
      f.replace(path, text);
      check(!boundary.poll(date), "malformed/expired/unbounded deadline activated camera");
    }
    f.replace(path, "on 18446744073709551615\n");
    ActivationControl overflow(path.string());
    check(overflow.poll(UINT64_MAX - 1), "deadline subtraction incorrectly overflowed");
    check(!overflow.poll(UINT64_MAX), "maximum deadline did not expire");
    ActivationControl invalid_clock(path.string());
    check(!invalid_clock.poll(0), "nonpositive clock accepted");
    check(!invalid_clock.poll(date), "invalid clock reader recovered without new generation");
  }
  f.replace(path, "on\n");
  const auto hardlink = parent / "hardlink";
  fs::create_hard_link(path, hardlink);
  check(!control.poll(), "multiply linked request activated camera");
  fs::remove(hardlink);
  f.replace(path, "on\n");
  for (const auto mode :
       {fs::perms::owner_all, fs::perms::owner_read,
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read,
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::others_write}) {
    fs::permissions(path, mode);
    check(!control.poll(), "unsafe request permissions activated camera");
  }
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
  check(control.poll(), "safe permissions did not recover activation");
  const auto real = parent / "real";
  fs::rename(path, real);
  fs::create_symlink(real, path);
  check(!control.poll(), "request symlink was followed");
  fs::remove(path);
  fs::create_directory(path);
  check(!control.poll(), "request directory activated camera");
  fs::remove(path);
  check(::mkfifo(path.c_str(), 0600) == 0, "FIFO fixture failed");
  const auto began = std::chrono::steady_clock::now();
  check(!control.poll(), "FIFO request activated camera");
  check(std::chrono::steady_clock::now() - began < std::chrono::seconds(1),
        "FIFO request blocked the reader");
  fs::remove(path);
  // Ownership checks also run when privileged test execution can change owner.
  if (::geteuid() == 0) {
    f.replace(path, "on\n");
    check(::chown(path.c_str(), 1, 1) == 0, "foreign owner fixture failed");
    check(!control.poll(), "foreign-owned request activated camera");
    fs::remove(path);
    const auto other = f.folder / "other-owner";
    fs::create_directory(other);
    fs::permissions(other, fs::perms::owner_all);
    check(::chown(other.c_str(), 1, 1) == 0, "foreign parent fixture failed");
    rejects([&] { ActivationControl bad((other / "activation").string()); });
  }
  rejects([&] { ActivationControl bad("relative"); });
  rejects([&] { ActivationControl bad(std::string("/tmp/invalid\0path", 17)); });
  rejects([&] { ActivationControl bad((parent / ".").string()); });
  rejects([&] { ActivationControl bad((parent / "..").string()); });
  rejects([&] { ActivationControl bad((f.folder / "missing" / "activation").string()); });
  fs::permissions(parent, fs::perms::owner_all | fs::perms::group_read);
  rejects([&] { ActivationControl bad(path.string()); });
  check(!control.poll(), "unsafe parent permissions retained activation");
  fs::permissions(parent, fs::perms::owner_all);
  f.replace(path, "on\n");
  check(control.poll(), "safe parent did not recover activation");
  const auto alias = f.folder / "alias";
  fs::create_directory_symlink(parent, alias);
  rejects([&] { ActivationControl bad((alias / "activation").string()); });
  rejects([&] { ActivationControl bad((alias / "." / "activation").string()); });
  rejects([&] { ActivationControl bad((parent / ".." / "private" / "activation").string()); });
  // A renamed directory remains pinned, but its old on file cannot be used
  // through a replacement path or a new symlink at the configured parent.
  const auto moved = f.folder / "moved";
  fs::rename(parent, moved);
  fs::create_directory(parent);
  fs::permissions(parent, fs::perms::owner_all);
  f.replace(path, "on\n");
  check(!control.poll(), "replacement parent retained old activation");
  fs::remove_all(parent);
  fs::create_directory_symlink(moved, parent);
  check(!control.poll(), "replacement parent symlink was followed");
  std::cout << "camera activation control: exact requests, atomic replacement, private ownership, "
               "redirects and fail-closed errors passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
