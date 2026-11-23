// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Production GpuDisplay/Runtime/clock/input scheduling, with an explicit fake
// output that releases test receipts only on request. This is not GPU or
// compositor qualification, and is never linked into production executables.
#include <headunit/theme/gpu_display.hpp>
#include <headunit/theme/camera_state.hpp>
#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace g = headunit::theme::graphics;
namespace s = headunit::theme::script;
namespace v = headunit::theme::video;
namespace vehicle = headunit::theme::vehicle;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
std::uint64_t ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}
struct Submission {
  std::uint64_t token;
  g::WindowContent content;
  bool resolved = false, presented = false;
};
struct Output {
  std::deque<Submission> pending;
  std::vector<Submission> history;
  unsigned capacity = 1;
  bool composed = false, touch_enabled = false;
  std::function<void()> during_poll;
  std::optional<g::PresentationReceipt> last_receipt;
  std::optional<std::uint64_t> replay_token;
  void release(bool presented = true) {
    check(!pending.empty() && !pending.front().resolved,
          "fixture release without unresolved submission");
    pending.front().resolved = true;
    pending.front().presented = presented;
  }
};
std::vector<Output*> outputs;
}  // namespace
namespace headunit::theme::graphics {
struct GpuCompositor::Impl {
  Output output;
};
GpuCompositor::GpuCompositor(std::uint32_t, std::uint32_t, WaylandOptions options)
    : impl_(std::make_unique<Impl>()) {
  impl_->output.capacity = options.max_pending_presentations;
  outputs.push_back(&impl_->output);
}
GpuCompositor::~GpuCompositor() {
  std::erase(outputs, &impl_->output);
}
GpuTimings GpuCompositor::compose_conical(std::span<const GpuConicalGradient>, Color) {
  impl_->output.composed = true;
  return {};
}
GpuTimings GpuCompositor::compose_scene(const std::shared_ptr<const Layer>& root, std::uint64_t,
                                        const std::function<void()>& during) {
  check(bool(root), "fixture scene composition without root");
  if (during) during();
  impl_->output.composed = true;
  return {};
}
GpuSceneCacheStats GpuCompositor::scene_cache_stats() const {
  return {};
}
void GpuCompositor::submit(std::uint64_t token, Image*, WindowContent content) {
  auto& output = impl_->output;
  check(output.composed && presentation_slot_available(),
        "fixture uncomposed or overfull submission");
  const Submission item{token, content};
  output.history.push_back(item);
  output.pending.push_back(item);
  output.composed = false;
}
bool GpuCompositor::presentation_pending() const {
  return !impl_->output.pending.empty();
}
bool GpuCompositor::presentation_slot_available() const {
  return impl_->output.pending.size() < impl_->output.capacity;
}
void GpuCompositor::set_presentation_queue_depth(unsigned depth) {
  impl_->output.capacity = depth;
}
bool GpuCompositor::preview_feedback_delayed() const {
  return false;
}
std::vector<headunit::TouchInput> GpuCompositor::take_touches(bool enabled) {
  impl_->output.touch_enabled = enabled;
  return {};
}
std::optional<PresentationReceipt> GpuCompositor::poll_presentation() {
  auto& output = impl_->output;
  if (output.during_poll) output.during_poll();
  if (output.pending.empty() || !output.pending.front().resolved) return {};
  const auto item = output.pending.front();
  output.pending.pop_front();
  const auto at = ns();
  PresentationReceipt receipt;
  receipt.token = item.token;
  receipt.presented = item.presented;
  // Explicit synthetic cross-clock offset, and a distinct poll observation.
  // Tests must preserve these facts instead of replacing them with callback time.
  const auto raw = at + 1000000000;
  receipt.clock_id = 4;
  receipt.seconds = raw / 1000000000;
  receipt.nanoseconds = raw % 1000000000;
  receipt.received_ns = at + 1234;
  receipt.sequence = item.token + 500;
  receipt.flags = 3;
  if (output.replay_token) receipt.token = *output.replay_token;
  receipt.refresh_ns = 16666667;
  if (item.presented) receipt.correlation = v::ClockCorrelation{4, raw, at, at};
  output.last_receipt = receipt;
  return receipt;
}
}  // namespace headunit::theme::graphics
namespace {
class Input final : public v::DisplayVideoInput {
 public:
  explicit Input(std::shared_ptr<v::Stream> stream) : stream_(std::move(stream)) {}
  const std::shared_ptr<v::Stream>& stream() const override { return stream_; }
  bool next = false;
  unsigned eligible_pumps = 0, draining_pumps = 0;
  std::vector<std::pair<std::uint64_t, bool>> completions;
  std::vector<std::optional<v::InputPresentationReceipt>> receipts;
  void pump(const v::MediaClock* clock) override {
    if (clock)
      ++eligible_pumps;
    else
      ++draining_pumps;
  }
  std::optional<std::uint64_t> stage(const v::MediaClock& clock) override {
    if (!next) return {};
    next = false;
    const auto at = ns();
    check(stream_->offer(clock.capture_at(at, at).pts_seconds,
                         {8, 8, std::vector<std::uint32_t>(64, 0xff0088ff)}),
          "fixture source offer failed");
    auto frame = stream_->prepare_next();
    check(bool(frame), "fixture source stage failed");
    return frame->token;
  }
  void complete(std::uint64_t token, bool presented) override {
    check(stream_->prepared(token), "source preparation completed twice");
    completions.emplace_back(token, presented);
    if (presented)
      stream_->presented(token);
    else
      stream_->cancel(token);
  }
  void complete(std::uint64_t token, bool presented,
                const v::InputPresentationReceipt* receipt) override {
    receipts.push_back(receipt ? std::optional(*receipt) : std::nullopt);
    complete(token, presented);
  }
  void suspend() override { stream_->discard_all(); }

 private:
  std::shared_ptr<v::Stream> stream_;
};
struct Fixture {
  s::Display spec{"fixture", 96, 64};
  s::Runtime runtime{
      [](std::string_view) -> std::string { throw std::runtime_error("no resources"); }, spec};
  std::shared_ptr<Input> input =
      std::make_shared<Input>(runtime.add_video_input({"fixture-input", 8, 8}));
  std::shared_ptr<vehicle::CameraStateSnapshot> camera;
  std::uint64_t camera_generation = 0;
  std::unique_ptr<g::GpuDisplay> display;
  Output* output = nullptr;
  std::uint64_t origin = ns(), phase = 0;
  std::uint64_t claimed_scene = 0;
  bool locked = true;
  unsigned cancellations = 0;
  Fixture(bool reveal, unsigned capacity = 1, bool overlap = false, std::uint64_t stall_timeout = 0,
          bool withdrawal = false) {
    g::GpuDisplayOptions options;
    options.reveal_underlay_while_waiting = reveal;
    options.window.opaque_background = true;
    options.overlap_script_tick = overlap;
    options.scene_stall_timeout_ns = stall_timeout;
    options.window.max_pending_presentations = capacity;
    options.frame_interval_ns = 20000000;
    options.inputs = {input};
    options.touches = [](auto) {
    };
    options.cancel_touches = [this] {
      ++cancellations;
    };
    if (withdrawal) {
      camera = std::make_shared<vehicle::CameraStateSnapshot>();
      camera_generation = camera->begin(ns());
      options.input_withdrawal = g::GpuDisplayOptions::InputWithdrawal{
          0,
          [this](std::uint64_t at) { return camera->off_for_composition(camera_generation, at); },
          [this](const g::InputWithdrawalReceipt& r) {
            camera->presented_off(camera_generation, r.sequence, r.submission_epoch,
                                  r.current_epoch, r.draw, r.submitted_ns, r.presented_ns,
                                  r.observed_ns);
          }};
    }
    display = std::make_unique<g::GpuDisplay>(
        runtime, spec,
        [this]() -> std::optional<headunit::AirplayClockSample> {
          if (!locked) return {};
          const auto at = ns(), elapsed = at - origin;
          const auto media = ((std::uint64_t{2208989700} + elapsed / 1000000000) << 32) +
                             ((elapsed % 1000000000) << 32) / 1000000000;
          return headunit::AirplayClockSample{media + phase, at, at, 1000};
        },
        options);
    output = outputs.back();
  }
  s::UiWorker::DisplayTurn step(
      s::UiWorker::DisplayPhase phase = s::UiWorker::DisplayPhase::frame) {
    return display->turn(phase);
  }
  void root() {
    runtime.evaluate(R"(
    local root=CP.DPLayer.create()
    root.frame=CP.DPRect(0,0,96,64)
    local video=CP.DPVideoLayer.create('fixture-input')
    video.frame=CP.DPRect(0,0,96,64)
    video:subscribe();root:addSublayer(video)
    CP.setRootLayer('fixture',root)
  )");
  }
  void remove_root() { runtime.evaluate("CP.setRootLayer('fixture',nil)"); }
  void bootstrap() {
    const auto first = step(s::UiWorker::DisplayPhase::bootstrap);
    check(!first.pts && !first.allow_tasks, "unpresented bootstrap authorized tasks");
    output->release();
    const auto accepted = step(s::UiWorker::DisplayPhase::bootstrap);
    check(accepted.pts && accepted.allow_tasks && !display->stats().scene_visible,
          "blank bootstrap qualified a scene");
    runtime.tick(accepted.pts);
    runtime.evaluate("-- original scheduling fixture");
    runtime.start_tasks(accepted.pts);
  }
  void queue_scene() {
    const auto deadline = Clock::now() + 500ms;
    for (;;) {
      // A feedback turn can itself submit the next scene. Claim that actual
      // submission instead of requiring an extra draw behind a full queue.
      for (const auto& item : output->history)
        if (item.token > claimed_scene && item.content == g::WindowContent::rendered_scene) {
          claimed_scene = item.token;
          return;
        }
      step();
      check(Clock::now() < deadline, "scene scheduling fixture deadline");
      std::this_thread::sleep_for(1ms);
    }
  }
};
void bootstrap_and_default() {
  Fixture waiting(true);
  waiting.step(s::UiWorker::DisplayPhase::bootstrap);
  check(waiting.output->history.back().content == g::WindowContent::reveal_underlay &&
            waiting.display->stats().underlay_submissions == 1,
        "bootstrap did not reveal underlay");
  waiting.output->release(false);
  auto turn = waiting.step(s::UiWorker::DisplayPhase::bootstrap);
  check(!turn.pts && !turn.allow_tasks && !waiting.display->stats().scene_visible,
        "discarded bootstrap manufactured PTS");
  check(waiting.output->history.size() == 2, "discarded bootstrap was not retried honestly");
  waiting.output->release();
  turn = waiting.step(s::UiWorker::DisplayPhase::bootstrap);
  check(turn.pts && turn.allow_tasks && !waiting.display->stats().scene_visible,
        "presented waiting buffer qualified instruments");
  for (unsigned i = 0; i < 20; ++i)
    waiting.step(s::UiWorker::DisplayPhase::bootstrap);
  check(waiting.input->eligible_pumps == 0 && waiting.input->draining_pumps >= 23,
        "bootstrap leased decoded video before scene presentation could drain it");
  waiting.step(s::UiWorker::DisplayPhase::frame);
  check(waiting.input->eligible_pumps == 1,
        "ordinary display turn did not restore qualified video decoding");
  Fixture normal(false);
  normal.bootstrap();
  check(normal.output->history.front().content == g::WindowContent::rendered_scene &&
            normal.display->stats().underlay_submissions == 0,
        "default behavior changed");
}
void missing_root(unsigned capacity) {
  Fixture f(true, capacity);
  f.bootstrap();
  f.root();
  f.input->next = true;
  f.queue_scene();
  f.output->release();
  f.input->next = true;
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.display->stats().scene_visible && f.output->touch_enabled &&
            f.input->completions.size() == 1 && f.input->completions[0].second,
        "initial scene/source was not qualified");
  f.queue_scene();
  if (capacity == 2) {
    f.input->next = true;
    f.queue_scene();
  }
  const auto old_count = f.output->pending.size();
  const auto cancelled = f.cancellations;
  f.remove_root();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(!f.display->stats().scene_visible && !f.output->touch_enabled &&
            f.cancellations == cancelled + 1,
        "root loss left scene/touch eligible");
  for (std::size_t i = 0; i < old_count; ++i) {
    f.output->release();
    const auto turn = f.step(s::UiWorker::DisplayPhase::feedback);
    check(turn.pts && !turn.allow_tasks && !f.display->stats().scene_visible,
          "older receipt revived scene or lost actual PTS");
  }
  check(f.input->completions.size() == old_count + 1,
        "root loss erased genuine source completions");
  for (const auto& completion : f.input->completions)
    check(completion.second, "actually presented source was falsely rejected");
  check(f.output->pending.size() == 1 &&
            f.output->history.back().content == g::WindowContent::reveal_underlay,
        "missing root did not queue one clear after pending receipts");
  const auto first_clear = f.display->stats().underlay_submissions;
  f.output->release(false);
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(!f.display->stats().scene_visible &&
            f.display->stats().underlay_submissions == first_clear + 1 &&
            f.output->pending.size() == 1 &&
            f.output->history.back().content == g::WindowContent::reveal_underlay,
        "discarded missing-root clear was falsely treated as visible or not retried");
  f.output->release();
  auto turn = f.step();
  check(turn.pts && turn.allow_tasks && !f.display->stats().scene_visible &&
            f.display->stats().waiting,
        "root recovery tasks lost genuine timing");
  const auto clears = f.display->stats().underlay_submissions;
  for (unsigned i = 0; i < 5; ++i)
    f.step();
  check(f.display->stats().underlay_submissions == clears,
        "waiting repeatedly submitted empty frames");
  f.root();
  f.queue_scene();
  check(!f.display->stats().scene_visible, "submission qualified visibility without a receipt");
  f.output->release(false);
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(!f.display->stats().scene_visible, "discarded recovered scene qualified visibility");
  f.queue_scene();
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.display->stats().scene_visible && !f.display->stats().waiting && f.output->touch_enabled,
        "fresh recovered scene did not qualify");
}
void recovered_root_before_old_receipt() {
  Fixture f(true, 2);
  f.bootstrap();
  f.root();
  f.queue_scene();
  f.remove_root();
  f.step(s::UiWorker::DisplayPhase::feedback);
  f.root();
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(!f.display->stats().scene_visible, "root returning requalified the pre-loss queued scene");
  // A fresh submission may already have followed that receipt on the feedback
  // turn; whichever turn submits it, only its own presented receipt qualifies.
  if (f.output->pending.empty()) f.queue_scene();
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.display->stats().scene_visible, "fresh post-loss scene failed to qualify");
}
void clock_loss(unsigned capacity) {
  Fixture f(true, capacity);
  f.bootstrap();
  f.root();
  f.input->next = true;
  f.queue_scene();
  if (capacity == 2) f.queue_scene();
  const auto count = f.output->pending.size();
  f.locked = false;
  auto turn = f.step();
  check(!turn.pts && !turn.allow_tasks && !f.display->stats().scene_visible,
        "clock loss retained authorization");
  for (std::size_t i = 0; i < count; ++i) {
    f.output->release();
    turn = f.step();
    check(!turn.pts && !turn.allow_tasks, "old epoch receipt revived timing");
  }
  check(f.input->receipts.empty(), "retired epoch exposed mapped input receipt");
  check(f.output->pending.size() == 1 &&
            f.output->history.back().content == g::WindowContent::reveal_underlay,
        "clock outage did not clear output");
  f.output->release();
  f.step();
  const auto clears = f.display->stats().underlay_submissions;
  for (unsigned i = 0; i < 5; ++i)
    f.step();
  check(f.display->stats().underlay_submissions == clears,
        "clock outage repeatedly submitted clears");
  f.locked = true;
  turn = f.step();
  check(!turn.pts && !turn.allow_tasks, "relock invented presentation");
  f.output->release();
  turn = f.step(s::UiWorker::DisplayPhase::bootstrap);
  check(turn.pts && turn.allow_tasks && !f.display->stats().scene_visible,
        "relock bootstrap qualified a scene");
  f.queue_scene();
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.display->stats().scene_visible, "fresh same-source relock scene not qualified");
}
void inline_root_loss() {
  Fixture f(true, 1, true);
  f.bootstrap();
  f.root();
  f.input->next = true;
  f.runtime.evaluate(R"(
    CP.DPLuaTask.create():run(function() CP.setRootLayer('fixture',nil) end)
  )");
  const auto turn = f.step();
  check(turn.pts && !turn.allow_tasks && !f.runtime.scene_root(),
        "inline task did not remove the root exactly once");
  check(f.output->history.back().content == g::WindowContent::reveal_underlay &&
            f.display->stats().scene_submissions == 0 && !f.display->stats().scene_visible,
        "inline root loss committed an opaque old scene");
  check(f.input->completions.empty(), "omitted source lease retired before a real receipt");
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.input->completions.size() == 1 && !f.input->completions[0].second &&
            !f.display->stats().scene_visible,
        "transparent commit falsely presented its omitted source");
}
void discarded_clears_do_not_extend_stall() {
  Fixture f(true, 1, false, 1000000000);
  f.bootstrap();
  f.root();
  f.queue_scene();
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.display->stats().scene_visible, "stall fixture has no qualified starting scene");
  f.remove_root();
  const auto deadline = Clock::now() + 1500ms;
  bool stalled = false;
  while (!stalled) {
    if (!f.output->pending.empty()) f.output->release(false);
    try {
      f.step();
    } catch (const std::exception& error) {
      check(std::string_view(error.what()) == "qualified scene presentation stalled",
            "waiting escaped through an unrelated fault");
      stalled = true;
    }
    check(Clock::now() < deadline, "repeated clears indefinitely extended scene stall");
    if (!stalled) std::this_thread::sleep_for(10ms);
  }
  const auto stats = f.display->stats();
  check(stats.underlay_submissions > 5 && stats.faulted && stats.surface_withdrawn &&
            !stats.scene_visible,
        "stall did not retire a window after repeated real clear attempts");
}
void input_receipt_facts() {
  Fixture f(true, 2);
  f.bootstrap();
  f.root();
  f.input->next = true;
  f.queue_scene();
  const auto first = f.claimed_scene;
  f.queue_scene();  // Same staged source drawn again, no second source lease.
  const auto successor = f.claimed_scene;
  check(successor > first, "missing second real submission");
  f.output->release(false);
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.input->receipts.empty(), "discarded shared-source receipt completed early");
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.input->receipts.size() == 1 && f.input->receipts[0] && f.input->completions[0].second,
        "accepted successor lost copied receipt");
  const auto& r = *f.input->receipts[0];
  const auto& actual = *f.output->last_receipt;
  check(r.feedback.token == successor && r.mapped.token == successor &&
            r.feedback.sequence == actual.sequence && r.feedback.clock_id == 4 &&
            r.feedback.flags == 3 && r.feedback.seconds == actual.seconds &&
            r.feedback.nanoseconds == actual.nanoseconds &&
            r.feedback.refresh_ns == actual.refresh_ns,
        "source completion substituted raw compositor facts");
  check(r.feedback.correlation &&
            r.feedback.correlation->source_ns == actual.correlation->source_ns &&
            r.mapped.monotonic_ns == actual.correlation->before_ns &&
            r.feedback.received_ns == r.mapped.monotonic_ns + 1234 && r.submitted_ns &&
            r.submitted_ns <= r.feedback.received_ns && r.submission_epoch == r.current_epoch,
        "mapped presentation, epoch, poll or original submission date lost");
  f.queue_scene();
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.input->receipts.size() == 1, "redraw minted another source receipt");
  f.input->next = true;
  f.queue_scene();
  f.output->replay_token = successor;
  f.output->release();
  bool rejected = false;
  try {
    f.step(s::UiWorker::DisplayPhase::feedback);
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected && f.input->receipts.size() == 1 && f.display->stats().faulted,
        "replayed old receipt completed successor source");
}
void camera_withdrawal() {
  const auto wait = [](Fixture& f) {
    const auto deadline = Clock::now() + 500ms;
    while (f.output->pending.empty()) {
      f.step();
      check(Clock::now() < deadline, "withdrawal submission deadline");
      std::this_thread::sleep_for(1ms);
    }
  };
  Fixture f(true, 1, false, 0, true);
  f.bootstrap();
  f.root();
  f.input->next = true;
  wait(f);
  const auto request = *f.camera->request_off(ns(), ns() + 1000000000);
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.camera->off_result(f.camera_generation, request.sequence, ns())->status ==
            vehicle::CameraStateSnapshot::OffStatus::pending,
        "late camera receipt completed OFF");
  // The original input receipt has retired. Withdraw its retained current
  // texture exactly as the production camera owner does before stamping OFF.
  f.input->suspend();
  check(f.camera->owner_off(f.camera_generation, request.sequence, ns()),
        "fixture owner withdrawal failed");
  if (!f.output->pending.empty()) {
    f.output->release();
    f.step(s::UiWorker::DisplayPhase::feedback);
    check(f.camera->off_result(f.camera_generation, request.sequence, ns())->status ==
              vehicle::CameraStateSnapshot::OffStatus::pending,
          "pre-withdrawal retained camera composition completed OFF");
  }
  wait(f);
  f.output->release(false);
  f.step(s::UiWorker::DisplayPhase::feedback);
  check(f.camera->off_result(f.camera_generation, request.sequence, ns())->status ==
            vehicle::CameraStateSnapshot::OffStatus::pending,
        "discarded withdrawal commit completed OFF");
  wait(f);
  f.output->release();
  f.step(s::UiWorker::DisplayPhase::feedback);
  const auto result = f.camera->off_result(f.camera_generation, request.sequence, ns());
  check(result && result->status == vehicle::CameraStateSnapshot::OffStatus::applied &&
            result->presented_ns >= result->withdrawn_ns,
        "actual accepted withdrawal receipt did not complete OFF");
}
void camera_withdrawal_retirement() {
  for (bool lost_clock : {false, true}) {
    Fixture f(true, 1, false, 0, true);
    f.bootstrap();
    f.root();
    const auto request = *f.camera->request_off(ns(), ns() + 1000000000);
    check(f.camera->owner_off(f.camera_generation, request.sequence, ns()), "fixture OFF failed");
    f.queue_scene();
    if (lost_clock) {
      f.locked = false;
      f.step();
    } else {
      f.remove_root();
      f.step(s::UiWorker::DisplayPhase::feedback);
      f.root();
    }
    f.output->release();
    f.step(s::UiWorker::DisplayPhase::feedback);
    check(f.camera->off_result(f.camera_generation, request.sequence, ns())->status ==
              vehicle::CameraStateSnapshot::OffStatus::pending,
          "obsolete epoch/scene receipt completed OFF");
    f.camera->retire(f.camera_generation, ns());
    check(f.camera->off_result(f.camera_generation, request.sequence, ns())->status ==
              vehicle::CameraStateSnapshot::OffStatus::retired,
          "worker retirement lost pending request");
  }
}
}  // namespace
int main() try {
  bootstrap_and_default();
  input_receipt_facts();
  for (unsigned capacity : {1u, 2u}) {
    missing_root(capacity);
    clock_loss(capacity);
  }
  recovered_root_before_old_receipt();
  inline_root_loss();
  discarded_clears_do_not_extend_stall();
  camera_withdrawal();
  camera_withdrawal_retirement();
  std::cout
      << "Production display scheduling: explicit fake receipts, underlay bootstrap, missing-root "
         "recovery, actual source completion, clock loss and default compatibility passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
