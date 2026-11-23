// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_display.hpp>
#include <cmath>
#include <iostream>
#include <limits>

namespace c = headunit::camera;
namespace g = headunit::theme::graphics;
namespace v = headunit::theme::video;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
template <class F>
void rejects(F f) {
  bool bad = false;
  try {
    f();
  } catch (const std::exception&) {
    bad = true;
  }
  check(bad, "invalid camera display operation accepted");
}
c::Configuration configuration() {
  c::Configuration x;
  x.enabled = true;
  x.manual_activation = true;
  x.width = 128;
  x.height = 96;
  x.canvas_width = 400;
  x.canvas_height = 240;
  x.destination = {80, 56, 240, 160};
  return x;
}
std::shared_ptr<const c::Frame> fixture() {
  auto image = std::make_shared<g::Image>(g::Image{128, 96, std::vector<std::uint32_t>(128 * 96)});
  constexpr std::uint32_t colours[] = {0xffee1100, 0xff22dd00, 0xff0033ee, 0xffdd9900};
  for (unsigned y = 0; y < 96; ++y)
    for (unsigned x = 0; x < 128; ++x)
      image->pixels[y * 128 + x] = colours[2 * (y >= 48) + (x >= 64)];
  auto frame = std::make_shared<c::Frame>();
  frame->sequence = 1;
  frame->capture_monotonic_ns = 1000000000;
  frame->simulated = true;
  frame->image = std::move(image);
  return frame;
}
v::PresentationFeedback feedback(std::uint64_t token = 1, std::uint64_t shift = 0) {
  v::PresentationFeedback f;
  f.token = token;
  f.presented = true;
  f.seconds = 10;
  f.nanoseconds = 100000000 + shift;
  f.clock_id = 4;
  f.flags = 7;
  f.refresh_ns = 16666667;
  f.received_ns = 21010100000 + shift;
  f.correlation =
      v::ClockCorrelation{4, 10110000000 + shift, 21010000000 + shift, 21010002000 + shift};
  return f;
}
int main(int argc, char** argv) try {
  check(argc <= 2, "usage: camera-display-test [PNG]");
  const auto frame = fixture();
  for (unsigned degrees : {0u, 90u, 180u, 270u})
    for (bool mirror : {false, true}) {
      auto config = configuration();
      config.rotation_degrees = degrees;
      config.mirror_horizontal = mirror;
      const auto transform = c::capture_transform(config);
      check(bool(transform.inverse()), "camera transform collapsed");
      double minx = 1e10, miny = 1e10, maxx = -1, maxy = -1;
      for (g::Point point : std::initializer_list<g::Point>{{0, 0}, {128, 0}, {0, 96}, {128, 96}}) {
        const auto out = transform.apply(point);
        minx = std::min(minx, out.x);
        miny = std::min(miny, out.y);
        maxx = std::max(maxx, out.x);
        maxy = std::max(maxy, out.y);
        check(
            out.x >= 80 - 1e-8 && out.x <= 320 + 1e-8 && out.y >= 56 - 1e-8 && out.y <= 216 + 1e-8,
            "transformed camera exceeded configured rectangle");
      }
      check(std::abs((minx + maxx) / 2 - 200) < 1e-8 && std::abs((miny + maxy) / 2 - 136) < 1e-8,
            "camera not centered within destination");
      const double xscale = std::hypot(transform.a, transform.b),
                   yscale = std::hypot(transform.c, transform.d);
      check(std::abs(xscale - yscale) < 1e-8, "camera aspect ratio was stretched");
      c::SceneBuilder builder(config);
      const auto scene = builder.build(frame, c::Availability::ready);
      const auto rendered = g::render(std::const_pointer_cast<g::Layer>(scene.root), 400, 240);
      for (g::Point p : std::initializer_list<g::Point>{{32, 24}, {96, 24}, {32, 72}, {96, 72}}) {
        const auto out = transform.apply(p);
        const auto actual =
            rendered
                .pixels[std::size_t(std::lround(out.y)) * 400 + std::size_t(std::lround(out.x))];
        const auto expected = frame->image->pixels[std::size_t(p.y) * 128 + std::size_t(p.x)];
        check(actual == expected, "GPU scene transform differs from its pixel coordinate mapping");
      }
      const auto corner = rendered.pixels.front();
      check((corner >> 24) == 255 && ((corner >> 16) & 255) < 8 && ((corner >> 8) & 255) < 8 &&
                (corner & 255) < 8,
            "camera capture escaped the canvas placement");
      const auto badge = scene.root->children.back()->children.back();
      check(badge->image && badge->image->pixels.size() == 512 * 80, "simulation label absent");
      check(frame->image->pixels.front() == 0xffee1100 && frame->image->pixels.back() == 0xffdd9900,
            "camera scene mutated provider pixels");
    }
  auto config = configuration();
  c::SceneBuilder builder(config);
  const auto live = builder.build(frame, c::Availability::ready),
             missing = builder.build({}, c::Availability::missing),
             stale = builder.build(frame, c::Availability::stale),
             reversed = builder.build(frame, c::Availability::clock_reversed),
             closed = builder.build({}, c::Availability::closed);
  check(live.root->children.size() == 2 && stale.root->children.size() == 1 &&
            !stale.root->children.front()->video,
        "stale state retained a camera texture");
  const auto tile = [](const c::CameraScene& scene) {
    return scene.root->children.back()->children.back()->image;
  };
  check(tile(builder.build({}, c::Availability::ready)) == tile(missing),
        "ready state without pixels falsely announced a camera image");
  check(tile(missing) != tile(stale) && tile(stale) != tile(reversed) &&
            tile(reversed) != tile(closed),
        "unavailable states are visually indistinguishable");
  check(tile(builder.build(frame, c::Availability::ready)) == tile(live),
        "unchanged simulation label was re-rasterized");
  check(!builder.status_overlay(false, c::Availability::ready, true, {400, 240}),
        "inactive camera overlay visible");
  const auto half = builder.status_overlay(true, c::Availability::stale, false, {200, 120});
  check(half->transform.a == .5 && half->transform.d == .5,
        "camera status ignored output geometry");
  const auto transparent = g::render(std::const_pointer_cast<g::Layer>(half), 200, 120);
  check(transparent.pixels.front() == 0 && (transparent.pixels[30 * 200 + 42] >> 24) == 255,
        "unavailable camera overlay covered unrelated UI or retained a stale camera texture");
  rejects([&] { builder.status_overlay(true, c::Availability::stale, false, {0, 120}); });
  rejects([&] {
    auto bad = std::make_shared<c::Frame>(*frame);
    bad->image = std::make_shared<g::Image>();
    builder.build(bad, c::Availability::ready);
  });
  auto native = std::make_shared<v::NativeFrame>();
  native->metadata = {0, 99, 128, 96};
  native->lifetime = std::make_shared<int>(42);
  auto camera = std::make_shared<c::Frame>();
  camera->native = native;
  camera->capture_monotonic_ns = 1000000000;
  const auto native_scene = builder.build(camera, c::Availability::ready);
  check(native_scene.native_stream &&
            native_scene.native_stream->input().type == v::InputType::camera &&
            !native_scene.native_stream->current_pts() &&
            native_scene.native_stream->prepared(*native_scene.native_token),
        "native camera texture falsely advanced before a receipt or was typed as phone video");
  check(native->metadata.sender_ntp == 0 && native->metadata.timestamp_ns == 99,
        "camera scene fabricated native phone timestamps");
  native_scene.native_stream->presented(*native_scene.native_token);
  check(native_scene.native_stream->current_pts() == 1.,
        "actual local texture receipt not applied");

  // Retaining only the generated scene must also retain the provider's outer
  // permit. Holding just the pixel/sample pointer would bypass the four-frame
  // limit after its separately staged camera::Frame reference is released.
  c::Queue scene_queue(config);
  std::vector<c::CameraScene> retained;
  for (unsigned i = 0; i < c::Queue::maximum_leases; ++i) {
    auto pixels = *frame->image;
    check(scene_queue.offer_image(pixels, 100 + i, 100 + i, true), "scene permit filled early");
    auto staged = scene_queue.stage(100 + i);
    retained.push_back(builder.build(staged, c::Availability::ready));
    scene_queue.discard();
    staged.reset();
  }
  auto blocked = *frame->image;
  check(scene_queue.statistics().outstanding_leases == 4 &&
            !scene_queue.offer_image(blocked, 200, 200, true) && !blocked.pixels.empty(),
        "escaped image scene bypassed camera frame permit bound");
  retained.pop_back();
  check(scene_queue.statistics().outstanding_leases == 3 &&
            scene_queue.offer_image(blocked, 200, 200, true),
        "destroyed image scene failed to release capture permit");
  retained.clear();
  scene_queue.discard();
  check(scene_queue.statistics().outstanding_leases == 0, "image scene leaked frame permit");
  auto external = config;
  external.source = c::SourceKind::external;
  c::Queue native_queue(external);
  {
    auto sample = std::make_shared<v::NativeFrame>();
    sample->metadata = {0, 99, 128, 96};
    sample->storage_width = 128;
    sample->storage_height = 96;
    sample->plane_count = 1;
    sample->planes[0] = {17, 0, 512};
    sample->lifetime = std::make_shared<int>(42);
    check(native_queue.offer_native(sample, 300, 300), "native scene permit capture rejected");
    auto staged = native_queue.stage(300);
    const auto held_scene = builder.build(staged, c::Availability::ready);
    native_queue.discard();
    staged.reset();
    sample.reset();
    check(native_queue.statistics().outstanding_leases == 1 && held_scene.native_stream->native(),
          "escaped native scene bypassed outer frame permit");
  }
  check(native_queue.statistics().outstanding_leases == 0, "native scene leaked frame permit");

  // Two genuine presentation submissions can be outstanding while B's cold
  // draw expires. Replacing B's pixels with a stale scene must retain its
  // staged capture token until A and then the stale-status receipt complete.
  c::Queue ordered(config);
  auto pixels_a = *frame->image, pixels_b = *frame->image;
  check(ordered.offer_image(pixels_a, 21000000000, 21000000000, true),
        "cold-order capture A rejected");
  auto capture_a = ordered.stage(21000000000);
  check(ordered.offer_image(pixels_b, 21000000900, 21000000900, true),
        "cold-order capture B rejected");
  auto capture_b = ordered.stage(21000000900);
  const auto expired_scene = builder.build({}, c::Availability::stale);
  check(ordered.statistics().staged == 2 && ordered.statistics().cancelled == 0 &&
            !ordered.current(21000001000) && expired_scene.root,
        "cold-expiry scene prematurely completed an out-of-order camera token");
  c::LocalPresentationClock cold_clock;
  auto receipt_a = feedback();
  receipt_a.correlation->source_ns += 100000000;
  receipt_a.correlation->before_ns += 100000000;
  receipt_a.correlation->after_ns += 100000000;
  receipt_a.received_ns += 100000000;
  check(bool(cold_clock.accept(receipt_a)) && ordered.complete(capture_a->sequence, true),
        "older camera receipt not completed first");
  const auto stale_receipt = cold_clock.accept(feedback(2, 200000000));
  check(stale_receipt && ordered.complete(capture_b->sequence, false),
        "stale-status receipt failed ordered camera cancellation");
  check(ordered.statistics().presented == 1 && ordered.statistics().cancelled == 1 &&
            ordered.current(21000001000) == capture_a,
        "stale-status presentation falsely advanced camera capture");
  c::LocalPresentationClock clock;
  const auto first = clock.accept(feedback());
  check(first && first->monotonic_ns == 21000001000 && first->uncertainty_ns == 21000,
        "actual presentation time mapped incorrectly to local clock");
  auto discarded = feedback(2, 10000000);
  discarded.presented = false;
  discarded.correlation.reset();
  check(!clock.accept(discarded), "discarded feedback fabricated camera presentation");
  const auto later = clock.accept(feedback(3, 20000000));
  check(later && later->monotonic_ns > first->monotonic_ns,
        "local receipt timeline did not advance");
  rejects([&] { clock.accept(feedback(3, 20000000)); });
  check(clock.failed(), "invalid receipt did not fault its mapping generation");
  const auto reject_receipt = [](auto mutate) {
    c::LocalPresentationClock tested;
    auto f = feedback();
    mutate(f);
    rejects([&] { tested.accept(f); });
    check(tested.failed(), "malformed receipt not terminal");
  };
  reject_receipt([](auto& f) { f.correlation.reset(); });
  reject_receipt([](auto& f) { f.nanoseconds = 1000000000; });
  reject_receipt([](auto& f) { f.flags = 16; });
  reject_receipt([](auto& f) { f.seconds = std::numeric_limits<std::uint64_t>::max(); });
  reject_receipt([](auto& f) { f.correlation->clock_id = 1; });
  reject_receipt([](auto& f) { f.correlation->after_ns = f.correlation->before_ns + 2000001; });
  reject_receipt([](auto& f) { f.correlation->source_ns = 10099999999; });
  reject_receipt([](auto& f) { f.received_ns += 2000000000; });
  c::LocalPresentationClock jumping;
  jumping.accept(feedback());
  auto jump = feedback(2, 10000000);
  jump.correlation->source_ns += 1000000;
  rejects([&] { jumping.accept(jump); });
  c::LocalPresentationClock idle;
  idle.accept(feedback());
  auto after_idle = feedback(2, 2000000000);
  after_idle.seconds += 2;
  after_idle.nanoseconds -= 2000000000;
  check(bool(idle.accept(after_idle)),
        "genuine fresh feedback after a static idle scene was rejected");
  if (argc == 2)
    g::write_png(g::render(std::const_pointer_cast<g::Layer>(live.root), 400, 240), argv[1]);
  std::cout << "Camera fit/rotation/mirror pixels, immutable simulation/status tiles, native "
               "texture staging and genuine local presentation clock checks passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
