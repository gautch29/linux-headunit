// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/standard_preview_worker.hpp>
#include <headunit/camera_surface_lease.hpp>
#include <headunit/presentation_status.hpp>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace g = headunit::theme::graphics;
namespace v = headunit::theme::video;
using Worker = headunit::StandardPreviewWorker;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F fn) {
  bool failed = false;
  try {
    fn();
  } catch (const std::exception&) {
    failed = true;
  }
  check(failed, "unsafe worker operation accepted");
}
template <class F>
void wait(F ready) {
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (!ready()) {
    check(std::chrono::steady_clock::now() < until, "worker fixture deadline");
    std::this_thread::sleep_for(1ms);
  }
}
std::uint64_t ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
struct Submission {
  std::uint64_t token;
  bool resolved = false, presented = false, bad_clock = false, bad_token = false;
};
struct Output {
  unsigned id = 0, capacity = 0;
  std::thread::id owner;
  std::deque<Submission> pending;
  std::deque<headunit::TouchInput> input;
  bool composed = false, enabled = false;
};
struct Fixture {
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::shared_ptr<Output> output;
  unsigned constructors = 0, destructors = 0, submissions = 0, maximum_pending = 0;
  bool block_constructor = false, constructor_entered = false, block_compose = false,
       compose_entered = false;
  bool compose_failure = false;
  std::uint32_t last_pixel = 0;
  std::filesystem::path directory, lease_file;
  Fixture() {
    char name[] = "/tmp/headunit-standard-worker-XXXXXX";
    const auto* path = ::mkdtemp(name);
    check(path, "worker private fixture directory failed");
    directory = path;
    lease_file = directory / "main-lease";
  }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  Worker::Options options(unsigned capacity = 1, bool show = true) {
    Worker::Options result;
    result.width = 320;
    result.height = 240;
    result.window.max_pending_presentations = capacity;
    result.initially_visible = show;
    result.surface_lease_file = lease_file.string();
    return result;
  }
  unsigned count() const {
    std::lock_guard lock(mutex);
    return submissions;
  }
  unsigned windows() const {
    std::lock_guard lock(mutex);
    return constructors;
  }
  unsigned destroyed() const {
    std::lock_guard lock(mutex);
    return destructors;
  }
  bool composing() const {
    std::lock_guard lock(mutex);
    return compose_entered;
  }
  bool constructing() const {
    std::lock_guard lock(mutex);
    return constructor_entered;
  }
  void hold_compose(bool value) {
    std::lock_guard lock(mutex);
    block_compose = value;
    compose_entered = false;
    wake.notify_all();
  }
  void hold_constructor(bool value) {
    std::lock_guard lock(mutex);
    block_constructor = value;
    constructor_entered = false;
    wake.notify_all();
  }
  void release(bool presented = true, bool bad_clock = false, bool bad_token = false) {
    std::lock_guard lock(mutex);
    check(output && !output->pending.empty(), "no submitted fixture frame");
    auto& item = output->pending.front();
    item.resolved = true;
    item.presented = presented;
    item.bad_clock = bad_clock;
    item.bad_token = bad_token;
  }
  void touch(headunit::TouchInput input) {
    std::lock_guard lock(mutex);
    check(bool(output), "no fixture input surface");
    output->input.push_back(input);
  }
};
Fixture* fixture = nullptr;
struct Current {
  explicit Current(Fixture& value) {
    check(!fixture, "overlapping fixture");
    fixture = &value;
  }
  ~Current() { fixture = nullptr; }
};
headunit::GstPreviewFrames::Frame frame(std::uint64_t pts, std::uint32_t pixel = 0xff0088cc) {
  auto image = std::make_shared<g::Image>();
  image->width = 320;
  image->height = 240;
  image->pixels.assign(320 * 240, pixel);
  return {pts, std::move(image)};
}
void ready(Worker& worker, Worker::Generation generation = 1) {
  wait([&] {
    const auto s = worker.status();
    check(!s.faulted, "fixture presenter faulted before readiness");
    return s.window_ready && s.window == generation;
  });
}
std::vector<headunit::TouchInput> take(Worker& worker) {
  std::vector<headunit::TouchInput> result;
  worker.drain_touches([&](headunit::TouchInput report) {
    result.push_back(report);
    return true;
  });
  return result;
}
}  // namespace

// Explicit test output only. Production GpuCompositor/Wayland receipt code is
// NOT executed here; these tests qualify worker scheduling and ownership.
namespace headunit::theme::graphics {
struct GpuCompositor::Impl {
  std::shared_ptr<Output> output;
};
GpuCompositor::GpuCompositor(std::uint32_t width, std::uint32_t height, WaylandOptions options)
    : impl_(std::make_unique<Impl>()) {
  check(width == 320 && height == 240 && options.accept_touch && options.opaque_background,
        "wrong fixture window configuration");
  auto& f = *fixture;
  std::unique_lock lock(f.mutex);
  check(!f.output, "new GPU window constructed before retiring previous owner");
  f.constructor_entered = true;
  f.wake.notify_all();
  check(f.wake.wait_for(lock, 2s, [&] { return !f.block_constructor; }),
        "blocked constructor fixture timed out");
  auto output = std::make_shared<Output>();
  output->id = ++f.constructors;
  output->capacity = options.max_pending_presentations;
  output->owner = std::this_thread::get_id();
  impl_->output = output;
  f.output = std::move(output);
}
GpuCompositor::~GpuCompositor() {
  auto& f = *fixture;
  std::lock_guard lock(f.mutex);
  check(std::this_thread::get_id() == impl_->output->owner, "GPU destroyed by control owner");
  ++f.destructors;
  f.output.reset();
}
GpuTimings GpuCompositor::compose_scene(const std::shared_ptr<const Layer>& root,
                                        std::uint64_t timeout,
                                        const std::function<void()>& during) {
  check(timeout == 100000000 && !during, "unexpected fixture GPU completion policy");
  check(root && root->children.size() == 1 && root->children.front()->kind == Kind::image,
        "Standard did not compose owned source image");
  const auto image = root->children.front()->image;
  check(image && image->width == 320 && image->height == 240 && root->background.a == 1,
        "wrong image scene");
  auto& f = *fixture;
  std::unique_lock lock(f.mutex);
  check(std::this_thread::get_id() == impl_->output->owner, "GPU composed on control owner");
  f.compose_entered = true;
  f.wake.notify_all();
  check(f.wake.wait_for(lock, 2s, [&] { return !f.block_compose; }),
        "blocked compose fixture timed out");
  if (f.compose_failure) throw std::runtime_error("fixture private path must never escape");
  f.last_pixel = image->pixels.front();
  impl_->output->composed = true;
  return {};
}
void GpuCompositor::submit(std::uint64_t token, Image* readback, WindowContent content) {
  auto& f = *fixture;
  std::lock_guard lock(f.mutex);
  auto& output = *impl_->output;
  check(!readback && content == WindowContent::rendered_scene && output.composed &&
            output.pending.size() < output.capacity,
        "fixture readback, missing composition or queue overflow");
  output.composed = false;
  output.pending.push_back({token});
  ++f.submissions;
  f.maximum_pending = std::max(f.maximum_pending, unsigned(output.pending.size()));
}
bool GpuCompositor::presentation_slot_available() const {
  std::lock_guard lock(fixture->mutex);
  return impl_->output->pending.size() < impl_->output->capacity;
}
std::optional<PresentationReceipt> GpuCompositor::poll_presentation() {
  std::lock_guard lock(fixture->mutex);
  auto& output = *impl_->output;
  if (output.pending.empty() || !output.pending.front().resolved) return {};
  const auto item = output.pending.front();
  output.pending.pop_front();
  const auto now = ns();
  PresentationReceipt result;
  result.token = item.bad_token ? item.token + 100 : item.token;
  result.clock_id = 1;
  result.received_ns = now;
  result.seconds = now / 1000000000;
  result.nanoseconds = now % 1000000000;
  result.presented = item.presented;
  result.refresh_ns = 16666667;
  if (item.presented && !item.bad_clock) result.correlation = v::ClockCorrelation{1, now, now, now};
  return result;
}
std::vector<headunit::TouchInput> GpuCompositor::take_touches(bool enabled) {
  std::lock_guard lock(fixture->mutex);
  auto& output = *impl_->output;
  output.enabled = enabled;
  std::vector<headunit::TouchInput> result;
  for (const auto& input : output.input)
    if (enabled || !input.pressed) result.push_back(input);
  output.input.clear();
  return result;
}
}  // namespace headunit::theme::graphics
namespace {
void heartbeat_is_owned_by_worker() {
  Fixture f;
  Current current(f);
  Worker worker(f.options());
  ready(worker);
  wait([&] { return worker.status().observed_monotonic_ns > 0; });
  check(worker.arm_source(1, {}), "heartbeat initial arm failed");
  f.hold_compose(true);
  check(worker.offer(1, frame(10)), "heartbeat source offer failed");
  wait([&] { return f.composing(); });
  const auto observed = worker.status().observed_monotonic_ns;
  check(observed > 0 && observed <= headunit::presentation_monotonic_ns(),
        "worker heartbeat is not actual CLOCK_MONOTONIC");
  std::this_thread::sleep_for(30ms);
  for (unsigned count = 0; count < 100; ++count)
    check(worker.status().observed_monotonic_ns == observed,
          "control reads renewed a blocked worker heartbeat");
  f.hold_compose(false);
  wait([&] { return worker.status().observed_monotonic_ns > observed; });
}
void initial_receipt_and_static_retry() {
  Fixture f;
  Current current(f);
  Worker worker(f.options());
  ready(worker);
  check(!worker.status().touch_ready && worker.input_fd() >= 0, "window ACK enabled input");
  check(worker.arm_source(1, {}), "initial source arm failed");
  check(worker.offer(1, frame(10)), "initial source offer failed");
  wait([&] { return f.count() == 1; });
  f.touch({true, 90, 100});
  std::this_thread::sleep_for(20ms);
  check(take(worker).empty() && !worker.status().touch_ready,
        "unpresented source enabled physical input");
  f.release(false);
  wait([&] { return f.count() == 2; });
  check(
      worker.status().accepted == 1 && worker.status().retried == 1 && !worker.status().touch_ready,
      "discard retry fabricated source arrival or input readiness");
  f.release();
  wait([&] { return worker.status().touch_ready; });
  const auto state = worker.status();
  check(state.presented == 1 && state.presented_arrival_running_ns == 10,
        "actual fixture receipt not attributed to source arrival");
  f.touch({true, 120, 130});
  std::vector<headunit::TouchInput> reports;
  wait([&] {
    auto batch = take(worker);
    reports.insert(reports.end(), batch.begin(), batch.end());
    return !reports.empty();
  });
  check(reports.front().pressed && reports.front().x == 120 && reports.front().y == 130,
        "fresh static surface lost touch");
}
void retained_source_bounds() {
  Fixture f;
  Current current(f);
  Worker worker(f.options(2));
  ready(worker);
  check(worker.arm_source(1, {}), "initial arm failed");
  auto first = frame(10);
  std::weak_ptr<const g::Image> first_lease = first.image;
  check(worker.offer(1, std::move(first)), "first offer failed");
  wait([&] { return f.count() == 1; });
  check(worker.offer(1, frame(11)), "second offer failed");
  wait([&] { return f.count() == 2; });
  auto third = frame(12);
  std::weak_ptr<const g::Image> third_lease = third.image;
  check(worker.offer(1, std::move(third)), "mailbox third offer failed");
  check(worker.offer(1, frame(13, 0xffabcdef)), "mailbox replacement failed");
  wait([&] { return third_lease.expired(); });
  check(!first_lease.expired() && worker.status().coalesced == 1 && f.count() == 2,
        "pending source freed or mailbox unbounded");
  check(!worker.offer(1, frame(13)) && !worker.offer(1, frame(9)),
        "duplicate or backward source accepted");
  f.release();
  wait([&] { return f.count() == 3; });
  {
    std::lock_guard lock(f.mutex);
    check(f.maximum_pending == 2 && f.last_pixel == 0xffabcdef,
          "newest copied image or pending bound lost");
  }
  wait([&] { return first_lease.expired(); });
}
void hide_return_lease_and_touch() {
  Fixture f;
  Current current(f);
  Worker worker(f.options());
  headunit::camera::CameraSurfaceLease camera(f.lease_file.string());
  ready(worker);
  check(!camera.acquire_exclusive(), "main window lacks shared lease");
  check(worker.arm_source(1, {}), "initial source arm failed");
  auto source = frame(10);
  std::weak_ptr<const g::Image> pixels = source.image;
  check(worker.offer(1, std::move(source)), "source offer failed");
  wait([&] { return f.count() == 1; });
  f.release();
  wait([&] { return worker.status().touch_ready; });
  f.touch({true, 70, 80});
  std::vector<headunit::TouchInput> press;
  wait([&] {
    press = take(worker);
    return !press.empty();
  });
  check(press.back().pressed, "held press not delivered");
  const auto hidden = worker.request_window(false);
  unsigned rejected_releases = 0;
  check(worker.drain_touches([&](headunit::TouchInput report) {
    check(!report.pressed && report.x == 70 && report.y == 80,
          "backpressured cancellation lost delivered position");
    ++rejected_releases;
    return false;
  }) == 0 && rejected_releases == 1,
        "backpressured release was not retained");
  const auto release = take(worker);
  check(release.size() == 1 && !release[0].pressed && release[0].x == 70,
        "withdrawal failed to release delivered phone contact");
  check(!worker.status().touch_ready && !worker.offer(1, frame(11)),
        "retired source/input revived");
  wait([&] { return worker.status().retired >= hidden; });
  check(pixels.expired(), "withdrawn image lease retained");
  check(camera.acquire_exclusive(), "withdrawn window did not release main lease");
  const auto returned = worker.request_window(true);
  std::this_thread::sleep_for(30ms);
  check(!worker.status().window_ready && !worker.arm_source(returned, 100),
        "camera exclusive lease bypassed");
  camera.release();
  ready(worker, returned);
  rejects([&] { worker.arm_source(returned, {}); });
  check(worker.arm_source(returned, 100) && !worker.arm_source(returned, 90),
        "return boundary not pinned once");
  check(!worker.offer(returned, frame(99)) && !worker.offer(returned, frame(100)) &&
            !worker.offer(1, frame(101)),
        "old/equal source or retired generation admitted");
  check(worker.offer(returned, frame(101)), "new original source arrival rejected");
  wait([&] { return f.count() == 2; });
  check(!worker.status().touch_ready,
        "return window/source submission enabled touch before receipt");
  f.release();
  wait([&] { return worker.status().touch_ready; });
  check(worker.status().presented_arrival_running_ns == 101, "return receipt matched wrong source");
}
void rapid_changes_during_gpu_work() {
  Fixture f;
  Current current(f);
  Worker worker(f.options());
  ready(worker);
  check(worker.arm_source(1, {}), "initial arm failed");
  f.hold_compose(true);
  auto source = frame(10);
  std::weak_ptr<const g::Image> old = source.image;
  check(worker.offer(1, std::move(source)), "initial offer failed");
  wait([&] { return f.composing(); });
  const auto began = std::chrono::steady_clock::now();
  const auto hidden = worker.request_window(false), returned = worker.request_window(true);
  check(returned == hidden + 1 && std::chrono::steady_clock::now() - began < 50ms,
        "GPU work blocked control request");
  check(!worker.arm_source(1, {}), "old window ACK could arm returned source");
  f.hold_compose(false);
  ready(worker, returned);
  check(f.count() == 0 && f.windows() == 2 && f.destroyed() == 1 && old.expired(),
        "rapid hide/show skipped GPU retirement or submitted obsolete source");
  check(worker.arm_source(returned, 100), "new source generation not armable");
}
void suspension_during_composition_is_sticky() {
  Fixture f;
  Current current(f);
  Worker worker(f.options());
  ready(worker);
  headunit::camera::CameraSurfaceLease camera(f.lease_file.string());
  check(worker.arm_source(1, {}), "initial arm failed");
  f.hold_compose(true);
  auto drawing = frame(10);
  std::weak_ptr<const g::Image> drawing_pixels = drawing.image;
  check(worker.offer(1, std::move(drawing)), "drawing source offer failed");
  wait([&] { return f.composing(); });
  auto queued = frame(11);
  std::weak_ptr<const g::Image> queued_pixels = queued.image;
  check(worker.offer(1, std::move(queued)), "queued source offer failed");
  check(worker.suspend_source(1), "current source suspension failed");
  const auto suspended = worker.status();
  check(suspended.requested == 1 && suspended.window == 1 && suspended.window_ready &&
            suspended.source_suspended && !suspended.source_armed && !suspended.touch_ready &&
            !suspended.presented_arrival_running_ns && !suspended.faulted,
        "source suspension retired the window or retained source authority");
  check(queued_pixels.expired() && !drawing_pixels.expired() && !camera.acquire_exclusive(),
        "suspension retained queued pixels, freed active drawing or released the surface lease");
  check(!worker.arm_source(1, {}) && !worker.arm_source(1, 100) && !worker.offer(1, frame(12)),
        "suspended generation accepted rearm or another source");
  check(!worker.suspend_source(0) && worker.status().source_suspended,
        "obsolete suspension changed the current source generation");
  f.hold_compose(false);
  wait([&] { return drawing_pixels.expired(); });
  check(f.count() == 0 && f.windows() == 1 && f.destroyed() == 0 && !worker.status().faulted,
        "in-flight composition submitted after suspension or retired its held window");

  // Even a visible-to-visible request must rebuild the suspended window and
  // provide a new decoder-output boundary before another source can qualify.
  const auto returned = worker.request_window(true);
  ready(worker, returned);
  check(returned == 2 && f.windows() == 2 && f.destroyed() == 1 &&
            !worker.status().source_suspended && !worker.status().touch_ready,
        "new request reused the suspended window or retained input readiness");
  check(!worker.suspend_source(1) && worker.arm_source(returned, 100),
        "old source suspension affected the fresh generation");
  check(!worker.offer(returned, frame(100)) && !worker.offer(1, frame(101)),
        "fresh generation admitted the boundary or obsolete source generation");
  check(worker.offer(returned, frame(101)), "fresh return source rejected");
  wait([&] { return f.count() == 1; });
  check(!worker.status().touch_ready, "new source enabled input without a receipt");
  f.release();
  wait([&] { return worker.status().touch_ready; });
  const auto recovered = worker.status();
  check(recovered.presented == 1 && recovered.submitted == 1 &&
            recovered.presented_arrival_running_ns == 101,
        "fresh generation recovery reused suspended source or presentation authority");
}
void suspension_rejects_old_receipts() {
  Fixture f;
  Current current(f);
  Worker worker(f.options(2));
  ready(worker);
  headunit::camera::CameraSurfaceLease camera(f.lease_file.string());
  check(worker.arm_source(1, {}), "initial arm failed");
  check(worker.offer(1, frame(10)), "initial source offer failed");
  wait([&] { return f.count() == 1; });
  f.release();
  wait([&] { return worker.status().touch_ready; });
  f.touch({true, 80, 90});
  std::vector<headunit::TouchInput> pressed;
  wait([&] {
    pressed = take(worker);
    return !pressed.empty();
  });
  check(pressed.size() == 1 && pressed[0].pressed, "pre-suspension contact not delivered");

  auto first = frame(11);
  std::weak_ptr<const g::Image> first_pixels = first.image;
  check(worker.offer(1, std::move(first)), "first pending source rejected");
  wait([&] { return f.count() == 2; });
  auto second = frame(12);
  std::weak_ptr<const g::Image> second_pixels = second.image;
  check(worker.offer(1, std::move(second)), "second pending source rejected");
  wait([&] { return f.count() == 3; });
  auto queued = frame(13);
  std::weak_ptr<const g::Image> queued_pixels = queued.image;
  check(worker.offer(1, std::move(queued)) && worker.suspend_source(1),
        "pending source suspension failed");
  check(queued_pixels.expired() && !first_pixels.expired() && !second_pixels.expired() &&
            !camera.acquire_exclusive(),
        "suspension lost pending source leases or released the live surface");
  check(!worker.arm_source(1, 200), "pending old receipts allowed same-generation rearm");
  unsigned rejected_releases = 0;
  check(worker.drain_touches([&](headunit::TouchInput report) {
    check(!report.pressed && report.x == 80 && report.y == 90,
          "suspension cancellation lost contact position");
    ++rejected_releases;
    return false;
  }) == 0 && rejected_releases == 1,
        "suspension lost a backpressured release");

  // Both dispositions are genuine resolved receipts for old sources. Neither
  // may restore readiness or schedule a retry while that source is suspended.
  f.release();
  wait([&] { return first_pixels.expired(); });
  f.release(false);
  wait([&] { return second_pixels.expired(); });
  const auto suspended = worker.status();
  check(suspended.source_suspended && !suspended.source_armed && !suspended.touch_ready &&
            !suspended.presented_arrival_running_ns && suspended.presented == 1 &&
            suspended.discarded == 0 && suspended.retried == 0 && f.count() == 3 &&
            !suspended.faulted,
        "old receipt revived suspended source authority, fabricated progress or retried its image");
  const auto released = take(worker);
  check(released.size() == 1 && !released[0].pressed && released[0].x == 80 && released[0].y == 90,
        "resolved old receipt lost the retained cancellation release");

  const auto returned = worker.request_window(true);
  ready(worker, returned);
  check(worker.arm_source(returned, 200) && !worker.offer(returned, frame(200)) &&
            !worker.offer(1, frame(201)),
        "returned source accepted old boundary or generation");
  check(worker.offer(returned, frame(201)), "new generation source offer failed");
  wait([&] { return f.count() == 4; });
  check(!worker.status().touch_ready, "new window reused the old actual receipt");
  f.release();
  wait([&] { return worker.status().touch_ready; });
  const auto recovered = worker.status();
  check(recovered.presented == 2 && recovered.presented_arrival_running_ns == 201 &&
            recovered.retried == 0 && !recovered.source_suspended,
        "source recovery failed to qualify its own fresh receipt");
}
void stale_constructor_ack() {
  Fixture f;
  Current current(f);
  Worker worker(f.options());
  ready(worker);
  const auto hidden = worker.request_window(false);
  wait([&] { return worker.status().retired >= hidden; });
  f.hold_constructor(true);
  const auto old = worker.request_window(true);
  wait([&] { return f.constructing(); });
  worker.request_window(false);
  const auto current_generation = worker.request_window(true);
  check(!worker.arm_source(old, 100), "uncompleted obsolete constructor ACK accepted");
  f.hold_constructor(false);
  ready(worker, current_generation);
  check(f.windows() == 3 && f.destroyed() == 2 && !worker.arm_source(old, 100),
        "stale constructor surface survived latest request");
  check(worker.arm_source(current_generation, 100), "current constructor not armable");
}
void output_fault_withdraws() {
  for (unsigned fault = 0; fault < 3; ++fault) {
    Fixture f;
    Current current(f);
    Worker worker(f.options());
    ready(worker);
    headunit::camera::CameraSurfaceLease camera(f.lease_file.string());
    check(worker.arm_source(1, {}), "initial arm failed");
    if (fault == 0) {
      std::lock_guard lock(f.mutex);
      f.compose_failure = true;
    }
    auto source = frame(10);
    std::weak_ptr<const g::Image> held = source.image;
    check(worker.offer(1, std::move(source)), "source offer failed");
    if (fault) {
      wait([&] { return f.count() == 1; });
      f.release(true, fault == 1, fault == 2);
    }
    wait([&] { return worker.status().faulted; });
    const auto state = worker.status();
    check(!state.touch_ready && !state.window_ready && !state.source_armed &&
              state.error.size() < 128 && state.error.find("private") == std::string::npos,
          "output fault retained visibility/input or leaked dependency detail");
    check(camera.acquire_exclusive() && held.expired() && f.destroyed() == 1,
          "fault retained GPU/source/lease owner");
    check(!worker.offer(1, frame(11)) && take(worker).empty(), "faulted source became live");
    rejects([&] { worker.request_window(true); });
  }
}
void repeated_discard_is_bounded() {
  Fixture f;
  Current current(f);
  Worker worker(f.options());
  ready(worker);
  check(worker.arm_source(1, {}), "initial arm failed");
  check(worker.offer(1, frame(10)), "source offer failed");
  for (unsigned count = 1; count <= 4; ++count) {
    wait([&] { return f.count() == count; });
    f.release(false);
  }
  wait([&] { return worker.status().faulted; });
  const auto state = worker.status();
  check(state.accepted == 1 && state.submitted == 4 && state.retried == 3 && state.presented == 0 &&
            !state.touch_ready,
        "discard retry unbounded or reported as fresh presentation");
}
void replaced_lease_retires_window() {
  Fixture f;
  Current current(f);
  Worker worker(f.options());
  ready(worker);
  const auto old = f.directory / "old-lease";
  std::filesystem::rename(f.lease_file, old);
  headunit::camera::CameraSurfaceLease replacement(f.lease_file.string());
  wait([&] { return worker.status().faulted; });
  check(!worker.status().window_ready && !worker.status().touch_ready && f.destroyed() == 1,
        "replaced surface lease retained active phone window");
  check(replacement.acquire_exclusive(), "retired old inode still blocked new lease");
}
void hidden_start_and_shutdown() {
  Fixture f;
  Current current(f);
  headunit::camera::CameraSurfaceLease camera(f.lease_file.string());
  auto worker = std::make_unique<Worker>(f.options(1, false));
  wait([&] { return worker->status().retired == 1; });
  check(f.windows() == 0 && camera.acquire_exclusive(), "hidden startup built a main surface");
  const auto generation = worker->request_window(true);
  camera.release();
  ready(*worker, generation);
  rejects([&] { worker->arm_source(generation, {}); });
  check(worker->arm_source(generation, 0), "hidden startup lacks genuine return boundary");
  auto source = frame(1);
  std::weak_ptr<const g::Image> image = source.image;
  check(worker->offer(generation, std::move(source)), "hidden startup source offer failed");
  wait([&] { return f.count() == 1; });
  worker.reset();
  check(f.destroyed() == 1 && image.expired() && camera.acquire_exclusive(),
        "worker shutdown outlived GPU/source/lease");
}
void invalid_options_and_pixels() {
  Fixture f;
  Current current(f);
  auto options = f.options();
  options.width = 8192;
  rejects([&] { Worker worker(options); });
  options = f.options();
  options.window.readback_render_node = "/fixture";
  rejects([&] { Worker worker(options); });
  options = f.options();
  options.window.defer_scene_fence = true;
  rejects([&] { Worker worker(options); });
  options = f.options();
  options.window.max_pending_presentations = 3;
  rejects([&] { Worker worker(options); });
  Worker worker(f.options());
  ready(worker);
  check(worker.arm_source(1, {}), "initial arm failed");
  rejects([&] { worker.offer(1, {10, {}}); });
  auto bad = std::make_shared<g::Image>();
  bad->width = 320;
  bad->height = 240;
  rejects([&] { worker.offer(1, {10, bad}); });
  check(worker.status().accepted == 0 && f.count() == 0, "bad pixel storage entered GPU queue");
}
}  // namespace
int main() {
  try {
    heartbeat_is_owned_by_worker();
    initial_receipt_and_static_retry();
    retained_source_bounds();
    hide_return_lease_and_touch();
    rapid_changes_during_gpu_work();
    suspension_during_composition_is_sticky();
    suspension_rejects_old_receipts();
    stale_constructor_ack();
    output_fault_withdraws();
    repeated_discard_is_bounded();
    replaced_lease_retires_window();
    hidden_start_and_shutdown();
    invalid_options_and_pixels();
    std::cout << "13 Standard worker groups passed (explicit fixture output; no GPU/Wayland "
                 "qualification)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
