// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/camera_input.hpp>
#include <headunit/camera_controller_io.hpp>
#include <iostream>
#include <stdexcept>

namespace c = headunit::camera;
namespace v = headunit::theme::video;
namespace g = headunit::theme::graphics;
namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct ProviderState {
  unsigned opened = 0, live = 0, retired = 0, fail_generation = 0;
  bool retirement_allowed = false;
} owner;
// Only the source-factory boundary is substituted. The production input,
// Queue, Stream, capture leases, clock mapping and receipt paths run unchanged.
// Delayed stop and failure are explicit models, not driver/kernel evidence.
class RetiringProvider final : public c::Provider {
  c::Queue& queue_;
  unsigned generation_;
  bool stopping_ = false;
  std::optional<std::uint64_t> last_;

 public:
  explicit RetiringProvider(c::Queue& queue) : queue_(queue), generation_(++owner.opened) {
    check(!owner.live, "camera reopened before old source owner retired");
    ++owner.live;
  }
  ~RetiringProvider() override {
    --owner.live;
    ++owner.retired;
  }
  bool poll(std::uint64_t at, c::Queue& queue) override {
    check(&queue == &queue_, "provider moved between camera generations");
    if (owner.fail_generation == generation_) {
      queue_.close();
      return false;
    }
    if (stopping_ || last_ == at) return false;
    g::Image image{64, 64, std::vector<std::uint32_t>(4096, 0xff000000 | generation_)};
    const bool offered = queue_.offer_image(image, at, at, true);
    if (offered) last_ = at;
    return offered;
  }
  void request_stop() noexcept override { stopping_ = true; }
  bool stopped() const noexcept override { return stopping_ && owner.retirement_allowed; }
};
c::Configuration configuration(c::SourceKind source) {
  c::Configuration result;
  result.enabled = true;
  result.manual_activation = true;
  result.source = source;
  result.width = 64;
  result.height = 64;
  result.canvas_width = 64;
  result.canvas_height = 64;
  result.destination = {0, 0, 64, 64};
  return result;
}
std::shared_ptr<v::Stream> stream(const c::Configuration& config,
                                  const std::shared_ptr<v::Memory>& memory) {
  return std::make_shared<v::Stream>(
      v::Input{config.stream_identifier, 64, 64, 1. / 30, v::InputType::camera}, memory);
}
void owned_source_recovery() {
  owner = {};
  auto config = configuration(c::SourceKind::synthetic_test_pattern);
  auto memory = std::make_shared<v::Memory>();
  auto video = stream(config, memory);
  auto original = std::make_shared<c::Queue>(config);
  std::uint64_t now = 10'000'000'000ULL;
  bool visible = false;
  std::uint64_t acquisition = 0;
  v::MediaClock clock;
  clock.update({true, (std::uint64_t{2208989700} << 32), now, now, now, 0});
  v::CameraInput input(
      config, video, original, [&] { return now; },
      [&](bool on, std::uint64_t capture, std::uint64_t) {
        visible = on;
        acquisition = capture;
      });
  input.set_active(true);
  input.pump(&clock);
  const auto first = input.stage(clock);
  check(first && visible && acquisition == now, "initial camera source unavailable");
  check(c::source_control_phase(true, input.availability()) == c::ControlPhase::active,
        "fresh camera source did not report active eligibility");
  auto old_gpu = video->image();
  input.complete(*first, true);
  const auto initial = input.consumption().value();
  check(initial.source_generation == 1 && initial.stream_token == *first && initial.simulated,
        "initial consumed source identity missing");
  owner.fail_generation = 1;
  now += 40'000'000;
  input.pump(&clock);
  check(!input.consumption(), "failed source retained consumed metadata");
  check(!visible && !video->composition_token() && input.availability() == c::Availability::closed,
        "terminal camera failure retained visibility or pixels");
  check(c::source_control_phase(true, input.availability()) == c::ControlPhase::fault,
        "terminal camera source was reported as transient waiting");
  for (unsigned i = 0; i < 64; ++i) {
    input.set_active(true);
    input.pump(&clock);
  }
  check(owner.opened == 1, "held On request retried a terminal camera source");
  input.set_active(false);
  check(c::source_control_phase(false, input.availability()) == c::ControlPhase::idle,
        "explicit Off retained terminal camera fault status");
  input.set_active(true);
  for (unsigned i = 0; i < 64; ++i)
    input.pump(&clock);
  check(owner.live == 1 && owner.opened == 1 && !visible && !input.stage(clock),
        "new activation competed with a retiring provider");
  owner.retirement_allowed = true;
  now += 40'000'000;
  input.pump(&clock);
  check(owner.opened == 2 && owner.retired == 1 && owner.live == 1 && visible && acquisition == now,
        "explicit reentry did not create a fresh source after retirement");
  check(c::source_control_phase(true, input.availability()) == c::ControlPhase::active,
        "recovered camera source retained fault status");
  check(original->availability(now) == c::Availability::closed &&
            old_gpu->pixels.front() == 0xff000001,
        "recovery reopened old queue or changed retained GPU backing");
  const auto fresh = input.stage(clock);
  check(fresh && *fresh > *first && video->image()->pixels.front() == 0xff000002,
        "new source reused a receipt token or old capture");
  const auto replacement = input.consumption().value();
  check(replacement.source_generation == 2 && replacement.lease_sequence == 1 &&
            replacement.stream_token == *fresh && replacement.capture_monotonic_ns == now &&
            !replacement.receipt_observed_monotonic_ns,
        "replacement borrowed old generation/receipt metadata");
  input.complete(*first, true);
  check(video->prepared(*fresh) && !video->current_pts(),
        "late old receipt changed replacement generation");
  check(input.consumption()->source_generation == replacement.source_generation &&
            !input.consumption()->receipt_observed_monotonic_ns,
        "late old receipt republished consumed metadata");
  input.complete(*fresh, true);
  check(input.statistics().presented == 2, "new real receipt was not accepted");
  old_gpu.reset();
  check(memory->bytes == 64 * 64 * 4, "retained old camera backing leaked after release");
  input.set_active(false);
  check(!input.consumption(), "Off retained consumed replacement metadata");
  check(!visible && !video->composition_token(), "recovered source did not withdraw on Off");
}
void external_queue_remains_terminal() {
  const auto before = owner.opened;
  auto config = configuration(c::SourceKind::external);
  auto queue = std::make_shared<c::Queue>(config);
  auto video = stream(config, std::make_shared<v::Memory>());
  std::uint64_t now = 10'000'000'000ULL;
  v::CameraInput input(config, video, queue, [&] { return now; });
  input.set_active(true);
  check(c::source_control_phase(true, input.availability()) == c::ControlPhase::waiting,
        "unstarted external source reported a terminal failure");
  g::Image image{64, 64, std::vector<std::uint32_t>(4096, 0xff000001)};
  check(queue->offer_image(image, now, now), "external fixture capture was not accepted");
  now += 151'000'000;
  check(input.availability() == c::Availability::stale &&
            c::source_control_phase(true, input.availability()) == c::ControlPhase::waiting,
        "stale frame incorrectly reported a terminal source fault");
  now = 9'000'000'000;
  check(input.availability() == c::Availability::clock_reversed &&
            c::source_control_phase(true, input.availability()) == c::ControlPhase::fault,
        "source clock failure remained transient waiting");
  queue->close();
  input.set_active(false);
  input.set_active(true);
  check(input.availability() == c::Availability::closed && owner.opened == before,
        "host replaced an externally owned terminal queue");
}
}  // namespace
namespace headunit::camera {
std::unique_ptr<Provider> make_provider(const Configuration& config, Queue& queue) {
  if (config.source == SourceKind::external) return {};
  check(config.source == SourceKind::synthetic_test_pattern,
        "fixture attempted a physical provider");
  return std::make_unique<RetiringProvider>(queue);
}
}  // namespace headunit::camera
int main() try {
  owned_source_recovery();
  check(owner.live == 0, "camera input destructor retained provider");
  external_queue_remains_terminal();
  std::cout << "Camera explicit fault reentry, delayed retirement, old GPU leases/receipts and "
               "external terminal ownership passed (provider boundary modeled)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
