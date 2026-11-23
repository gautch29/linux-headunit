// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_display.hpp>
#include <headunit/theme/camera_input.hpp>
#include <headunit/theme/gpu_display.hpp>
#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace c = headunit::camera;
namespace g = headunit::theme::graphics;
namespace s = headunit::theme::script;
namespace v = headunit::theme::video;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
std::uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}
template <class F>
void until(F done, const char* why) {
  const auto deadline = Clock::now() + 6s;
  while (!done()) {
    check(Clock::now() < deadline, why);
    std::this_thread::sleep_for(1ms);
  }
}
g::Image pixels(std::uint32_t colour) {
  return {64, 64, std::vector<std::uint32_t>(4096, colour)};
}
c::Configuration configuration() {
  c::Configuration result;
  result.enabled = true;
  result.manual_activation = true;
  result.source = c::SourceKind::external;
  result.stream_identifier = "camera";
  result.width = 64;
  result.height = 64;
  result.maximum_age_ms = 1000;
  result.canvas_width = 128;
  result.canvas_height = 64;
  result.destination = {64, 0, 64, 64};
  return result;
}

// The phone fixture stages one immutable CPU frame on request. Its receipt
// checks detect an abandoned preparation, duplicate completion or wrong order.
class PhoneInput final : public v::DisplayVideoInput {
 public:
  explicit PhoneInput(std::shared_ptr<v::Stream> stream) : stream_(std::move(stream)) {}
  const std::shared_ptr<v::Stream>& stream() const override { return stream_; }
  void request() {
    check(!capture_, "phone fixture request already queued");
    capture_ = now_ns();
  }
  void pump(const v::MediaClock*) override {}
  std::optional<std::uint64_t> stage(const v::MediaClock& clock) override {
    if (!capture_) {
      repeated_draws += stream_->composition_token().has_value();
      return {};
    }
    const auto pts = clock.capture_at(*capture_, now_ns()).pts_seconds;
    auto image = pixels(0xff2288dd);
    check(stream_->offer(pts, std::move(image)), "phone fixture backpressure");
    capture_.reset();
    const auto frame = stream_->prepare_next();
    check(frame.has_value(), "phone fixture preparation missing");
    outstanding.push_back(frame->token);
    issued = frame->token;
    ++staged;
    return frame->token;
  }
  void complete(std::uint64_t token, bool presented) override {
    check(!outstanding.empty() && outstanding.front() == token,
          "phone receipt duplicated or out of order");
    check(stream_->prepared(token), "phone receipt lost its preparation");
    if (presented) {
      stream_->presented(token);
      ++shown;
    } else {
      stream_->discard_prepared(token);
      ++rejected;
    }
    outstanding.pop_front();
    ++completed;
  }
  void suspend() override {
    retired += outstanding.size();
    outstanding.clear();
    capture_.reset();
    stream_->discard_all();
  }
  std::deque<std::uint64_t> outstanding;
  std::uint64_t issued = 0, staged = 0, completed = 0, shown = 0, rejected = 0, retired = 0,
                repeated_draws = 0;

 private:
  std::shared_ptr<v::Stream> stream_;
  std::optional<std::uint64_t> capture_;
};

// Keep the real CameraInput clock, queue and lifetime behavior, while recording
// the display adapter's exact per-submission lease/completion calls.
class CameraTrace final : public v::DisplayVideoInput {
 public:
  explicit CameraTrace(std::shared_ptr<v::CameraInput> input) : input_(std::move(input)) {}
  const std::shared_ptr<v::Stream>& stream() const override { return input_->stream(); }
  void pump(const v::MediaClock* clock) override { input_->pump(clock); }
  std::optional<std::uint64_t> stage(const v::MediaClock& clock) override {
    const auto token = input_->stage(clock);
    if (token) {
      outstanding.push_back(*token);
      issued = *token;
      ++staged;
    } else
      repeated_draws += stream()->composition_token().has_value();
    return token;
  }
  void complete(std::uint64_t token, bool presented) override {
    check(token && token <= issued, "camera receipt token never issued");
    check(!outstanding.empty() && outstanding.front() == token,
          "camera receipt duplicated or out of order");
    input_->complete(token, presented);
    outstanding.pop_front();
    ++completed;
    shown += presented;
  }
  void suspend() override {
    retired += outstanding.size();
    outstanding.clear();
    input_->suspend();
  }
  bool composition_fresh() const override { return input_->composition_fresh(); }
  std::deque<std::uint64_t> outstanding;
  std::uint64_t issued = 0, staged = 0, completed = 0, shown = 0, retired = 0, repeated_draws = 0;

 private:
  std::shared_ptr<v::CameraInput> input_;
};
}  // namespace

int main(int argc, char**) try {
  check(argc == 1,
        "usage: theme-camera-display-probe (run under the isolated Wayland test harness)");
  const auto origin = now_ns();
  bool locked = true;
  auto clock_source = [&]() -> std::optional<headunit::AirplayClockSample> {
    if (!locked) return {};
    const auto at = now_ns(), elapsed = at - origin;
    // Only the network clock is a synthetic fixture. GPU completion and every
    // presentation timestamp, correlation and receipt come from Wayland.
    const auto ntp =
        ((900 + elapsed / 1000000000) << 32) + ((elapsed % 1000000000) << 32) / 1000000000;
    return headunit::AirplayClockSample{ntp + (std::uint64_t{2208988800} << 32), at, at, 1000};
  };
  const s::Display spec{"camera-fixture", 128, 64};
  s::Runtime runtime(
      [](std::string_view) -> std::string { throw std::runtime_error("fixture has no resources"); },
      spec);
  auto phone = std::make_shared<PhoneInput>(runtime.add_video_input({"phone", 64, 64}));
  const auto config = configuration();
  auto camera_stream = runtime.add_video_input({"camera", 64, 64, 1. / 30, v::InputType::camera});
  auto queue = std::make_shared<c::Queue>(config);
  std::uint64_t camera_age_shift = 0;
  bool camera_visible = false;
  auto camera = std::make_shared<v::CameraInput>(
      config, camera_stream, queue, [&] { return now_ns() + camera_age_shift; },
      [&](bool visible, std::uint64_t, std::uint64_t) { camera_visible = visible; });
  auto camera_trace = std::make_shared<CameraTrace>(camera);
  c::SceneBuilder labels(config);
  struct Overlay {
    bool visible, camera_source, phone_prepared;
    const g::Image* label;
  };
  std::vector<Overlay> overlays;
  bool expire_once = true;
  unsigned expirations = 0;
  g::GpuDisplayOptions options;
  options.window.title = "Headunit original camera receipt fixture";
  options.window.max_pending_presentations = 2;  // Exercise repeated draws of one source.
  options.frame_interval_ns = 1000000;
  options.scene_completion_timeout_ns = 2000000000;
  options.inputs = {phone, camera_trace};
  options.overlay = [&](g::Size output) {
    check(overlays.size() < 256, "fixture overlay budget exceeded");
    auto label = labels.status_overlay(
        true, camera_visible ? c::Availability::ready : c::Availability::missing, true, output);
    check(label && !label->children.empty() && label->children.back()->image,
          "camera status label missing");
    overlays.push_back({camera_visible, camera_stream->composition_token().has_value(),
                        !phone->outstanding.empty(), label->children.back()->image.get()});
    if (expirations && !camera_visible) camera_age_shift = 0;
    return label;
  };
  options.slow_frame_threshold_ns = 1;
  options.slow_frame = [&](const g::GpuSlowFrameTrace&) {
    if (expire_once && camera_stream->composition_token()) {
      check(phone->outstanding.size() == 1, "expiry did not share a newly staged phone submission");
      camera_age_shift = std::uint64_t(config.maximum_age_ms) * 1000000 + 1;
      expire_once = false;
      ++expirations;
    }
  };
  g::GpuDisplay display(runtime, spec, clock_source, std::move(options));
  using Phase = s::UiWorker::DisplayPhase;
  auto turn = display.turn(Phase::bootstrap);
  check(!turn.pts, "GPU submission invented bootstrap presentation time");
  until(
      [&] {
        turn = display.turn(Phase::bootstrap);
        return turn.pts.has_value();
      },
      "bootstrap feedback timed out");
  runtime.tick(turn.pts);
  runtime.evaluate(R"(
    local root=CP.DPLayer.create()
    root.frame=CP.DPRect(0,0,128,64)
    root.backgroundColor=CP.DPColor(0,0,0,1)
    for index,name in ipairs({'phone','camera'}) do
      local layer=CP.DPVideoLayer.create(name)
      layer.frame=CP.DPRect((index-1)*64,0,64,64)
      layer:subscribe()
      root:addSublayer(layer)
    end
    CP.setRootLayer('camera-fixture',root)
  )",
                   "original-camera-receipt-fixture");
  runtime.start_tasks(turn.pts);
  camera->set_active(true);
  const auto offer = [&] {
    auto image = pixels(0xffdd8822);
    const auto captured = now_ns();
    check(queue->offer_image(image, captured, captured, true), "camera fixture capture rejected");
    phone->request();
  };
  const auto drain = [&] {
    until(
        [&] {
          display.turn(Phase::bootstrap);
          return !display.stats().pending;
        },
        "actual compositor receipts failed to drain");
  };

  offer();
  until(
      [&] {
        display.turn();
        return expirations == 1;
      },
      "post-composition camera expiry was not reached");
  check(overlays.size() == 2 && overlays[0].visible && overlays[0].camera_source &&
            overlays[0].phone_prepared && !overlays[1].visible && !overlays[1].camera_source &&
            overlays[1].phone_prepared && overlays[0].label != overlays[1].label,
        "expiry did not regenerate the missing label while retaining the phone lease");
  check(camera_trace->retired == 1 && camera_trace->outstanding.empty() &&
            !camera_stream->composition_token() && phone->outstanding.size() == 1 &&
            phone->completed == 0,
        "expiry abandoned the other source or retained a camera preparation");
  check(display.stats().scene_submissions == 1 && display.stats().scene_presentations == 0,
        "composition was confused with actual presentation");
  drain();
  check(phone->completed == 1 && phone->shown == 1 && phone->outstanding.empty() &&
            !phone->stream()->next_pts() && camera_trace->completed == 0 &&
            queue->statistics().outstanding_leases == 0 && display.stats().scene_presentations == 1,
        "missing-scene receipt did not retire the phone source exactly once");

  // Two permitted Wayland commits may draw one preparation repeatedly. Any
  // discarded-first ownership handoff must still complete that source once.
  offer();
  until(
      [&] {
        display.turn();
        return phone->completed == 2 && camera_trace->completed == 1;
      },
      "fresh camera/phone recovery receipts timed out");
  // Bootstrap drains deliberately suspend camera input. Check the retained
  // capture in the active phase, before returning to bootstrap for receipts.
  check(camera_visible && phone->shown == 2 && camera_trace->shown == 1 && phone->staged == 2 &&
            camera_trace->staged == 2 && phone->outstanding.empty() &&
            camera_trace->outstanding.empty() && camera_stream->current_pts() &&
            !camera_stream->next_pts() && queue->current(now_ns()),
        "fresh recovery leaked or duplicated a source preparation");
  drain();
  check(!camera_stream->current_pts() && !queue->current(now_ns()),
        "bootstrap drain retained an active camera capture");
  const auto before_cancel = display.stats();
  const auto discarded_receipts = before_cancel.feedback_receipts - before_cancel.presentations -
                                  before_cancel.ignored_receipts;

  offer();
  until(
      [&] {
        display.turn();
        return phone->staged == 3 && camera_trace->staged == 3;
      },
      "cancellation sources were not staged");
  const auto phone_completed = phone->completed, camera_completed = camera_trace->completed;
  locked = false;
  until(
      [&] {
        display.turn(Phase::bootstrap);
        return display.stats().ignored_receipts > before_cancel.ignored_receipts;
      },
      "clock-loss receipt was not invalidated");
  drain();
  check(phone->retired == 1 && camera_trace->retired == 2 && phone->completed == phone_completed &&
            camera_trace->completed == camera_completed && phone->outstanding.empty() &&
            camera_trace->outstanding.empty() && !phone->stream()->composition_token() &&
            !camera_stream->composition_token() && queue->statistics().outstanding_leases == 0,
        "clock suspension left staged ownership or completed retired tokens");
  locked = true;
  until(
      [&] {
        turn = display.turn(Phase::bootstrap);
        return turn.pts.has_value();
      },
      "clock recovery feedback timed out");
  offer();
  until(
      [&] {
        display.turn();
        return phone->completed == 3 && camera_trace->completed == 2;
      },
      "post-cancellation sources did not recover");
  check(phone->shown == 3 && camera_trace->shown == 2 && phone->staged == 4 &&
            camera_trace->staged == 4 && phone->outstanding.empty() &&
            camera_trace->outstanding.empty() && !display.stats().faulted &&
            camera->statistics().presented == 2 && camera->statistics().discarded == 2 &&
            queue->statistics().presented == 2 && camera_stream->current_pts() &&
            !camera_stream->next_pts() && queue->current(now_ns()) &&
            queue->statistics().maximum_outstanding_leases <= c::Queue::maximum_leases,
        "post-cancellation recovery violated source ownership bounds");
  drain();
  check(!camera_stream->current_pts() && !queue->current(now_ns()),
        "final bootstrap drain retained an active camera capture");
  std::cout << "GPU camera probe: expiry redraw and regenerated label, actual phone/camera "
               "receipts, clock cancellation and recovery passed; "
            << "repeatedPhoneDraws=" << phone->repeated_draws
            << " repeatedCameraDraws=" << camera_trace->repeated_draws
            << " discardedReceipts=" << discarded_receipts
            << " (handoff coverage depends on real compositor discards; no scanout claim)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
