// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/camera_input.hpp>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace c = headunit::camera;
namespace v = headunit::theme::video;
namespace g = headunit::theme::graphics;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "invalid input accepted");
}
constexpr std::uint64_t second = 1000000000, epoch = std::uint64_t{2208988800} << 32;
std::uint64_t ntp(std::uint64_t seconds, std::uint64_t nanos = 0) {
  return (seconds << 32) + (nanos << 32) / second;
}
v::ClockSample sample(std::uint64_t mono, std::uint64_t media) {
  return {true, media + epoch, mono, mono, mono, 0};
}
c::Configuration configuration() {
  c::Configuration result;
  result.enabled = true;
  result.manual_activation = true;
  result.source = c::SourceKind::external;
  result.width = 64;
  result.height = 64;
  result.canvas_width = 64;
  result.canvas_height = 64;
  result.destination = {0, 0, 64, 64};
  return result;
}
struct Fixture {
  c::Configuration config = configuration();
  std::uint64_t now = 10 * second;
  std::shared_ptr<v::Memory> memory = std::make_shared<v::Memory>();
  std::shared_ptr<v::Stream> stream = std::make_shared<v::Stream>(
      v::Input{config.stream_identifier, 64, 64, 1. / 30, v::InputType::camera}, memory);
  std::shared_ptr<c::Queue> queue = std::make_shared<c::Queue>(config);
  struct Gate {
    bool on;
    std::uint64_t capture, observed;
  };
  std::vector<Gate> gates;
  v::MediaClock clock;
  v::CameraInput input{config, stream, queue, [this] { return now; },
                       [this](bool on, std::uint64_t capture, std::uint64_t observed) {
                         gates.push_back({on, capture, observed});
                       }};
  Fixture() {
    clock.update(sample(now, ntp(900)));
    input.set_active(true);
  }
  void image(std::uint64_t captured, std::uint32_t colour = 0xff123456) {
    g::Image image{64, 64, std::vector<std::uint32_t>(4096, colour)};
    check(queue->offer_image(image, captured, now), "image offer failed");
  }
};
void mapped_and_ordered() {
  Fixture f;
  f.image(f.now);
  f.input.pump(&f.clock);
  check(f.gates.back().on && f.gates.back().capture == f.now, "capture gate not fresh");
  const auto a = f.input.stage(f.clock);
  check(a && *a == 1, "camera not staged");
  const auto pixels = f.stream->image();
  check(pixels && pixels->pixels.front() == 0xff123456, "camera image changed");
  check(f.queue->statistics().outstanding_leases == 1 && f.memory->bytes == 16384,
        "shared pixels unaccounted");
  f.now += 40000000;
  f.image(f.now, 0xff654321);
  f.input.pump(&f.clock);
  const auto b = f.input.stage(f.clock);
  check(b && *b == 2, "second camera source not distinct");
  rejects([&] { f.input.complete(*b, true); });
  f.input.complete(*a, false);
  check(!f.queue->current(f.now) && !f.stream->current_pts(), "discarded frame became current");
  f.input.complete(*b, true);
  check(f.queue->current(f.now)->sequence == 2 && f.stream->current_pts(),
        "second source was lost");
  check(!f.input.stage(f.clock) && f.stream->composition_token() == b,
        "repeat minted a new capture token");
  check(f.input.statistics().presented == 1 && f.input.statistics().discarded == 1,
        "receipt accounting wrong");
  // The rejected source remains owned by an independent GPU snapshot.
  f.input.suspend();
  check(!f.stream->current_pts() && f.memory->bytes == 16384 &&
            f.queue->statistics().outstanding_leases == 1,
        "snapshot lifetime lost");
  f.input.complete(*b, true);
  rejects([&] { f.input.complete(3, true); });
}
void history_and_phase() {
  Fixture f;
  f.now += 40000000;
  f.image(f.now);
  f.now += 10000000;
  f.clock.update(sample(f.now, ntp(900, 50000000)));
  check(!f.clock.ready(f.now - 10000000), "latest anchor incorrectly covers older event");
  f.input.pump(&f.clock);
  const auto first = f.input.stage(f.clock);
  check(first && *f.stream->next_pts() > 900.039 && *f.stream->next_pts() < 900.041,
        "capture mapped using wrong anchor");
  f.input.complete(*first, true);
  // Allowed negative clock phase correction: this capture maps below the
  // previous 900.040 source. Drop it rather than clamping or poisoning clock.
  f.now += 1000000;
  f.clock.update(sample(f.now, ntp(900, 31000000)));
  f.image(f.now);
  f.input.pump(&f.clock);
  check(!f.input.stage(f.clock) && f.input.statistics().non_monotonic == 1 && !f.clock.failed(),
        "phase regression not dropped");
  f.now += 40000000;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto next = f.input.stage(f.clock);
  check(next && *next == 2, "phase recovery failed");
  f.input.complete(*next, true);
  f.clock.suspend();
  f.input.suspend();
  f.now += second;
  f.image(f.now - 10000000);
  f.clock.update(sample(f.now, ntp(901, 71000000)));
  f.input.pump(&f.clock);
  check(!f.input.stage(f.clock) && f.input.statistics().unmapped == 1,
        "pre-relock capture given invented timeline");
  f.now += 40000000;
  f.image(f.now);
  f.input.pump(&f.clock);
  check(f.input.stage(f.clock).has_value(), "fresh post-relock capture not accepted");
}
void discarded_capture_visibility() {
  Fixture f;
  const auto original = f.now;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto first = f.input.stage(f.clock);
  check(first.has_value(), "visibility fixture did not stage its first capture");
  f.input.complete(*first, true);
  f.now += 40000000;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto next = f.input.stage(f.clock);
  check(next.has_value(), "visibility fixture did not stage its newer capture");
  f.input.complete(*next, false);
  f.input.pump(&f.clock);
  check(f.gates.back().on && f.gates.back().capture == original,
        "rejected camera capture renewed the visibility publication date");
  f.now = original + 151000000;
  f.input.pump(&f.clock);
  check(!f.gates.back().on && !f.stream->composition_token(),
        "expired retained camera source remained selected");
}
void original_age_and_flush() {
  Fixture f;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto a = f.input.stage(f.clock);
  f.input.complete(*a, true);
  f.now += 151000000;
  f.image(f.now);
  f.input.pump(&f.clock);
  check(!f.stream->composition_token() && !f.gates.back().on && f.input.statistics().expired == 1,
        "new capture renewed expired displayed texture");
  f.now += 40000000;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto b = f.input.stage(f.clock);
  f.stream->flush();
  f.input.complete(*b, true);
  check(!f.queue->current(f.now) && !f.stream->current_pts(),
        "script-flushed frame became presented");
  f.now += 40000000;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto d = f.input.stage(f.clock);
  f.input.set_active(false);
  f.input.complete(*d, true);
  check(!f.gates.back().on && f.queue->statistics().outstanding_leases == 0,
        "deactivated camera retained source");
  f.input.set_active(true);
  f.now += 40000000;
  f.image(f.now);
  f.input.pump(nullptr);
  check(!f.gates.back().on && !f.input.stage(f.clock), "unqualified pump retained camera pixels");
  f.now += 40000000;
  f.image(f.now);
  f.input.pump(&f.clock);
  check(f.input.stage(f.clock).has_value(), "camera not restaged");
  f.now += 151000000;
  check(!f.input.composition_fresh(), "capture stayed eligible after slow GPU draw");
  f.input.suspend();
  check(f.input.composition_fresh(), "empty composition is stale");
}
void shared_capture_permits() {
  Fixture f;
  std::vector<std::shared_ptr<const g::Image>> gpu;
  for (unsigned i = 0; i < c::Queue::maximum_leases; ++i) {
    f.now += 40000000;
    f.image(f.now);
    f.input.pump(&f.clock);
    const auto token = f.input.stage(f.clock);
    check(token.has_value(), "shared camera source not staged");
    gpu.push_back(f.stream->image());
    // A second scene can draw this SAME source while its first discarded
    // commit's ownership transfers to the later receipt. No new stage/token.
    check(f.stream->composition_token() == token && !f.input.stage(f.clock),
          "repeat changed capture token");
    f.input.complete(*token, true);
  }
  f.input.suspend();
  check(f.queue->statistics().outstanding_leases == 4 && !f.queue->can_offer(),
        "shared GPU snapshots lost capture permits");
  gpu.clear();
  check(f.queue->statistics().outstanding_leases == 0 && f.memory->bytes == 0,
        "shared capture permits leaked");
  auto used = std::make_shared<v::Stream>(
      v::Input{f.config.stream_identifier, 64, 64, 1. / 30, v::InputType::camera}, f.memory);
  auto image = std::make_shared<g::Image>(g::Image{64, 64, std::vector<std::uint32_t>(4096)});
  check(used->offer_shared(1, image), "test source offer failed");
  used->flush();
  rejects([&] { v::CameraInput input(f.config, used, f.queue); });
}
void native_permits() {
  Fixture f;
  std::vector<std::shared_ptr<const v::NativeFrame>> gpu;
  auto producer = std::make_shared<int>(42);
  std::weak_ptr<int> lifetime = producer;
  for (unsigned i = 0; i < c::Queue::maximum_leases; ++i) {
    auto native = std::make_shared<v::NativeFrame>();
    native->metadata.width = 64;
    native->metadata.height = 64;
    native->storage_width = 64;
    native->storage_height = 64;
    native->plane_count = 1;
    native->planes[0] = {7, 0, 256};
    native->lifetime = producer;
    f.now += 40000000;
    check(f.queue->offer_native(native, f.now, f.now), "native offer failed");
    f.input.pump(&f.clock);
    const auto token = f.input.stage(f.clock);
    check(token.has_value(), "native not staged");
    gpu.push_back(f.stream->native());
    f.input.complete(*token, true);
  }
  producer.reset();
  f.input.suspend();
  check(
      !lifetime.expired() && f.queue->statistics().outstanding_leases == 4 && !f.queue->can_offer(),
      "GPU did not retain outer capture permits");
  check(gpu.front()->planes[0].fd == 7 && gpu.front()->metadata.sender_ntp == 0,
        "native capture metadata or FD mutated");
  gpu.clear();
  check(lifetime.expired() && f.queue->statistics().outstanding_leases == 0 &&
            f.memory->native_frames == 0,
        "GPU permits leaked");
}
void shared_budget_and_rejection() {
  auto memory = std::make_shared<v::Memory>();
  v::Stream s({"camera", 64, 64, 1. / 30, v::InputType::camera}, memory);
  auto image =
      std::make_shared<g::Image>(g::Image{64, 64, std::vector<std::uint32_t>(4096, 0xffabcdef)});
  memory->bytes = v::Memory::limit;
  check(!s.offer_shared(1, image) && image->pixels.size() == 4096,
        "backpressure consumed shared pixels");
  memory->bytes = 0;
  check(s.offer_shared(1, image) && s.offer_shared(2, image), "immutable source offer failed");
  auto a = s.prepare_next().value();
  auto b = s.prepare_next().value();
  rejects([&] { s.discard_prepared(b.token); });
  s.discard_prepared(a.token);
  s.discard_prepared(b.token);
  check(!s.next_pts() && !s.current_pts() && memory->bytes == 32768,
        "rejected shared source retried or memory lease lost");
  a.image.reset();
  b.image.reset();
  check(memory->bytes == 0, "retained reference budget leaked");
}
void late_capture_on_reentry() {
  Fixture f;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto token = f.input.stage(f.clock);
  check(token.has_value(), "reentry fixture did not stage");
  f.input.complete(*token, true);
  f.input.set_active(false);
  check(!f.gates.back().on && !f.stream->composition_token(),
        "off did not withdraw visibility immediately");
  // Deterministically model a final in-progress producer offer after the off
  // discard. It is fresh, but belongs to the withdrawn activation.
  f.now += 40000000;
  f.image(f.now, 0xffee1111);
  f.input.set_active(true);
  f.input.pump(&f.clock);
  check(!f.input.stage(f.clock) && !f.gates.back().on,
        "late old producer image leaked into a new activation");
  f.now += 40000000;
  f.image(f.now, 0xff11ee11);
  f.input.pump(&f.clock);
  const auto fresh = f.input.stage(f.clock);
  check(fresh && f.stream->image()->pixels.front() == 0xff11ee11,
        "reentry failed to accept a genuinely new capture");
  f.input.complete(*fresh, true);
}
void local_widget() {
  auto config = c::Configuration{};
  config.enabled = true;
  config.source = c::SourceKind::external;
  config.width = config.height = config.canvas_width = config.canvas_height = 64;
  config.destination = {0, 0, 64, 64};
  config.stream_identifier = "adasView";
  auto memory = std::make_shared<v::Memory>();
  auto stream = std::make_shared<v::Stream>(
      v::Input{"adasView", 64, 64, 1. / 30, v::InputType::widget}, memory);
  auto queue = std::make_shared<c::Queue>(config);
  bool rejected = false;
  try {
    v::CameraInput camera(config, stream, queue);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "camera accepted a widget without explicit type admission");
  std::uint64_t at = 10 * second;
  v::LocalInput input(config, stream, queue, [&] { return at; }, {}, v::InputType::widget);
  input.set_active(true);
  input.pump(nullptr);
  check(!stream->composition_token(), "widget invented a phone clock");
  v::MediaClock clock;
  clock.update(sample(at, ntp(900)));
  g::Image pixels{64, 64, std::vector<std::uint32_t>(4096, 0xff123456)};
  check(queue->offer_image(pixels, at, at, true), "widget source rejected");
  input.pump(&clock);
  const auto token = input.stage(clock);
  check(token.has_value(), "widget did not stage through the qualified phone clock");
  input.complete(*token, true);
  check(stream->current_pts().has_value(), "widget did not advance after presentation");
  at += 150000001;
  check(!input.composition_fresh(), "widget redraw renewed acquisition age");
  input.pump(&clock);
  check(!stream->composition_token(), "stale widget pixels retained");
  input.set_active(false);
  check(queue->statistics().outstanding_leases == 0, "widget retirement leaked source permits");
}
void consumed_metadata() {
  Fixture f;
  check(!f.input.consumption(), "requested input invented consumed metadata");
  const auto capture = f.now;
  f.image(capture, 0xff2468ac);
  f.input.pump(&f.clock);
  check(!f.input.consumption(), "queued frame was reported as consumed");
  f.now += 1'000'000;
  const auto first = f.input.stage(f.clock).value();
  auto prepared = f.input.consumption().value();
  check(prepared.source_generation == 1 && prepared.lease_sequence == 1 &&
            prepared.stream_token == first && prepared.capture_monotonic_ns == capture &&
            prepared.prepared_monotonic_ns == f.now && !prepared.receipt_observed_monotonic_ns &&
            prepared.width == 64 && prepared.height == 64 && prepared.stride_bytes[0] == 256 &&
            prepared.input_width == 64 && prepared.input_height == 64 && !prepared.native &&
            !prepared.capture_reference,
        "prepared source metadata does not describe its original pixels/lease/date");
  auto held = f.stream->image();
  f.now += 2'000'000;
  f.input.complete(first, true);
  auto current = f.input.consumption().value();
  check(current.receipt_observed_monotonic_ns == f.now && current.capture_monotonic_ns == capture &&
            held->pixels.front() == 0xff2468ac,
        "receipt or diagnostic copy changed the original source");
  std::ostringstream report;
  current.write_json(report);
  check(report.str().size() < 2048 &&
            report.str().find("\"captureMonoNs\":\"10000000000\"") != std::string::npos &&
            report.str().find("\"target\":\"registered-input\"") != std::string::npos &&
            report.str().find("\"captureGeometry\":null,\"signedSceneFit\":null,") !=
                std::string::npos,
        "report invents unknown geometry or loses exact dates");
  f.now = capture + 40'000'000;
  f.image(f.now, 0xffabcdef);
  f.input.pump(&f.clock);
  check(f.input.consumption()->capture_monotonic_ns == capture,
        "queued metadata replaced the actually consumed source");
  const auto second = f.input.stage(f.clock).value();
  check(f.input.consumption()->stream_token == second &&
            !f.input.consumption()->receipt_observed_monotonic_ns,
        "new prepared source borrowed an older receipt");
  f.input.complete(second, false);
  check(f.input.consumption()->stream_token == first &&
            f.input.consumption()->capture_monotonic_ns == capture,
        "rejected newer frame renewed old metadata");
  f.now = capture + 150'000'000;
  check(f.input.consumption().has_value(), "exact maximum age changed");
  f.now += 1;
  check(!f.input.consumption(), "read before pump revived expired consumed metadata");
  f.input.pump(&f.clock);
  check(!f.input.consumption(), "expired source returned after withdrawal");
  f.now += 40'000'000;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto third = f.input.stage(f.clock).value();
  f.input.suspend();
  f.input.complete(third, true);
  check(!f.input.consumption() && held->pixels.front() == 0xff2468ac,
        "retired receipt revived metadata or destroyed held GPU pixels");
  held.reset();
  check(f.queue->statistics().outstanding_leases == 0 && f.memory->bytes == 0,
        "numeric diagnostic snapshots retained a pixel lease");
  f.now += 40'000'000;
  f.image(f.now);
  f.input.pump(&f.clock);
  const auto fourth = f.input.stage(f.clock).value();
  f.stream->flush();
  check(!f.input.consumption(), "script flush retained consumed metadata");
  f.input.complete(fourth, true);
  check(!f.input.consumption(), "late script-flushed receipt revived metadata");
}
void consumed_native_metadata() {
  Fixture f;
  auto backing = std::make_shared<int>(42);
  std::weak_ptr<int> lease = backing;
  auto native = std::make_shared<v::NativeFrame>();
  native->metadata.width = 64;
  native->metadata.height = 64;
  native->storage_width = 96;
  native->storage_height = 80;
  native->crop_x = 16;
  native->crop_y = 8;
  native->plane_count = 2;
  native->planes[0] = {7, 0, 128};
  native->planes[1] = {8, 128, 64};
  native->lifetime = backing;
  const auto capture = f.now;
  check(f.queue->offer_native(native, capture, f.now, c::CaptureReference::start_of_exposure),
        "explicit native fixture rejected");
  native.reset();
  backing.reset();
  f.input.pump(&f.clock);
  const auto token = f.input.stage(f.clock).value();
  const auto consumed = f.input.consumption().value();
  check(consumed.native && consumed.width == 64 && consumed.height == 64 &&
            consumed.storage_width == 96 && consumed.storage_height == 80 &&
            consumed.crop_x == 16 && consumed.crop_y == 8 && consumed.plane_count == 2 &&
            consumed.stride_bytes[0] == 128 && consumed.stride_bytes[1] == 64 &&
            consumed.capture_reference == c::CaptureReference::start_of_exposure &&
            consumed.capture_monotonic_ns == capture,
        "native consumed storage/crop/reference is not the actual selected sample");
  f.queue->close();
  check(!f.input.consumption(), "terminal source remained consumable before pump");
  f.input.pump(&f.clock);
  f.input.complete(token, true);
  check(!f.input.consumption() && lease.expired() && f.memory->native_frames == 0,
        "late failed-source receipt or numeric snapshot retained backing");
}
void terminal_source_between_pump_and_receipt() {
  Fixture f;
  std::vector<std::shared_ptr<const v::NativeFrame>> gpu;
  std::vector<std::weak_ptr<int>> backing;
  std::vector<std::uint64_t> tokens;
  for (unsigned i = 0; i < 3; ++i) {
    auto producer = std::make_shared<int>(42 + i);
    backing.push_back(producer);
    auto native = std::make_shared<v::NativeFrame>();
    native->metadata = {0, 9876 + i, 64, 64};
    native->storage_width = 96;
    native->storage_height = 80;
    native->crop_x = 16;
    native->crop_y = 8;
    native->plane_count = 1;
    native->planes[0] = {7 + int(i), 0, 256};
    native->lifetime = producer;
    f.now += 40'000'000;
    check(f.queue->offer_native(native, f.now, f.now, c::CaptureReference::start_of_exposure),
          "terminal receipt fixture rejected native capture");
    f.input.pump(&f.clock);
    tokens.push_back(f.input.stage(f.clock).value());
    gpu.push_back(f.stream->native());
    if (!i) f.input.complete(tokens.back(), true);
  }
  const auto selected = f.input.consumption().value();
  check(selected.stream_token == tokens.back() && selected.lease_sequence == 3 &&
            selected.capture_monotonic_ns == f.now && !selected.receipt_observed_monotonic_ns &&
            f.stream->current_pts() && f.gates.back().on,
        "terminal receipt fixture lost current or prepared ownership");
  // The capture worker can retire its Queue after the last display pump,
  // including during composition or before an older scene receipt arrives.
  // No input pump/suspend is allowed to hide that interval in this regression.
  f.queue->close();
  const bool guarded = !f.input.composition_fresh();
  f.input.complete(tokens[1], true);
  const bool withdrawn = !f.stream->composition_token() && !f.stream->current_pts() &&
                         !f.stream->prepared(tokens[2]) && !f.gates.back().on &&
                         !f.input.consumption() && f.input.statistics().presented == 1;
  if (!guarded || !withdrawn) {
    std::cerr << "terminal source before pump: composition withdrawn=" << guarded
              << ", receipt withdrawn=" << withdrawn << '\n';
  }
  check(guarded && withdrawn,
        "retired Queue authorized composition or advanced Stream on a late receipt");
  f.input.complete(tokens[2], true);
  f.input.complete(tokens[1], true);
  check(!f.stream->composition_token() && f.input.statistics().presented == 1 &&
            f.input.statistics().discarded == 2 && f.queue->statistics().cancelled == 2 &&
            f.queue->statistics().presented == 1,
        "retired receipts changed source or receipt accounting");
  check(f.queue->statistics().outstanding_leases == 3 && f.memory->native_frames == 3,
        "withdrawal released held native imports before their owner");
  for (unsigned i = 0; i < gpu.size(); ++i) {
    check(!backing[i].expired() && gpu[i]->planes[0].fd == 7 + int(i) &&
              gpu[i]->metadata.sender_ntp == 0 && gpu[i]->metadata.timestamp_ns == 9876 + i &&
              gpu[i]->crop_x == 16 && gpu[i]->crop_y == 8,
          "terminal withdrawal changed borrowed native backing or metadata");
  }
  gpu.clear();
  for (const auto& lease : backing)
    check(lease.expired(), "retired native producer backing leaked after import release");
  check(!f.queue->statistics().outstanding_leases && !f.memory->native_frames,
        "terminal receipt withdrawal leaked capture permits");
}
int main() {
  local_widget();
  try {
    mapped_and_ordered();
    history_and_phase();
    discarded_capture_visibility();
    original_age_and_flush();
    native_permits();
    shared_budget_and_rejection();
    shared_capture_permits();
    late_capture_on_reentry();
    consumed_metadata();
    consumed_native_metadata();
    terminal_source_between_pump_and_receipt();
    std::cout
        << "camera input clocks, receipts, original age, shared pixels and GPU permits passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
