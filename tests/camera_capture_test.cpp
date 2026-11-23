// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Exercises the real capture worker, mailbox and leases with an injected
// driver. It neither opens a camera nor imports a DMA buffer into a GPU.
#include <headunit/camera_capture.hpp>
#include <headunit/theme/camera_input.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sstream>

namespace c = headunit::camera;
namespace v = headunit::theme::video;
namespace {
constexpr std::uint64_t now = 1000000000, first = 900000000;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void eventually(F predicate, const char* why) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= end) throw std::runtime_error(why);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
template <class F>
void rejects(F action) {
  bool failed = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    failed = true;
  }
  check(failed, "invalid capture owner/configuration was accepted");
}
c::Configuration config(c::CaptureDelivery delivery = c::CaptureDelivery::cpu_copy) {
  c::Configuration result;
  result.enabled = true;
  result.manual_activation = true;
  result.source = c::SourceKind::v4l2;
  result.width = 64;
  result.height = 64;
  result.canvas_width = 64;
  result.canvas_height = 64;
  result.destination = {0, 0, 64, 64};
  result.v4l2 =
      c::V4l2Configuration{"/dev/video0",
                           delivery == c::CaptureDelivery::cpu_copy ? c::CapturePixelFormat::rgb24
                                                                    : c::CapturePixelFormat::yuyv,
                           delivery};
  return result;
}
c::CaptureDeviceFormat format(const c::Configuration& cfg) {
  return {{64, 64, cfg.v4l2->delivery == c::CaptureDelivery::cpu_copy ? 192U : 128U,
           cfg.v4l2->pixel_format, v::YuvMatrix::bt709, true},
          cfg.v4l2->delivery,
          4,
          std::size_t(cfg.v4l2->delivery == c::CaptureDelivery::cpu_copy ? 192 : 128) * 64,
          1,
          30};
}
struct State {
  std::mutex mutex;
  std::condition_variable wake;
  c::CaptureDeviceFormat negotiated;
  std::deque<c::CaptureSample> samples;
  std::thread::id owner;
  unsigned driver_calls = 0, stops = 0;
  bool wrong_thread = false, block_take = false, take_entered = false, block_stop = false,
       stop_entered = false, unknown_take_error = false;
  std::string start_error, take_error;
  std::atomic<unsigned> released{0};
  explicit State(const c::Configuration& cfg) : negotiated(format(cfg)) {}
  void mark() {
    if (owner == std::thread::id{}) owner = std::this_thread::get_id();
    if (owner != std::this_thread::get_id()) wrong_thread = true;
    ++driver_calls;
  }
  void push(c::CaptureSample sample) {
    std::lock_guard lock(mutex);
    samples.push_back(std::move(sample));
  }
};
class Driver final : public c::CaptureDevice {
  std::shared_ptr<State> state;

 public:
  explicit Driver(std::shared_ptr<State> s) : state(std::move(s)) {}
  c::CaptureDeviceFormat start() override {
    std::lock_guard lock(state->mutex);
    state->mark();
    if (!state->start_error.empty()) throw std::runtime_error(state->start_error);
    return state->negotiated;
  }
  std::optional<c::CaptureSample> take() override {
    std::unique_lock lock(state->mutex);
    state->mark();
    state->take_entered = true;
    state->wake.notify_all();
    // Deliberately stalled fake driver proves poll does not join its work.
    state->wake.wait(lock, [&] { return !state->block_take; });
    if (state->unknown_take_error) throw 7;
    if (!state->take_error.empty()) throw std::runtime_error(state->take_error);
    if (state->samples.empty()) return {};
    auto result = std::move(state->samples.front());
    state->samples.pop_front();
    return result;
  }
  void reclaim() override {
    std::lock_guard lock(state->mutex);
    state->mark();
  }
  void stop() noexcept override {
    std::unique_lock lock(state->mutex);
    state->mark();
    state->stop_entered = true;
    state->wake.notify_all();
    state->wake.wait(lock, [&] { return !state->block_stop; });
    ++state->stops;
  }
};
auto factory(const std::shared_ptr<State>& s) {
  return [s] {
    return std::make_unique<Driver>(s);
  };
}
auto clock() {
  return [] {
    return now;
  };
}
c::CaptureSample rgb(std::uint32_t serial, std::uint64_t captured, std::uint8_t value = 23) {
  auto storage = std::make_shared<std::vector<std::uint8_t>>(64 * 64 * 3, value);
  c::CaptureSample result;
  result.driver_sequence = serial;
  result.capture_monotonic_ns = captured;
  result.packed = *storage;
  result.lifetime = storage;
  return result;
}
struct Returned {
  std::shared_ptr<State> state;
  ~Returned() { ++state->released; }  // No driver call on the releasing thread.
};
c::CaptureSample dma(const std::shared_ptr<State>& state, std::uint32_t serial,
                     std::uint64_t captured) {
  auto backing = std::make_shared<Returned>();
  backing->state = state;
  auto native = std::make_shared<v::NativeFrame>();
  native->metadata = {0, captured, 64, 64};
  native->storage_width = 64;
  native->storage_height = 64;
  native->fourcc = std::uint32_t('Y') | (std::uint32_t('U') << 8) | (std::uint32_t('Y') << 16) |
                   (std::uint32_t('V') << 24);
  native->modifier = 0;
  native->plane_count = 1;
  native->planes[0] = {17, 0, 128};
  native->matrix = v::YuvMatrix::bt709;
  native->full_range = true;
  native->lifetime = backing;
  c::CaptureSample result;
  result.driver_sequence = serial;
  result.capture_monotonic_ns = captured;
  result.reference = c::CaptureReference::start_of_exposure;
  result.native = std::move(native);
  return result;
}
void isolation_and_missing() {
  const auto cfg = config();
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  state->block_take = true;
  c::CaptureProvider provider(cfg, queue, factory(state), clock());
  eventually(
      [&] {
        std::lock_guard lock(state->mutex);
        return state->take_entered;
      },
      "capture worker did not start");
  c::Queue other(cfg);
  rejects([&] { provider.poll(now, other); });
  const auto began = std::chrono::steady_clock::now();
  const bool changed = provider.poll(now, queue);
  const auto elapsed = std::chrono::steady_clock::now() - began;
  {
    std::lock_guard lock(state->mutex);
    state->block_take = false;
  }
  state->wake.notify_all();
  check(!changed && elapsed < std::chrono::milliseconds(100),
        "presentation poll waited for a camera driver");
  check(!provider.statistics().last_dequeued && !provider.statistics().last_published,
        "missing camera fabricated acquisition timing");
  check(queue.availability(now) == c::Availability::missing && !queue.stage(now),
        "absent camera acquired a fabricated image");
  std::lock_guard lock(state->mutex);
  check(!state->wrong_thread && state->owner != std::this_thread::get_id(),
        "driver calls escaped the capture worker");
}
void latest_and_copy() {
  const auto cfg = config();
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  for (unsigned i = 0; i < 4; ++i)
    state->push(rgb(i, first + i, std::uint8_t(30 + i)));
  // Some drivers report an allocation tail beyond stride × height. Its
  // sentinel bytes must not become pixels or invalidate a complete capture.
  state->negotiated.buffer_storage_bytes += 64;
  auto tail = std::make_shared<std::vector<std::uint8_t>>(64 * 64 * 3 + 64, 33);
  std::fill(tail->end() - 64, tail->end(), 250);
  state->samples.back().packed = *tail;
  state->samples.back().lifetime = tail;
  c::CaptureProvider provider(cfg, queue, factory(state), clock());
  eventually([&] { return provider.statistics().published == 1; },
             "latest capture did not publish");
  auto frame = queue.stage(now);
  check(frame && frame->acquisition && frame->acquisition->width == 64 &&
            frame->acquisition->height == 64 && frame->acquisition->stride_bytes == 192 &&
            frame->acquisition->driver_sequence == 3 &&
            frame->acquisition->published_monotonic_ns == now &&
            !frame->acquisition->adaptation_fit,
        "actual source acquisition metadata missing from copied frame");
  check(frame && frame->capture_monotonic_ns == first + 3 && !frame->simulated &&
            frame->capture_reference == c::CaptureReference::end_of_frame &&
            frame->image->pixels.front() == 0xff212121,
        "latest CPU frame lost content or original acquisition metadata");
  check(provider.statistics().superseded == 3 && provider.poll(now, queue) &&
            !provider.poll(now, queue),
        "worker replayed its capture backlog or lost change observation");
  check(queue.complete(frame->sequence, true) && queue.current(now) == frame,
        "CPU capture became visible without its real receipt");
  check(!queue.current(first + 150000004),
        "old camera pixels remained current beyond their acquisition limit");
}
void dma_lifetime_and_stop() {
  const auto cfg = config(c::CaptureDelivery::dma_buf);
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  state->push(dma(state, 10, first));
  std::shared_ptr<const c::Frame> gpu;
  {
    c::CaptureProvider provider(cfg, queue, factory(state), clock());
    eventually([&] { return provider.statistics().published == 1; },
               "native capture did not publish");
    gpu = queue.stage(now);
    check(gpu && gpu->native && !gpu->image && gpu->native->metadata.timestamp_ns == first &&
              !gpu->native->metadata.sender_ntp &&
              gpu->capture_reference == c::CaptureReference::start_of_exposure,
          "DMA capture changed clock domains, timestamp reference or storage");
    queue.complete(gpu->sequence, true);
  }
  check(state->released == 0 && queue.current(now) == gpu,
        "stopping capture destroyed in-flight native backing");
  queue.discard();
  check(state->released == 0 && queue.statistics().outstanding_leases == 1,
        "withdrawal released a frame retained by the GPU");
  unsigned calls;
  {
    std::lock_guard lock(state->mutex);
    calls = state->driver_calls;
    check(state->stops && !state->wrong_thread, "driver stop escaped its owner");
  }
  std::thread release([held = std::move(gpu)]() mutable { held.reset(); });
  release.join();
  check(state->released == 1 && queue.statistics().outstanding_leases == 0,
        "native lifetime leaked after its final user released it");
  std::lock_guard lock(state->mutex);
  check(state->driver_calls == calls, "a GPU release invoked a camera driver");
}
void corruption_and_stale() {
  const auto cfg = config();
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  c::CaptureSample broken;
  broken.corrupted = true;
  broken.reference = static_cast<c::CaptureReference>(99);
  state->push(std::move(broken));
  state->push(rgb(0xffffffffU, 1));
  c::CaptureProvider provider(cfg, queue, factory(state), clock());
  eventually(
      [&] {
        auto s = provider.statistics();
        return s.corrupted == 1 && s.stale == 1;
      },
      "corrupt/stale sample not discarded");
  check(!provider.failure() && queue.availability(now) == c::Availability::missing,
        "corruption refreshed visibility or killed recoverable capture");
  state->push(rgb(0, first));  // Genuine forward driver wrap, not restarted sequence.
  eventually([&] { return provider.statistics().published == 1; },
             "fresh frame after corruption/driver wrap did not recover");
  auto frame = queue.stage(now);
  check(frame && frame->capture_monotonic_ns == first, "recovery retimestamped old video");
}
void bounded_backpressure() {
  const auto cfg = config();
  c::Queue queue(cfg);
  std::vector<std::shared_ptr<const c::Frame>> held;
  for (unsigned i = 0; i < 4; ++i) {
    headunit::theme::graphics::Image image{64, 64, std::vector<std::uint32_t>(4096, 0xff112233)};
    check(queue.offer_image(image, first + i, now, false, c::CaptureReference::end_of_frame),
          "four-slot lease pool filled early");
    auto frame = queue.stage(now);
    queue.complete(frame->sequence, true);
    held.push_back(std::move(frame));
    queue.discard();
  }
  auto state = std::make_shared<State>(cfg);
  state->push(rgb(1, first + 10));
  c::CaptureProvider provider(cfg, queue, factory(state), clock());
  eventually([&] { return provider.statistics().backpressure == 1; },
             "retained display leases did not bound capture publication");
  check(!provider.statistics().published && queue.statistics().outstanding_leases == 4,
        "capture exceeded its four-lease bound");
  check(provider.statistics().last_dequeued && !provider.statistics().last_published &&
            provider.statistics().maximum_publication_age_ns == 0,
        "backpressured capture fabricated successful publication timing");
  held.pop_back();
  state->push(rgb(2, first + 20));
  eventually([&] { return provider.statistics().published == 1; },
             "released display lease did not allow a genuinely new capture");
  auto frame = queue.stage(now);
  check(frame && frame->capture_monotonic_ns == first + 20,
        "backpressure replayed the rejected capture");
}
void invalid_samples() {
  for (unsigned kind = 0; kind < 7; ++kind) {
    const auto cfg = config();
    c::Queue queue(cfg);
    auto state = std::make_shared<State>(cfg);
    state->push(rgb(10, first));
    c::CaptureProvider provider(cfg, queue, factory(state), clock());
    eventually([&] { return provider.statistics().published == 1; },
               "initial valid camera frame missing");
    auto held = queue.stage(now);
    queue.complete(held->sequence, true);
    auto bad = rgb(11, first + 1);
    switch (kind) {
      case 0:
        bad.capture_monotonic_ns = now + 1;
        break;
      case 1:
        bad.capture_monotonic_ns = first;
        break;
      case 2:
        bad.driver_sequence = 10;
        break;
      case 3:
        bad.reference = c::CaptureReference::start_of_exposure;
        break;
      case 4:
        bad.packed = bad.packed.first(1);
        break;
      case 5:
        bad.lifetime.reset();
        break;
      case 6:
        bad.reference = static_cast<c::CaptureReference>(99);
        break;
    }
    state->push(std::move(bad));
    eventually([&] { return provider.failure().has_value(); },
               "invalid acquisition did not retire its generation");
    check(queue.availability(now) == c::Availability::closed && !queue.current(now) &&
              held->image->pixels.front() == 0xff171717 && provider.statistics().published == 1,
          "fault kept stale camera visible, fabricated a new capture or destroyed held GPU pixels");
  }
  const auto cfg = config(c::CaptureDelivery::dma_buf);
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  auto sample = dma(state, 1, first);
  auto bad = std::make_shared<v::NativeFrame>(*sample.native);
  bad->metadata.sender_ntp = 99;
  sample.native = bad;
  state->push(std::move(sample));
  c::CaptureProvider provider(cfg, queue, factory(state), clock());
  eventually([&] { return provider.failure().has_value(); },
             "phone-clock DMA metadata accepted as camera capture");
}
void negotiation_and_faults() {
  for (unsigned kind = 0; kind < 8; ++kind) {
    const auto cfg = config();
    c::Queue queue(cfg);
    auto state = std::make_shared<State>(cfg);
    switch (kind) {
      case 0:
        state->negotiated.pixels.width = 65;
        break;
      case 1:
        state->negotiated.buffer_count = 5;
        break;
      case 2:
        state->negotiated.buffer_storage_bytes = c::maximum_packed_capture_storage + 1;
        break;
      case 3:
        state->negotiated.interval_denominator = 60;
        break;
      case 4:
        state->negotiated.interval_numerator = 0;
        break;
      case 5:
        state->negotiated.delivery = c::CaptureDelivery::dma_buf;
        break;
      case 6:
        state->start_error = std::string(600, 'x');
        break;
      case 7:
        state->take_error = "capture device removed";
        break;
    }
    c::CaptureProvider provider(cfg, queue, factory(state), clock());
    eventually([&] { return provider.failure().has_value(); },
               "bad capture negotiation/device fault was accepted");
    check(queue.availability(now) == c::Availability::closed && provider.failure()->size() <= 512 &&
              !provider.statistics().published,
          "device fault did not close its bounded generation");
  }
  const auto cfg = config();
  c::Queue queue(cfg);
  c::CaptureProvider provider(
      cfg, queue, [] { return std::unique_ptr<c::CaptureDevice>{}; }, clock());
  eventually([&] { return provider.failure().has_value(); }, "absent driver factory accepted");
  rejects([&] {
    c::CaptureProvider again(
        cfg, queue, [] { return std::unique_ptr<c::CaptureDevice>{}; }, clock());
  });
}
void observation_clock() {
  const auto cfg = config();
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  state->push(rgb(1, first));
  std::atomic<unsigned> calls{0};
  c::CaptureProvider provider(cfg, queue, factory(state),
                              [&] { return calls++ == 0 ? now : now - 1; });
  eventually([&] { return provider.failure().has_value(); }, "reversed observation clock accepted");
  check(!provider.statistics().published && queue.availability(now) == c::Availability::closed,
        "clock fault published retimestamped pixels");
}
void nonblocking_retirement() {
  const auto cfg = config();
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  state->block_stop = true;
  state->push(rgb(1, first));
  c::CaptureProvider provider(cfg, queue, factory(state), clock());
  eventually([&] { return provider.statistics().published == 1; },
             "retirement fixture did not publish");
  auto held = queue.stage(now);
  queue.complete(held->sequence, true);
  queue.discard();  // Presentation owner withdraws before requesting shutdown.
  const auto began = std::chrono::steady_clock::now();
  provider.request_stop();
  const auto elapsed = std::chrono::steady_clock::now() - began;
  eventually(
      [&] {
        std::lock_guard lock(state->mutex);
        return state->stop_entered;
      },
      "driver shutdown did not start");
  const bool prematurely_finished = provider.stopped();
  const bool visible = bool(queue.current(now));
  {
    std::lock_guard lock(state->mutex);
    state->block_stop = false;
  }
  state->wake.notify_all();
  eventually([&] { return provider.stopped(); }, "capture shutdown never acknowledged retirement");
  check(elapsed < std::chrono::milliseconds(100) && !prematurely_finished && !visible &&
            held->image->pixels.front() == 0xff171717,
        "slow driver stop blocked withdrawal or destroyed GPU-held pixels");
}
void terminal_fault_before_blocked_cleanup() {
  for (const bool unknown_error : {false, true}) {
    const auto cfg = config(c::CaptureDelivery::dma_buf);
    c::Queue queue(cfg);
    auto state = std::make_shared<State>(cfg);
    state->push(dma(state, 1, first));
    c::CaptureProvider provider(cfg, queue, factory(state), clock());
    eventually([&] { return provider.statistics().published == 1; },
               "terminal retirement fixture did not publish");
    auto held = queue.stage(now);
    queue.complete(held->sequence, true);
    {
      std::lock_guard lock(state->mutex);
      state->block_stop = true;
      state->unknown_take_error = unknown_error;
      state->take_error = "capture device removed";
    }
    eventually(
        [&] {
          std::lock_guard lock(state->mutex);
          return state->stop_entered;
        },
        "terminal cleanup did not start");
    const auto failure = provider.failure();
    const auto status = provider.statistics().state;
    const auto availability = queue.availability(now);
    const bool visible = bool(queue.current(now));
    const bool finished = provider.stopped();
    const bool backing_retained = held->native && held->native->lifetime && state->released == 0;
    {
      std::lock_guard lock(state->mutex);
      state->block_stop = false;
    }
    state->wake.notify_all();
    eventually([&] { return provider.stopped(); }, "terminal capture cleanup never finished");
    check(failure &&
              *failure == (unknown_error ? "camera capture failed" : "capture device removed") &&
              status == c::CaptureStatistics::State::failed &&
              availability == c::Availability::closed && !visible && !finished && backing_retained,
          "terminal capture failure waited for blocked driver cleanup before withdrawal");
    {
      std::lock_guard lock(state->mutex);
      check(state->stops == 1, "terminal capture invoked driver shutdown more than once");
    }
    held.reset();
    check(state->released == 1 && queue.statistics().outstanding_leases == 0,
          "terminal retirement leaked GPU-held backing");
  }
}
void closed_capture_with_pending_native_leases() {
  // This is one acquisition-owner interleaving with two driver outcomes. The
  // native descriptor and clock are fixtures; Boolean Queue completions model
  // receipts and do not establish a GPU fence or physical presentation.
  for (const bool fault : {false, true}) {
    const auto cfg = config(c::CaptureDelivery::dma_buf);
    c::Queue retired(cfg);
    auto state = std::make_shared<State>(cfg);
    auto provider = std::make_unique<c::CaptureProvider>(cfg, retired, factory(state), clock());
    // Release intentional fake-driver stalls even if an assertion throws,
    // before the provider destructor joins its actual worker.
    struct Unblock {
      std::shared_ptr<State> state;
      ~Unblock() {
        {
          std::lock_guard lock(state->mutex);
          state->block_take = false;
          state->block_stop = false;
        }
        state->wake.notify_all();
      }
    } unblock{state};
    std::vector<std::shared_ptr<const c::Frame>> held;
    for (unsigned i = 0; i < 3; ++i) {
      state->push(dma(state, 10 + i, first + i));
      eventually([&] { return provider->statistics().published == i + 1; },
                 "pending native retirement fixture did not publish");
      auto frame = retired.stage(now);
      check(frame && frame->native && frame->capture_monotonic_ns == first + i,
            "pending native retirement fixture lost its original capture");
      held.push_back(std::move(frame));
      if (i == 0)
        check(retired.complete(held.front()->sequence, true), "initial modeled receipt rejected");
    }
    check(retired.current(now) == held.front() && retired.statistics().staged == 3 &&
              retired.statistics().presented == 1 && retired.statistics().outstanding_leases == 3,
          "staged native captures advanced presentation or lost their leases");
    {
      std::lock_guard lock(state->mutex);
      state->block_take = true;
      state->take_entered = false;
      state->block_stop = true;
    }
    eventually(
        [&] {
          std::lock_guard lock(state->mutex);
          return state->take_entered;
        },
        "retiring capture never entered its in-flight driver call");
    if (fault) {
      std::lock_guard lock(state->mutex);
      state->take_error = "pending native capture removed";
    } else {
      // A real worker completion arrives after the presentation owner closes
      // this Queue. It must return its backing without publishing a capture.
      state->push(dma(state, 13, first + 3));
      const auto began = std::chrono::steady_clock::now();
      retired.close();
      provider->request_stop();
      check(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(100) &&
                !provider->stopped(),
            "close/stop waited for the in-flight driver or falsely acknowledged retirement");
    }
    {
      std::lock_guard lock(state->mutex);
      state->block_take = false;
    }
    state->wake.notify_all();
    eventually(
        [&] {
          std::lock_guard lock(state->mutex);
          return state->stop_entered;
        },
        "pending native driver cleanup did not start");
    const auto retiring = provider->statistics();
    check(!provider->stopped() && bool(provider->failure()) == fault &&
              retiring.state == (fault ? c::CaptureStatistics::State::failed
                                       : c::CaptureStatistics::State::running) &&
              retiring.last_published && retiring.last_dequeued && retiring.published == 3 &&
              retiring.dequeued == (fault ? 3 : 4) &&
              retiring.last_published->driver_sequence == 12 &&
              retiring.last_published->capture_monotonic_ns == first + 2 &&
              retiring.last_published->observed_monotonic_ns == now &&
              retired.availability(now) == c::Availability::closed && !retired.can_offer() &&
              !retired.current(now) && !retired.stage(now) && !retired.available_capture_ns(now) &&
              state->released == (fault ? 0 : 1),
          "blocked cleanup or late driver completion renewed the closed capture generation");
    for (const auto& frame : held)
      check(!retired.complete(frame->sequence, true) && frame->native->lifetime &&
                std::static_pointer_cast<const Returned>(frame->native->lifetime)->state == state &&
                frame->capture_reference == c::CaptureReference::start_of_exposure &&
                !frame->native->metadata.sender_ntp &&
                frame->native->metadata.timestamp_ns == frame->capture_monotonic_ns,
            "late receipt revived a retired capture or damaged its original native lease");
    check(retired.statistics().cancelled == 2 && retired.statistics().presented == 1 &&
              retired.statistics().outstanding_leases == 3,
          "retirement changed pending bounds or counted staged/retained frames as presented");
    {
      std::lock_guard lock(state->mutex);
      state->block_stop = false;
    }
    state->wake.notify_all();
    eventually([&] { return provider->stopped(); },
               "pending native provider did not retire while external backing remained held");
    provider.reset();
    unsigned retired_calls;
    {
      std::lock_guard lock(state->mutex);
      retired_calls = state->driver_calls;
      check(state->stops == 1 && !state->wrong_thread,
            "pending native driver stop was duplicated or escaped its worker");
    }
    // The new Queue and driver have independent identity and acquisition
    // ordering. Old permits remain held; they cannot consume the new pool.
    c::Queue successor(cfg);
    auto next = std::make_shared<State>(cfg);
    c::CaptureProvider healthy(cfg, successor, factory(next), clock());
    std::vector<std::shared_ptr<const c::Frame>> fresh;
    constexpr auto successor_capture = first + 40'000'000;
    for (unsigned i = 0; i < c::Queue::maximum_leases; ++i) {
      next->push(dma(next, 1 + i, successor_capture + i));
      eventually([&] { return healthy.statistics().published == i + 1; },
                 "independent successor borrowed old capture leases or driver sequence");
      auto frame = successor.stage(now);
      check(frame && frame->acquisition && frame->sequence == i + 1 &&
                frame->native != held.front()->native &&
                std::static_pointer_cast<const Returned>(frame->native->lifetime)->state == next &&
                frame->acquisition->driver_sequence == i + 1 &&
                frame->capture_monotonic_ns == successor_capture + i &&
                frame->acquisition->published_monotonic_ns == now &&
                frame->capture_reference == c::CaptureReference::start_of_exposure &&
                successor.current(now) == (fresh.empty() ? nullptr : fresh.back()),
            "successor lost independent identity/original dates or presented without a receipt");
      check(!retired.complete(held[1]->sequence, true) && successor.complete(frame->sequence, true),
            "old pending receipt restored retirement or prevented the independent receipt");
      fresh.push_back(std::move(frame));
    }
    check(successor.statistics().outstanding_leases == c::Queue::maximum_leases &&
              successor.statistics().maximum_outstanding_leases == c::Queue::maximum_leases &&
              successor.current(successor_capture + 3 + 150'000'000) == fresh.back() &&
              !successor.current(successor_capture + 3 + 150'000'001) &&
              retired.availability(now) == c::Availability::closed,
          "successor changed its fixed lease bound or renewed freshness after retirement");
    // Final old releases happen on another thread after driver destruction,
    // while the successor still owns all four of its independent leases.
    std::thread release([frames = std::move(held)]() mutable { frames.clear(); });
    release.join();
    {
      std::lock_guard lock(state->mutex);
      check(state->driver_calls == retired_calls && state->released == (fault ? 3 : 4) &&
                retired.statistics().outstanding_leases == 0 && next->released == 0,
            "old native release invoked a retired driver, leaked backing or returned a successor");
    }
    successor.close();
    healthy.request_stop();
    eventually([&] { return healthy.stopped(); }, "independent healthy capture did not retire");
    check(next->released == 0, "successor retirement destroyed externally held native backing");
    fresh.clear();
    check(next->released == c::Queue::maximum_leases &&
              successor.statistics().outstanding_leases == 0,
          "successor final release leaked independent native backing");
  }
}

void adapted_capture() {
  auto cfg = config();
  cfg.width = 128;
  cfg.v4l2->adaptation = c::CaptureAdaptation{64, 64, c::CaptureFit::contain};
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  auto sample = rgb(1, first);
  sample.reference = c::CaptureReference::start_of_exposure;
  state->push(std::move(sample));
  c::CaptureProvider provider(cfg, queue, factory(state), clock());
  eventually([&] { return provider.statistics().published == 1; },
             "adapted capture did not publish");
  auto frame = queue.stage(now);
  check(frame && frame->image->width == 128 && frame->image->height == 64 &&
            frame->capture_monotonic_ns == first && !frame->simulated &&
            frame->capture_reference == c::CaptureReference::start_of_exposure &&
            frame->image->pixels[31] == 0xff000000U && frame->image->pixels[32] == 0xff171717U &&
            frame->image->pixels[95] == 0xff171717U && frame->image->pixels[96] == 0xff000000U,
        "adaptation changed acquisition provenance or aspect geometry");
  queue.complete(frame->sequence, true);
  check(!queue.current(first + 150000001), "converted pixels renewed acquisition freshness");
  provider.request_stop();
  eventually([&] { return provider.stopped(); }, "adapted worker did not retire");
}
void consumed_adapted_capture() {
  // Production capture conversion -> immutable Queue -> actual LocalInput /
  // Stream consumption. Only the driver and dates/pixels are synthetic. The
  // external LocalInput factory leaves acquisition to this sole injected owner.
  auto cfg = config();
  cfg.width = cfg.canvas_width = 128;
  cfg.destination.width = 128;
  cfg.v4l2->adaptation = c::CaptureAdaptation{64, 64, c::CaptureFit::contain};
  auto queue = std::make_shared<c::Queue>(cfg);
  auto state = std::make_shared<State>(cfg);
  state->negotiated.pixels.stride = 200;
  state->negotiated.buffer_storage_bytes = 200 * 64;
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(200 * 64, 250);
  for (unsigned row = 0; row < 64; ++row)
    std::fill_n(bytes->begin() + row * 200, 64 * 3, 23);
  auto sample = rgb(17, first);
  sample.packed = *bytes;
  sample.lifetime = bytes;
  state->push(std::move(sample));
  c::CaptureProvider provider(cfg, *queue, factory(state), clock());
  eventually([&] { return provider.statistics().published == 1; },
             "actual padded adaptation did not publish");
  auto local = cfg;
  local.source = c::SourceKind::external;
  local.v4l2.reset();
  auto memory = std::make_shared<v::Memory>();
  auto stream = std::make_shared<v::Stream>(
      v::Input{cfg.stream_identifier, 128, 64, 1. / 30, v::InputType::camera}, memory);
  std::uint64_t at = now + 2'000'000;
  v::MediaClock media;
  media.update({true, (std::uint64_t{2208989700} << 32), first, first, first, 0});
  v::LocalInput input(local, stream, queue, [&] { return at; });
  input.set_active(true);
  // set_active discards prior offers by design: acquire a genuine NEW sample
  // after activation, never use fixture setup to resurrect queued old pixels.
  sample = rgb(18, first + 1);
  sample.packed = *bytes;
  sample.lifetime = bytes;
  state->push(std::move(sample));
  eventually([&] { return provider.statistics().published == 2; },
             "post-activation padded capture did not publish");
  input.pump(&media);
  check(!input.consumption(), "publication alone invented stream consumption");
  const auto token = input.stage(media).value();
  const auto consumed = input.consumption().value();
  check(consumed.acquisition && consumed.acquisition->width == 64 &&
            consumed.acquisition->height == 64 && consumed.acquisition->stride_bytes == 200 &&
            consumed.acquisition->driver_sequence == 18 &&
            consumed.acquisition->published_monotonic_ns == now &&
            consumed.acquisition->adaptation_fit == c::CaptureFit::contain &&
            consumed.acquisition->mapping && consumed.acquisition->mapping->scale == 1 &&
            consumed.acquisition->mapping->image.origin.x == 32 &&
            consumed.acquisition->mapping->source.size.width == 64 &&
            consumed.acquisition->mapping->destination.origin.x == 32 &&
            consumed.acquisition->mapping->destination.size.width == 64 && consumed.width == 128 &&
            consumed.height == 64 && consumed.stride_bytes[0] == 512 &&
            consumed.capture_monotonic_ns == first + 1 && consumed.prepared_monotonic_ns == at &&
            !consumed.receipt_observed_monotonic_ns,
        "consumed acquisition/delivery/date identity joined a different publication");
  const auto pixels = stream->image();
  std::vector<std::uint32_t> expected(128 * 64, 0xff000000);
  for (unsigned row = 0; row < 64; ++row)
    std::fill_n(expected.begin() + row * 128 + 32, 64, 0xff171717);
  check(pixels->pixels == expected,
        "metadata handoff changed baseline contain pixels or read source row padding");
  std::ostringstream report;
  consumed.write_json(report);
  check(report.str().size() < 2048 &&
            report.str().find("\"target\":\"capture-delivery\"") != std::string::npos &&
            report.str().find("\"deliveryRect\":{\"x\":32,\"y\":0,\"width\":64,\"height\":64}") !=
                std::string::npos &&
            report.str().find("\"signedSceneFit\":null") != std::string::npos,
        "actual consumed mapping was lost or relabelled signed fit in finite serializer");
  input.complete(token, true);
  const auto original = input.consumption().value();
  auto bad = rgb(19, first + 10);
  bad.corrupted = true;
  state->push(std::move(bad));
  eventually([&] { return provider.statistics().corrupted == 1; },
             "corrupt newer source fixture did not run");
  check(input.consumption()->acquisition->driver_sequence == 18 &&
            input.consumption()->capture_monotonic_ns == original.capture_monotonic_ns,
        "corrupted metadata renewed consumed acquisition");
  at = first + 150'000'002;
  check(!input.consumption(), "adapted source used publication date for freshness");
  provider.request_stop();
  eventually([&] { return provider.stopped(); }, "consumed adaptation source did not retire");
  input.set_active(false);
  input.complete(token, true);
  check(!input.consumption() && pixels->pixels[32] == 0xff171717,
        "retired metadata revived or changed externally retained source pixels");
}
void timing_diagnostics() {
  auto cfg = config();
  cfg.width = 128;
  cfg.v4l2->adaptation = c::CaptureAdaptation{64, 64, c::CaptureFit::contain};
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  state->push(rgb(0xffffffffU, first));
  std::atomic<unsigned> observations{0};
  c::CaptureProvider provider(cfg, queue, factory(state),
                              [&] { return now + observations++ * 20000000ULL; });
  eventually([&] { return provider.statistics().published == 1; },
             "timing fixture did not publish");
  auto stats = provider.statistics();
  check(stats.last_dequeued && stats.last_published &&
            stats.last_dequeued->capture_monotonic_ns == first &&
            stats.last_dequeued->observed_monotonic_ns == now &&
            stats.last_published->capture_monotonic_ns == first &&
            stats.last_published->observed_monotonic_ns == now + 20000000 &&
            stats.last_published->reference == c::CaptureReference::end_of_frame &&
            stats.maximum_dequeue_age_ns == 100000000 &&
            stats.maximum_publication_age_ns == 120000000 && stats.sequence_gaps == 0,
        "publication diagnostics lost original acquisition or conversion/adaptation age");
  state->push(rgb(0, first + 1));
  eventually([&] { return provider.statistics().stale == 1; },
             "conversion-expired capture did not retire");
  stats = provider.statistics();
  check(stats.last_dequeued->driver_sequence == 0 &&
            stats.last_dequeued->observed_monotonic_ns == now + 40000000 &&
            stats.last_published->driver_sequence == 0xffffffffU && stats.published == 1 &&
            stats.maximum_publication_age_ns == 120000000 && stats.sequence_gaps == 0,
        "expired conversion renewed publication timing or normal wrap invented a gap");
  c::CaptureSample corrupted;
  corrupted.corrupted = true;
  corrupted.driver_sequence = 999;
  corrupted.reference = static_cast<c::CaptureReference>(99);
  state->push(std::move(corrupted));
  eventually([&] { return provider.statistics().corrupted == 1; },
             "diagnostic corruption fixture was not consumed");
  stats = provider.statistics();
  check(stats.last_dequeued->driver_sequence == 0 &&
            stats.last_published->driver_sequence == 0xffffffffU && stats.sequence_gaps == 0,
        "corrupt metadata qualified a diagnostic date or driver sequence");
  state->push(rgb(2, now + 60000000));
  eventually([&] { return provider.statistics().published == 2; },
             "fresh capture after diagnostic expiry did not recover");
  stats = provider.statistics();
  check(stats.last_published->driver_sequence == 2 &&
            stats.last_published->capture_monotonic_ns == now + 60000000 &&
            stats.last_published->observed_monotonic_ns == now + 100000000 &&
            stats.maximum_dequeue_age_ns == 139999999 &&
            stats.maximum_publication_age_ns == 120000000 && stats.sequence_gaps == 1,
        "valid driver sequence gaps or historical age maxima were misreported");
  const auto prior = stats.last_published;
  {
    std::lock_guard lock(state->mutex);
    state->take_error = "camera removed after timing sample";
  }
  eventually([&] { return provider.stopped(); }, "timing fixture failed to retire");
  stats = provider.statistics();
  check(stats.state == c::CaptureStatistics::State::failed &&
            stats.last_published->capture_monotonic_ns == prior->capture_monotonic_ns &&
            stats.last_published->observed_monotonic_ns == prior->observed_monotonic_ns &&
            !queue.current(now + 100000000),
        "retirement renewed acquisition diagnostics or retained visible camera pixels");
}
void adapted_padded_owner() {
  for (auto fit : {c::CaptureFit::contain, c::CaptureFit::cover}) {
    auto cfg = config();
    cfg.width = 128;
    cfg.v4l2->adaptation = c::CaptureAdaptation{64, 64, fit};
    c::Queue queue(cfg);
    auto state = std::make_shared<State>(cfg);
    state->negotiated.pixels.stride = 197;
    state->negotiated.buffer_storage_bytes = 197 * 64;
    auto storage = std::make_shared<std::vector<std::uint8_t>>(197 * 63 + 192, 0xe7);
    for (unsigned y = 0; y < 64; ++y)
      for (unsigned x = 0; x < 64; ++x) {
        auto* p = storage->data() + y * 197 + x * 3;
        p[0] = y < 8 ? 255 : 0;
        p[1] = y >= 8 && y < 56 ? 255 : 0;
        p[2] = y >= 56 ? 255 : 0;
      }
    auto sample = rgb(91, first);
    sample.packed = *storage;
    sample.lifetime = storage;
    sample.reference = c::CaptureReference::start_of_exposure;
    state->push(std::move(sample));
    c::CaptureProvider provider(cfg, queue, factory(state), clock());
    eventually([&] { return provider.statistics().published == 1; },
               "padded adapted capture failed to publish");
    auto held = queue.stage(now);
    check(held && held->image->width == 128 && held->image->height == 64 &&
              held->capture_monotonic_ns == first && !held->simulated &&
              held->capture_reference == c::CaptureReference::start_of_exposure,
          "adapted padded owner lost original dimensions/date/reference");
    check(held->acquisition && held->acquisition->mapping &&
              held->acquisition->driver_sequence == 91 && held->acquisition->stride_bytes == 197,
          "successful actual adaptation lost its own consumed mapping");
    const auto& mapping = *held->acquisition->mapping;
    if (fit == c::CaptureFit::cover)
      check(mapping.scale == 2 && mapping.image.origin.y == -32 && mapping.source.origin.y == 16 &&
                mapping.source.size.height == 32 && mapping.destination.origin.x == 0 &&
                mapping.destination.size.width == 128 && mapping.destination.size.height == 64,
            "actual cover pixels do not carry their independent acquisition crop");
    else
      check(mapping.scale == 1 && mapping.image.origin.x == 32 && mapping.source.origin.y == 0 &&
                mapping.source.size.height == 64 && mapping.destination.origin.x == 32 &&
                mapping.destination.size.width == 64 && mapping.destination.size.height == 64,
            "actual contain pixels do not carry their independent visible content rectangle");
    if (fit == c::CaptureFit::cover)
      check(std::all_of(held->image->pixels.begin(), held->image->pixels.end(),
                        [](auto p) { return p == 0xff00ff00; }),
            "capture worker cover failed to crop original padded pixels");
    else
      check(held->image->pixels[31] == 0xff000000 && held->image->pixels[32] == 0xffff0000 &&
                held->image->pixels[63 * 128 + 95] == 0xff0000ff &&
                held->image->pixels[63 * 128 + 96] == 0xff000000,
            "capture worker contain changed original field or padding");
    eventually([&] { return storage.use_count() == 1; },
               "capture worker did not return its original padded sample lease");
    std::fill(storage->begin(), storage->end(), 0);
    check(queue.complete(held->sequence, true) && queue.current(now) == held &&
              !queue.current(first + 150000001),
          "adaptation/source reuse renewed acquisition freshness");
    const auto original = provider.statistics().last_published;
    check(original && original->driver_sequence == 91 && original->capture_monotonic_ns == first &&
              original->observed_monotonic_ns == now,
          "adapted publication did not preserve original event dates");
    // A malformed successor retires the actual owner/Queue, but an independent
    // GPU lease remains immutable and late receipts cannot restore eligibility.
    auto malformed = rgb(92, first + 1);
    malformed.reference = c::CaptureReference::start_of_exposure;
    state->push(std::move(malformed));  // Too short for negotiated padded rows.
    eventually([&] { return provider.stopped(); }, "adapted failed owner did not retire");
    const auto retired = provider.statistics();
    check(retired.state == c::CaptureStatistics::State::failed && retired.published == 1 &&
              retired.last_published->capture_monotonic_ns == first &&
              retired.last_published->observed_monotonic_ns == now &&
              queue.availability(now) == c::Availability::closed && !queue.current(now) &&
              !queue.complete(held->sequence, true) &&
              held->image->pixels[32 * 128 + 64] == 0xff00ff00,
          "retired adaptation revived metadata/pixels or lost held backing");
    held.reset();
    check(queue.statistics().outstanding_leases == 0,
          "retired adapted owner leaked its final capture lease");
  }
  auto cfg = config();
  cfg.width = 128;
  cfg.v4l2->adaptation = c::CaptureAdaptation{64, 64, c::CaptureFit::cover};
  c::Queue queue(cfg);
  auto state = std::make_shared<State>(cfg);
  state->negotiated.pixels.width = 66;
  state->negotiated.pixels.stride = 198;
  state->negotiated.buffer_storage_bytes = 198 * 64;
  c::CaptureProvider provider(cfg, queue, factory(state), clock());
  eventually([&] { return provider.stopped(); }, "changed acquisition geometry did not retire");
  check(provider.failure() && !provider.statistics().published &&
            !provider.statistics().last_published &&
            queue.availability(now) == c::Availability::closed,
        "unexpected negotiated geometry became a fit or acquisition claim");
}
}  // namespace
int main() try {
  isolation_and_missing();
  latest_and_copy();
  dma_lifetime_and_stop();
  corruption_and_stale();
  bounded_backpressure();
  invalid_samples();
  negotiation_and_faults();
  observation_clock();
  nonblocking_retirement();
  terminal_fault_before_blocked_cleanup();
  closed_capture_with_pending_native_leases();
  adapted_capture();
  consumed_adapted_capture();
  timing_diagnostics();
  adapted_padded_owner();
  std::cout << "Capture-worker groups passed: isolation, genuine acquisition, content, bounded "
               "leases, expiry, recovery, combined native retirement and faults (injected driver; "
               "no camera/GPU)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
