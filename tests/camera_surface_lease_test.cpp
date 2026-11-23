// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_surface_lease.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
namespace c = headunit::camera;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "unsafe surface lease accepted");
}
struct Fixture {
  std::filesystem::path directory, file;
  Fixture() {
    char name[] = "/tmp/headunit-camera-lease-XXXXXX";
    auto* path = ::mkdtemp(name);
    check(path, "temporary directory failed");
    directory = path;
    file = directory / "surface";
  }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};
struct FileSnapshot {
  struct stat info{};
  explicit FileSnapshot(const std::filesystem::path& path) {
    check(::lstat(path.c_str(), &info) == 0, "snapshot failed");
  }
  void unchanged(const std::filesystem::path& path) const {
    const FileSnapshot after(path);
    check(info.st_dev == after.info.st_dev && info.st_ino == after.info.st_ino &&
              info.st_mode == after.info.st_mode && info.st_uid == after.info.st_uid &&
              info.st_gid == after.info.st_gid && info.st_nlink == after.info.st_nlink &&
              info.st_size == after.info.st_size,
          "preflight changed lease identity or metadata");
#ifdef __APPLE__
    const auto modified = info.st_mtimespec, changed = info.st_ctimespec;
    const auto after_modified = after.info.st_mtimespec, after_changed = after.info.st_ctimespec;
#else
    const auto modified = info.st_mtim, changed = info.st_ctim;
    const auto after_modified = after.info.st_mtim, after_changed = after.info.st_ctim;
#endif
    check(modified.tv_sec == after_modified.tv_sec && modified.tv_nsec == after_modified.tv_nsec &&
              changed.tv_sec == after_changed.tv_sec && changed.tv_nsec == after_changed.tv_nsec,
          "preflight changed lease modification timestamps");
  }
};
void validation_keeps_missing_lease_absent() {
  Fixture f;
  for (unsigned i = 0; i < 3; ++i)
    c::CameraSurfaceLease::validate(f.file.string());
  check(!std::filesystem::exists(f.file) && std::filesystem::is_empty(f.directory),
        "read-only preflight created a lease or temporary file");
  // The normal runtime path still creates an owner-only lease afterward.
  c::CameraSurfaceLease owner(f.file.string());
  check(std::filesystem::exists(f.file) && owner.acquire_shared(),
        "runtime no longer creates a usable lease");
}
void validation_preserves_existing_lease_and_lock() {
  Fixture f;
  c::CameraSurfaceLease camera(f.file.string()), main(f.file.string());
  check(camera.acquire_exclusive(), "exclusive owner setup failed");
  const FileSnapshot before(f.file);
  for (unsigned i = 0; i < 3; ++i)
    c::CameraSurfaceLease::validate(f.file.string());
  before.unchanged(f.file);
  check(camera.holds() && !main.acquire_shared(), "preflight changed a live exclusive lock");
  camera.release();
  check(main.acquire_shared(), "preflight retained a lock after returning");
  c::CameraSurfaceLease::validate(f.file.string());
  before.unchanged(f.file);
  main.release();
  check(camera.acquire_exclusive(), "preflight retained a shared lock");
}
void validation_rejects_unsafe_existing_without_changes() {
  Fixture f;
  const std::string contents = "do not truncate\n";
  {
    std::ofstream output(f.file);
    output << contents;
  }
  check(::chmod(f.file.c_str(), 0600) == 0, "nonempty lease mode failed");
  const FileSnapshot nonempty(f.file);
  rejects([&] { c::CameraSurfaceLease::validate(f.file.string()); });
  nonempty.unchanged(f.file);
  {
    std::ifstream input(f.file);
    const std::string actual{std::istreambuf_iterator<char>(input), {}};
    check(actual == contents, "preflight changed unsafe lease bytes");
  }
  std::filesystem::remove(f.file);
  {
    std::ofstream output(f.file);
  }
  check(::chmod(f.file.c_str(), 0644) == 0, "unsafe lease mode failed");
  const FileSnapshot public_file(f.file);
  rejects([&] { c::CameraSurfaceLease::validate(f.file.string()); });
  public_file.unchanged(f.file);
  check(::chmod(f.file.c_str(), 0600) == 0, "private lease mode failed");
  const auto target = f.directory / "target";
  std::filesystem::rename(f.file, target);
  std::filesystem::create_symlink(target, f.file);
  const FileSnapshot symlink(f.file), target_before(target);
  rejects([&] { c::CameraSurfaceLease::validate(f.file.string()); });
  symlink.unchanged(f.file);
  target_before.unchanged(target);
  std::filesystem::remove(f.file);
  std::filesystem::create_hard_link(target, f.file);
  const FileSnapshot linked(f.file);
  rejects([&] { c::CameraSurfaceLease::validate(f.file.string()); });
  linked.unchanged(f.file);
  std::filesystem::remove(f.file);
  check(::mkfifo(f.file.c_str(), 0600) == 0, "preflight FIFO setup failed");
  const FileSnapshot fifo(f.file);
  const auto start = std::chrono::steady_clock::now();
  rejects([&] { c::CameraSurfaceLease::validate(f.file.string()); });
  fifo.unchanged(f.file);
  check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
        "preflight waited on a FIFO");
  std::filesystem::remove(f.file);
  std::filesystem::create_directory(f.file);
  const FileSnapshot directory(f.file);
  rejects([&] { c::CameraSurfaceLease::validate(f.file.string()); });
  directory.unchanged(f.file);
}
void validation_rejects_unsafe_parent_and_path() {
  Fixture f;
  check(::chmod(f.directory.c_str(), 0755) == 0, "unsafe parent mode failed");
  rejects([&] { c::CameraSurfaceLease::validate(f.file.string()); });
  check(!std::filesystem::exists(f.file), "invalid parent preflight created a lease");
  check(::chmod(f.directory.c_str(), 0700) == 0, "private parent mode failed");
  const auto alias = f.directory / "alias";
  std::filesystem::create_symlink(f.directory, alias);
  rejects([&] { c::CameraSurfaceLease::validate((alias / "surface").string()); });
  check(!std::filesystem::exists(f.file), "symlink parent preflight created a lease");
  for (const auto& path : std::vector<std::string>{
           "", "relative", std::string(4001, 'x'), f.file.string() + std::string(1, '\0'),
           (f.directory / "." / "surface").string(), (f.directory / ".." / "surface").string(),
           f.directory.string() + "/", (f.directory / "missing" / "surface").string()})
    rejects([&] { c::CameraSurfaceLease::validate(path); });
  check(!std::filesystem::exists(f.file), "invalid pathname preflight created a lease");
}
void exclusion_and_release() {
  Fixture f;
  c::CameraSurfaceLease main(f.file.string()), other_main(f.file.string()), camera(f.file.string());
  check(main.acquire_shared() && other_main.acquire_shared(), "shared main owners did not acquire");
  check(!camera.acquire_exclusive(), "camera mapped while main owned surface");
  main.release();
  check(!camera.acquire_exclusive(), "camera ignored second main owner");
  other_main.release();
  check(camera.acquire_exclusive() && camera.holds(), "native handoff failed");
  check(!main.acquire_shared(), "main mapped over native camera");
  rejects([&] { camera.acquire_shared(); });
  camera.release();
  check(main.acquire_shared(), "main did not regain surface");
  struct stat info{};
  check(::stat(f.file.c_str(), &info) == 0 && info.st_size == 0 && (info.st_mode & 0777) == 0600,
        "lease file was truncated or mutated");
}
void generation_and_files() {
  Fixture f;
  c::CameraSurfaceLease owner(f.file.string());
  check(owner.acquire_shared(), "test owner failed");
  std::filesystem::rename(f.file, f.directory / "old");
  std::ofstream(f.file).close();
  ::chmod(f.file.c_str(), 0600);
  rejects([&] { owner.holds(); });
  rejects([&] { owner.acquire_shared(); });
  owner.release();
  std::filesystem::remove(f.file);
  std::filesystem::create_symlink(f.directory / "old", f.file);
  rejects([&] { c::CameraSurfaceLease bad(f.file.string()); });
  std::filesystem::remove(f.file);
  check(::mkfifo(f.file.c_str(), 0600) == 0, "FIFO setup failed");
  rejects([&] { c::CameraSurfaceLease bad(f.file.string()); });
  std::filesystem::remove(f.file);
  std::filesystem::create_hard_link(f.directory / "old", f.file);
  rejects([&] { c::CameraSurfaceLease bad(f.file.string()); });
  std::filesystem::remove(f.file);
  std::ofstream(f.file) << "on\n";
  ::chmod(f.file.c_str(), 0600);
  rejects([&] { c::CameraSurfaceLease bad(f.file.string()); });
  std::filesystem::remove(f.file);
  std::ofstream(f.file).close();
  ::chmod(f.file.c_str(), 0644);
  rejects([&] { c::CameraSurfaceLease bad(f.file.string()); });
  ::chmod(f.file.c_str(), 0600);
  ::chmod(f.directory.c_str(), 0755);
  rejects([&] { c::CameraSurfaceLease bad(f.file.string()); });
  ::chmod(f.directory.c_str(), 0700);
  rejects([&] { c::CameraSurfaceLease bad((f.directory / ".." / "surface").string()); });
}
void process_death_releases_camera() {
  Fixture f;
  c::CameraSurfaceLease main(f.file.string());
  int ready[2];
  check(::pipe(ready) == 0, "pipe failed");
  const auto child = ::fork();
  check(child >= 0, "fork failed");
  if (child == 0) {
    ::close(ready[0]);
    try {
      c::CameraSurfaceLease camera(f.file.string());
      const char value = camera.acquire_exclusive() ? '1' : '0';
      if (::write(ready[1], &value, 1) != 1) ::_exit(2);
      for (;;)
        ::pause();
    } catch (...) {
      ::_exit(3);
    }
  }
  ::close(ready[1]);
  char value = 0;
  const bool acquired = ::read(ready[0], &value, 1) == 1 && value == '1';
  ::close(ready[0]);
  const bool blocked = !main.acquire_shared();
  ::kill(child, SIGTERM);
  int status = 0;
  check(::waitpid(child, &status, 0) == child, "child reap failed");
  check(acquired && blocked && WIFSIGNALED(status) && main.acquire_shared(),
        "dead native owner did not release kernel lease");
}
int main() try {
  exclusion_and_release();
  generation_and_files();
  process_death_releases_camera();
  validation_keeps_missing_lease_absent();
  validation_preserves_existing_lease_and_lock();
  validation_rejects_unsafe_existing_without_changes();
  validation_rejects_unsafe_parent_and_path();
  std::cout << "7 camera lease suites passed: runtime exclusion and read-only preflight\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
