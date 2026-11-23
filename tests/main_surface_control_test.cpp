// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/main_surface_control.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace headunit;
namespace fs = std::filesystem;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
namespace {
std::uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
struct Fixture {
  fs::path root, request, lease;
  Fixture() {
    char name[] = "/tmp/headunit-main-surface-XXXXXX";
    CHECK(::mkdtemp(name));
    root = fs::canonical(name);
    request = root / "request";
    lease = root / "lease";
    CHECK(::chmod(root.c_str(), 0700) == 0);
  }
  ~Fixture() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  void publish(std::string value) {
    const auto next = root / "next";
    const int fd = ::open(next.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    CHECK(::write(fd, value.data(), value.size()) == ssize_t(value.size()));
    CHECK(::close(fd) == 0);
    CHECK(::rename(next.c_str(), request.c_str()) == 0);
  }
};
template <class F>
void rejects(F callback) {
  bool failed = false;
  try {
    callback();
  } catch (...) {
    failed = true;
  }
  CHECK(failed);
}
void admission() {
  Fixture f;
  MainSurfaceControl::validate(f.request.string(), f.lease.string());
  CHECK(!fs::exists(f.request) && !fs::exists(f.lease));  // Validation allocates no owner.
  rejects([&] { MainSurfaceControl::validate(f.request.string(), f.request.string()); });
  Fixture other;
  rejects([&] { MainSurfaceControl::validate(f.request.string(), other.lease.string()); });
  const auto alias = f.root / "alias";
  fs::create_directory_symlink(other.root, alias);
  rejects([&] {
    MainSurfaceControl::validate((alias / "request").string(), (alias / "lease").string());
  });
  rejects([&] {
    MainSurfaceControl::validate((f.root / ".." / f.root.filename() / "request").string(),
                                 f.lease.string());
  });
  f.publish("off\n");
  CHECK(::chmod(f.request.c_str(), 0644) == 0);
  rejects([&] { MainSurfaceControl::validate(f.request.string(), f.lease.string()); });
  CHECK(::chmod(f.request.c_str(), 0600) == 0);
  fs::create_hard_link(f.request, f.lease);
  rejects([&] { MainSurfaceControl::validate(f.request.string(), f.lease.string()); });
  fs::remove(f.lease);
  fs::remove(f.request);
  fs::create_symlink(other.request, f.request);
  rejects([&] { MainSurfaceControl::validate(f.request.string(), f.lease.string()); });
  fs::remove(f.request);
  CHECK(::mkfifo(f.request.c_str(), 0600) == 0);
  rejects([&] { MainSurfaceControl::validate(f.request.string(), f.lease.string()); });
}
void handoff() {
  Fixture f;
  MainSurfaceControl main(f.request.string(), f.lease.string());
  CHECK(!fs::exists(f.request) && main.native_requested() && !main.renderer_acquire());
  camera::CameraSurfaceLease page(f.lease.string());
  f.publish("off\n");
  CHECK(!main.native_requested() && main.renderer_acquire() && main.renderer_holds());
  CHECK(!page.acquire_exclusive());
  f.publish("on " + std::to_string(now() + 250000000) + "\n");
  CHECK(main.native_requested() && !main.renderer_acquire());
  // An ON request cannot release a still-live renderer surface on its own.
  CHECK(main.renderer_holds() && !page.acquire_exclusive());
  main.renderer_release();
  CHECK(!main.renderer_holds() && page.acquire_exclusive());
  CHECK(!main.renderer_acquire());
  f.publish("off\n");  // Back's OFF still cannot bypass a foreground page's EX.
  CHECK(!main.native_requested() && !main.renderer_acquire());
  page.release();
  CHECK(main.renderer_acquire());
  main.renderer_release();
  f.publish("on " + std::to_string(now() - 1) + "\n");
  CHECK(!main.native_requested() && main.renderer_acquire());
  main.renderer_release();
  for (const auto* bad : {"", "on 0\n", "on 01\n", "off \n", "on -1\n"}) {
    f.publish(bad);
    CHECK(main.native_requested() && !main.renderer_acquire());
  }
  f.publish("on " + std::to_string(now() + 2000000000) + "\n");
  CHECK(main.native_requested() && !main.renderer_acquire());
  f.publish("off\n");
  CHECK(::chmod(f.request.c_str(), 0644) == 0);
  CHECK(main.native_requested() && !main.renderer_acquire());
  CHECK(::chmod(f.request.c_str(), 0600) == 0);
  CHECK(main.renderer_acquire());
  const auto replacement = f.root / "replacement";
  {
    std::ofstream empty(replacement);
  }
  CHECK(::chmod(replacement.c_str(), 0600) == 0);
  fs::rename(replacement, f.lease);
  rejects([&] { (void)main.renderer_holds(); });
  rejects([&] { (void)main.renderer_acquire(); });
  main.renderer_release();  // Retiring the old pin cannot unlock a new inode.
}
void actual_other_process() {
  Fixture f;
  f.publish("off\n");
  MainSurfaceControl main(f.request.string(), f.lease.string());
  CHECK(main.renderer_acquire());
  int notify[2], resume[2];
  CHECK(::pipe(notify) == 0 && ::pipe(resume) == 0);
  const auto child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    // Inherited renderer flock is not used: the page opens its own inode.
    ::close(notify[0]);
    ::close(resume[1]);
    try {
      camera::CameraSurfaceLease page(f.lease.string());
      CHECK(!page.acquire_exclusive());
      CHECK(::write(notify[1], "n", 1) == 1);
      char go = 0;
      CHECK(::read(resume[0], &go, 1) == 1);
      CHECK(page.acquire_exclusive());
      CHECK(::write(notify[1], "y", 1) == 1);
      CHECK(::read(resume[0], &go, 1) == 1);
      page.release();
      ::_exit(0);
    } catch (...) {
      ::_exit(1);
    }
  }
  ::close(notify[1]);
  ::close(resume[0]);
  char reply = 0;
  CHECK(::read(notify[0], &reply, 1) == 1 && reply == 'n');
  main.renderer_release();
  CHECK(::write(resume[1], "r", 1) == 1);
  CHECK(::read(notify[0], &reply, 1) == 1 && reply == 'y');
  CHECK(!main.renderer_acquire());
  CHECK(::write(resume[1], "r", 1) == 1);
  int status = 0;
  CHECK(::waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(main.renderer_acquire());
  ::close(notify[0]);
  ::close(resume[1]);
  main.renderer_release();
}
void concurrent_queries() {
  Fixture f;
  f.publish("off\n");
  MainSurfaceControl main(f.request.string(), f.lease.string());
  std::vector<std::jthread> threads;
  for (unsigned i = 0; i < 4; ++i)
    threads.emplace_back([&] {
      for (unsigned count = 0; count < 100; ++count)
        CHECK(!main.native_requested());
    });
  for (unsigned count = 0; count < 100; ++count) {
    CHECK(main.renderer_acquire());
    main.renderer_release();
  }
  threads.clear();
}
}  // namespace
int main() try {
  ::umask(0077);
  admission();
  handoff();
  actual_other_process();
  concurrent_queries();
  std::cout << "Main surface admission and genuine request/flock handoff checks passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
