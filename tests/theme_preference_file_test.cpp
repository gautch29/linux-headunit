// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Compile the production disk owner with read/sync syscall interventions. These
// fixtures use real private files; they do not qualify power-loss durability.
#include <headunit/theme/preference_file.hpp>
#include <headunit/theme/layout_control.hpp>
#include <headunit/theme/ui_worker.hpp>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
const std::string first = "00000000-1111-2222-3333-444444444444\nred\n";
const std::string second = "00000000-1111-2222-3333-555555555555\nred\n";
fs::path target;
enum class Intervention { none, rewrite, replace, interrupt };
Intervention intervention = Intervention::none;
bool injected = false;
std::mutex pause_mutex;
std::condition_variable pause_condition;
bool pause_history = false, history_waiting = false, history_released = false;
fs::path delayed_history;
std::mutex sync_mutex;
std::condition_variable sync_condition;
bool sync_race = false, directory_waiting = false, directory_released = false;
bool file_waiting = false, file_released = false, directory_failure = false;
std::mutex write_mutex;
std::condition_variable write_condition;
bool write_gate = false, write_waiting = false, write_released = false;
bool write_failure = false, write_injected = false;
std::size_t write_size = 0;
void record(const fs::path& path, const std::string& bytes) {
  std::ofstream output(path);
  output << bytes;
  output.close();
  check(bool(output) && !chmod(path.c_str(), 0600), "fixture write failed");
}
}  // namespace
ssize_t fixture_read(int fd, void* bytes, std::size_t bound) {
  {
    struct stat opened{}, named{};
    std::unique_lock lock(pause_mutex);
    if (pause_history && !::fstat(fd, &opened) && !::stat(delayed_history.c_str(), &named) &&
        opened.st_dev == named.st_dev && opened.st_ino == named.st_ino) {
      history_waiting = true;
      pause_condition.notify_all();
      pause_condition.wait(lock, [] { return history_released; });
    }
  }
  if (!injected && intervention == Intervention::interrupt) {
    injected = true;
    errno = EINTR;
    return -1;
  }
  const auto count = ::read(fd, bytes, bound);
  if (injected || intervention == Intervention::none || count <= 0) return count;
  injected = true;
  if (intervention == Intervention::replace) {
    // Retain the old inode's single link: link-count checks alone cannot detect
    // that the name now selects another preference.
    fs::rename(target, target.string() + ".old");
    record(target, second);
  } else {
    struct stat before{};
    check(!fstat(fd, &before), "fixture pre-edit stat failed");
    const int writer = ::open(target.c_str(), O_WRONLY | O_CLOEXEC);
    check(writer >= 0, "fixture writer failed");
    check(::pwrite(writer, second.data(), second.size(), 0) == ssize_t(second.size()),
          "fixture same-size edit failed");
#ifdef __APPLE__
    const timespec dates[]{before.st_atimespec,
                           {before.st_mtimespec.tv_sec + 1, before.st_mtimespec.tv_nsec}};
#else
    const timespec dates[]{before.st_atim, {before.st_mtim.tv_sec + 1, before.st_mtim.tv_nsec}};
#endif
    check(!::futimens(writer, dates), "fixture timestamp advance failed");
    ::close(writer);
  }
  return count;
}
int fixture_fsync(int fd) {
  struct stat info{};
  check(!::fstat(fd, &info), "fixture sync descriptor failed");
  bool fail = false;
  {
    std::unique_lock lock(sync_mutex);
    if (sync_race && S_ISDIR(info.st_mode) && !directory_waiting) {
      directory_waiting = true;
      sync_condition.notify_all();
      sync_condition.wait(lock, [] { return directory_released; });
      fail = directory_failure;
    } else if (sync_race && S_ISREG(info.st_mode) && directory_waiting && !file_waiting) {
      file_waiting = true;
      sync_condition.notify_all();
      sync_condition.wait(lock, [] { return file_released; });
    }
  }
  {
    std::unique_lock lock(write_mutex);
    if (write_gate && S_ISREG(info.st_mode) && info.st_size == ssize_t(write_size) &&
        !write_waiting) {
      write_waiting = true;
      write_condition.notify_all();
      write_condition.wait(lock, [] { return write_released; });
      fail = write_failure;
      write_injected = true;
    }
  }
  const auto result = ::fsync(fd);
  if (fail && !result) {
    errno = EIO;
    return -1;
  }
  return result;
}
// Include headers before replacing only the production syscall use.
#define read fixture_read
#define fsync fixture_fsync
#include "../src/theme/preference_file.cpp"
#undef fsync
#undef read
// Combine the existing controller and disk owners with actual UiWorker capture
// retirement. Controller readback/catalogues and poll dates remain synthetic;
// no signed session, phone confirmation or power-loss durability is exercised.
void confirmed_write_worker_retirement(const fs::path& folder, bool fail_sync) {
  using namespace headunit::theme::script;
  using namespace std::chrono_literals;
  const std::string a = "00000000-1111-2222-3333-444444444444";
  const std::string b = "00000000-1111-2222-3333-555555555555";
  const std::string c = "00000000-1111-2222-3333-666666666666";
  const LayoutControl::Publisher old_publisher{"0123456789abcdef0123456789abcdef", 11};
  const LayoutControl::Publisher new_publisher{"fedcba9876543210fedcba9876543210", 12};
  const auto path = folder / (fail_sync ? "retiring-failed" : "retiring-saved");
  const auto saved = path.string() + ".confirmed.reference:985";
  const auto history = saved + ".palettes";
  const auto old_record = a + "\nred\n";
  const auto new_record = b + "\nblue\n";
  record(saved, old_record);
  const auto contents = [](const fs::path& file) {
    std::ifstream input(file);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  const auto request = [&](const LayoutControl::Publisher& publisher, const std::string& layout,
                           const std::string& palette) {
    const auto temporary = path.string() + ".new";
    record(temporary, "HUL2 " + publisher.nonce + " " +
                          std::to_string(publisher.connection_generation) + "\n" + layout + "\n" +
                          palette + "\n\n\n");
    fs::rename(temporary, path);
  };
  std::atomic<bool> configured = false, retiring = false, retired = false;
  std::thread::id owner_thread, retirement_thread;
  struct Owner {
    std::unique_ptr<LayoutControl> control;
    std::atomic<bool>& retiring;
    std::atomic<bool>& retired;
    std::thread::id& retirement_thread;
    Owner(std::atomic<bool>& entering, std::atomic<bool>& completed, std::thread::id& thread)
        : retiring(entering), retired(completed), retirement_thread(thread) {}
    ~Owner() {
      retirement_thread = std::this_thread::get_id();
      retiring = true;
      control.reset();
      retired = true;
    }
  };
  auto owner = std::make_shared<Owner>(retiring, retired, retirement_thread);
  UiWorker::Configuration config;
  config.displays = {{"Cluster_Display"}};
  config.routes = {{"aabbccdd-0000-0000-0000-000000000002", "Cluster_Display"}};
  config.resources = [owner](std::string_view name) {
    (void)owner;
    check(name == "main.lua", "unexpected worker resource");
    return std::string("return {}");
  };
  config.configure = [&, owner](Channel&) {
    owner_thread = std::this_thread::get_id();
    owner->control = std::make_unique<LayoutControl>(path.string(), "reference:985", old_publisher);
    owner->control->wait_for_preferences();
    LayoutSelection state;
    state.ready = state.idle = true;
    state.active = a;
    state.active_palette = "red";
    state.choices = {{a, "A"}, {b, "B"}, {c, "C"}};
    state.palettes = {{"red", "Red"}, {"blue", "Blue"}};
    std::vector<ClusterSelectionRequest> requests;
    const auto select = [&](const std::optional<ClusterSelectionRequest>& choice) {
      auto result = state;
      if (choice) {
        requests.push_back(*choice);
        result.result = choice->layout == state.active && choice->palette == state.active_palette
                            ? LayoutRequest::selected
                            : LayoutRequest::requested;
      }
      return result;
    };
    owner->control->poll_cluster(select, LayoutControl::Clock::time_point(1s));
    check(requests.size() == 1 && requests.back().layout == a, "initial restore was lost");
    owner->control->wait_for_preferences();
    request(old_publisher, b, "blue");
    owner->control->poll_cluster(select, LayoutControl::Clock::time_point(2s));
    check(contents(saved) == old_record, "pending request was persisted");
    state.active = b;
    state.active_palette = "blue";
    state.ready = false;
    owner->control->poll_cluster(select, LayoutControl::Clock::time_point(3s));
    check(contents(saved) == old_record, "unavailable confirmation was persisted");
    state.ready = true;
    {
      std::lock_guard lock(write_mutex);
      write_gate = true;
      write_waiting = write_released = write_injected = false;
      write_failure = fail_sync;
      write_size = new_record.size();
    }
    owner->control->poll_cluster(select, LayoutControl::Clock::time_point(4s));
    {
      std::unique_lock lock(write_mutex);
      check(write_condition.wait_for(lock, 2s, [] { return write_waiting; }),
            "confirmed file sync was not held");
    }
    check(contents(saved) == old_record, "held file sync replaced the durable preference");
    request(old_publisher, c, "red");
    owner->control->poll_cluster(select, LayoutControl::Clock::time_point(5s));
    check(requests.size() == 3 && requests.back().layout == c, "last pending request was lost");
    configured = true;
  };
  std::unique_ptr<UiWorker> worker;
  struct ReleaseWrite {
    ~ReleaseWrite() {
      {
        std::lock_guard lock(write_mutex);
        write_released = true;
        write_gate = false;
      }
      write_condition.notify_all();
    }
  } release;
  worker = std::make_unique<UiWorker>(111, std::move(config));
  config = {};
  owner.reset();
  const auto until = [&](const auto& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
      check(std::chrono::steady_clock::now() < deadline, "worker retirement fixture timed out");
      std::this_thread::sleep_for(1ms);
    }
  };
  until([&] { return configured || worker->finished(); });
  check(configured, worker->failure_reason().c_str());
  check(owner_thread != std::this_thread::get_id(), "controller left worker owner");
  until([&] { return worker->status().state == UiWorker::State::running || worker->finished(); });
  check(!worker->finished(), "worker faulted before retirement");
  worker->request_stop();
  until([&] { return retiring.load(); });
  check(retirement_thread == owner_thread, "controller retired outside its worker owner");
  check(!worker->finished() && !retired, "finished preceded confirmed disk owner retirement");
  check(worker->status().state == UiWorker::State::stopping, "worker escaped stopping state");
  headunit::DataMessage late;
  late.type = 0x636f6d6d;
  check(!worker->receive(111, late), "retired worker admitted late input");
  check(!worker->send_pending([](const auto&) { return true; }), "retired worker revived output");
  check(contents(saved) == old_record, "retirement manufactured write completion");
  {
    std::lock_guard lock(write_mutex);
    write_released = true;
  }
  write_condition.notify_all();
  until([&] { return worker->finished(); });
  check(retired && worker->status().state == UiWorker::State::stopped,
        "disk completion failed to retire worker");
  worker.reset();
  {
    std::lock_guard lock(write_mutex);
    check(write_injected, "file sync intervention did not finish");
    write_gate = false;
  }
  const auto expected = fail_sync ? old_record : new_record;
  check(contents(saved) == expected, "failed or successful write changed the wrong preference");
  check(contents(history).find(c + " red\n") == std::string::npos,
        "pending successor intent entered colour history");
  for (const auto& entry : fs::directory_iterator(folder))
    check(!entry.path().filename().string().starts_with(".cluster-preference-"),
          "retirement leaked a temporary file");

  LayoutSelection state;
  state.ready = state.idle = true;
  state.active = a;
  state.active_palette = "red";
  state.choices = {{a, "A"}, {c, "C"}};
  state.palettes = {{"red", "Red"}};
  std::vector<ClusterSelectionRequest> requests;
  const auto select = [&](const std::optional<ClusterSelectionRequest>& choice) {
    auto result = state;
    if (choice) {
      requests.push_back(*choice);
      result.result = choice->layout == a ? LayoutRequest::selected : LayoutRequest::unknown;
    }
    return result;
  };
  {
    LayoutControl successor(path.string(), "reference:985", new_publisher);
    successor.wait_for_preferences();
    successor.poll_cluster(select, LayoutControl::Clock::time_point(6s));
    successor.wait_for_preferences();
    check(requests.size() == 1 && requests.back().layout == (fail_sync ? a : b),
          "successor inherited pending intent or failed candidate");
    check(requests.back().palette == (fail_sync ? "red" : ""),
          "successor offered a removed colour");
    request(old_publisher, c, "red");
    successor.poll_cluster(select, LayoutControl::Clock::time_point(7s));
    successor.poll_cluster(select, LayoutControl::Clock::time_point(9s));
    successor.wait_for_preferences();
    check(requests.size() == 1 && contents(saved) == expected,
          "stale request or old failure revived in successor");
    const auto status = contents(path.string() + ".status");
    check(status.find(new_publisher.nonce) != std::string::npos &&
              status.find("\"confirmedActive\":\"" + a + "\"") != std::string::npos,
          "successor publication lost current catalogue ownership");
    check(fail_sync || status.find("Layout is not declared by this pack") != std::string::npos,
          "successor catalogue mismatch lost rejection");
  }
  const auto successor_status = contents(path.string() + ".status");
  check(successor_status.find(new_publisher.nonce) != std::string::npos,
        "retirement erased successor publication");
  {
    LayoutControl foreign(path.string(), "other:986", {new_publisher.nonce, 13});
    foreign.wait_for_preferences();
    foreign.poll_cluster(select, LayoutControl::Clock::time_point(10s));
    foreign.wait_for_preferences();
    check(requests.size() == 1 && !fs::exists(path.string() + ".confirmed.other:986") &&
              contents(saved) == expected,
          "foreign pack inherited confirmation or pending request");
  }
}

int main() try {
  char name[] = "/tmp/headunit-preference-file-XXXXXX";
  check(mkdtemp(name), "fixture directory failed");
  struct Cleanup {
    fs::path path;
    ~Cleanup() { fs::remove_all(path); }
  } cleanup{name};
  target = cleanup.path / "confirmed";
  using headunit::theme::script::PreferenceFile;
  for (const auto mode : {Intervention::none, Intervention::interrupt, Intervention::rewrite,
                          Intervention::replace}) {
    record(target, first);
    intervention = mode;
    injected = false;
    PreferenceFile store(target.string());
    store.wait();
    const auto saved = store.snapshot();
    check(saved.loaded, "startup did not finish");
    if (mode == Intervention::rewrite || mode == Intervention::replace) {
      check(injected, "race intervention did not run");
      check(!saved.data && !saved.error.empty(), mode == Intervention::rewrite
                                                     ? "same-inode edit was admitted"
                                                     : "replaced preference name was admitted");
      check(std::ifstream(target).good(), "rejected read deleted the new preference");
    } else {
      check(saved.data == first && saved.error.empty(), "stable or interrupted read failed");
    }
  }
  intervention = Intervention::none;
  // Hold a completed rename at directory sync while another worker reuses
  // the now-free temporary name. Cleanup must preserve the second inode,
  // including when the first directory sync reports a durability failure.
  for (const bool failed_sync : {false, true}) {
    const auto primary_path = cleanup.path / (failed_sync ? "sync-failed" : "sync-good");
    const auto secondary_path = primary_path.string() + ".palettes";
    PreferenceFile primary(primary_path.string());
    PreferenceFile secondary(secondary_path, 4096);
    primary.wait();
    secondary.wait();
    struct ReleaseSync {
      ~ReleaseSync() {
        {
          std::lock_guard lock(sync_mutex);
          directory_released = file_released = true;
          sync_race = false;
        }
        sync_condition.notify_all();
      }
    } release_sync;
    {
      std::lock_guard lock(sync_mutex);
      sync_race = true;
      directory_waiting = directory_released = file_waiting = file_released = false;
      directory_failure = failed_sync;
    }
    primary.save(first);
    {
      std::unique_lock lock(sync_mutex);
      check(
          sync_condition.wait_for(lock, std::chrono::seconds(2), [] { return directory_waiting; }),
          "first rename was not paused");
    }
    secondary.save(second);
    {
      std::unique_lock lock(sync_mutex);
      check(sync_condition.wait_for(lock, std::chrono::seconds(2), [] { return file_waiting; }),
            "second temporary was not paused");
      directory_released = true;
    }
    sync_condition.notify_all();
    primary.wait();
    const auto temporary =
        cleanup.path / (".cluster-preference-" + std::to_string(getpid()) + "-1-0");
    check(fs::is_regular_file(temporary), "completed rename unlinked another worker's temporary");
    {
      std::lock_guard lock(sync_mutex);
      file_released = true;
      sync_race = false;
    }
    sync_condition.notify_all();
    secondary.wait();
    check(primary.snapshot().error.empty() == !failed_sync,
          "directory sync failure was reported as durable success");
    check(secondary.snapshot().error.empty() && secondary.snapshot().data == second,
          "concurrent history write was lost");
    std::ifstream contents(secondary_path);
    const std::string saved{std::istreambuf_iterator<char>(contents), {}};
    check(saved == second, "concurrent history bytes changed");
  }
  const auto bounded = cleanup.path / "history";
  const std::string larger(4096, 'x');
  record(bounded, larger);
  {
    PreferenceFile legacy(bounded.string());
    legacy.wait();
    check(!legacy.snapshot().data && !legacy.snapshot().error.empty(),
          "legacy reader admitted larger records");
  }
  {
    PreferenceFile history(bounded.string(), 4096);
    history.wait();
    check(history.snapshot().data == larger && history.snapshot().error.empty(),
          "bounded history read failed");
    bool rejected = false;
    try {
      history.save(std::string(4097, 'x'));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "oversized history write was admitted");
    history.save(std::string(1024, 'y'));
  }
  check(std::filesystem::file_size(bounded) == 1024, "retirement lost history write");
  chmod(bounded.c_str(), 0644);
  {
    PreferenceFile exposed(bounded.string(), 4096);
    exposed.wait();
    check(!exposed.snapshot().data && !exposed.snapshot().error.empty(),
          "history reader admitted exposed record");
  }
  for (const std::size_t bound : {0, 4097}) {
    bool rejected = false;
    try {
      PreferenceFile invalid(bounded.string(), bound);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "unbounded preference owner was admitted");
  }
  // Hold the real history reader while a missing primary record is ready.
  // Phone observations must wait for both snapshots rather than overwrite
  // existing history with an edit admitted before the history preload.
  using namespace headunit::theme::script;
  using namespace std::chrono_literals;
  const std::string a = "00000000-1111-2222-3333-444444444444";
  const std::string b = "00000000-1111-2222-3333-555555555555";
  const auto control_path = cleanup.path / "delayed";
  const auto primary_path = control_path.string() + ".confirmed.reference:985";
  delayed_history = primary_path + ".palettes";
  record(delayed_history, "HUP1\n" + a + " red\n" + b + " blue\n");
  auto primary = std::make_shared<PreferenceFile>(primary_path);
  primary->wait();
  {
    std::lock_guard lock(pause_mutex);
    pause_history = true;
  }
  struct ReleaseHistory {
    ~ReleaseHistory() {
      {
        std::lock_guard lock(pause_mutex);
        history_released = true;
      }
      pause_condition.notify_all();
    }
  };
  LayoutControl owner(control_path.string(), "reference:985",
                      {"0123456789abcdef0123456789abcdef", 1}, primary);
  ReleaseHistory release;
  {
    std::unique_lock lock(pause_mutex);
    check(pause_condition.wait_for(lock, 2s, [] { return history_waiting; }),
          "history read was not delayed");
  }
  LayoutSelection state;
  state.ready = state.idle = true;
  state.active = a;
  state.active_palette = "red";
  state.choices = {{a, "A"}, {b, "B"}};
  state.palettes = {{"red", "Red"}, {"blue", "Blue"}};
  const auto select = [&](const std::optional<ClusterSelectionRequest>& request) {
    check(!request, "preload manufactured a layout request");
    return state;
  };
  owner.poll_cluster(select, LayoutControl::Clock::time_point(1s));
  state.active = b;
  state.active_palette = "blue";
  owner.poll_cluster(select, LayoutControl::Clock::time_point(2s));
  primary->wait();
  check(!fs::exists(primary_path), "phone edit was admitted before history preload");
  {
    std::lock_guard lock(pause_mutex);
    history_released = true;
  }
  pause_condition.notify_all();
  owner.wait_for_preferences();
  owner.poll_cluster(select, LayoutControl::Clock::time_point(3s));
  state.active = a;
  state.active_palette = "red";
  owner.poll_cluster(select, LayoutControl::Clock::time_point(4s));
  owner.wait_for_preferences();
  std::ifstream completed(delayed_history);
  const std::string retained{std::istreambuf_iterator<char>(completed), {}};
  check(retained == "HUP1\n" + b + " blue\n" + a + " red\n",
        "delayed preload lost existing confirmed palette");
  confirmed_write_worker_retirement(cleanup.path, false);
  confirmed_write_worker_retirement(cleanup.path, true);
  std::cout << "preference file: stable read, EINTR, same-size edit and named replacement passed\n";
  std::cout << "confirmed preference: actual worker retirement, sync failure and successor "
               "admission passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
