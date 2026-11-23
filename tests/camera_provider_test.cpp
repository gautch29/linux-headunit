// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera.hpp>
#include <atomic>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

namespace c = headunit::camera;
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
  check(rejected, "invalid camera input accepted");
}
c::Configuration configuration() {
  c::Configuration result;
  result.enabled = true;
  result.manual_activation = true;
  result.width = 128;
  result.height = 96;
  result.canvas_width = 128;
  result.canvas_height = 96;
  result.destination = {0, 0, 128, 96};
  return result;
}
g::Image image(unsigned width = 128, unsigned height = 96, std::uint32_t colour = 0xffee1100) {
  return {width, height, std::vector<std::uint32_t>(std::size_t(width) * height, colour)};
}
bool offer(c::Queue& q, std::uint64_t timestamp) {
  auto value = image();
  return q.offer_image(value, timestamp, timestamp, true);
}
void discarded_capture_freshness() {
  c::Queue queue(configuration());
  constexpr std::uint64_t first = 1000000000, next = first + 40000000;
  check(offer(queue, first), "initial freshness capture rejected");
  const auto current = queue.stage(first);
  check(current && queue.complete(current->sequence, true), "initial receipt rejected");
  check(offer(queue, next), "new freshness capture rejected");
  const auto rejected = queue.stage(next);
  check(rejected && queue.complete(rejected->sequence, false), "discard receipt rejected");
  // The rejected frame may remain leased by the GPU, but cannot date the
  // older current image or qualify the source once that current image expires.
  check(queue.available_capture_ns(next) == first,
        "discarded capture dated an older retained camera source as fresh");
  const auto expired = first + 150000001;
  check(!queue.current(expired) && queue.availability(expired) == c::Availability::stale &&
            !queue.available_capture_ns(expired),
        "discarded capture renewed an expired camera source");
  check(offer(queue, expired), "fresh queued recovery capture rejected");
  check(queue.available_capture_ns(expired) == expired,
        "genuinely fresh queued capture failed recovery");
  const auto staged = queue.stage(expired);
  check(staged && queue.available_capture_ns(expired) == expired,
        "staged fresh capture lost its original date");
  queue.discard();
  check(queue.availability(expired) == c::Availability::missing &&
            !queue.available_capture_ns(expired) && !queue.complete(staged->sequence, true),
        "externally retained capture survived queue withdrawal");
}
int main() try {
  discarded_capture_freshness();
  std::ifstream file(std::string(HEADUNIT_SOURCE_DIR) + "/config/camera.example.json");
  std::stringstream text;
  text << file.rdbuf();
  const auto example = c::decode_configuration(text.str());
  check(!example.enabled && example.stream_identifier == "rear-camera",
        "example activates a camera or loses its identity");
  std::ifstream reference_file(std::string(HEADUNIT_SOURCE_DIR) +
                               "/config/camera-ultra-reference.example.json");
  std::stringstream reference_text;
  reference_text << reference_file.rdbuf();
  const auto reference = c::decode_configuration(reference_text.str());
  check(!reference.enabled && reference.stream_identifier == "camera" && reference.width == 1650 &&
            reference.height == 720 && reference.destination.x == 135 &&
            reference.destination.width == 1650 && reference.canvas_width == 1920,
        "reference camera template changed authored geometry or activates a stream");
  rejects([&] { c::decode_configuration(text.str() + "{}"); });
  rejects([&] { c::decode_configuration("{\"schemaVersion\":1,\"schemaVersion\":1}"); });
  rejects([&] {
    auto bad = text.str();
    const auto p = bad.find("\"schemaVersion\": 1");
    bad.replace(p, 18, "\"schemaVersion\": 1.0");
    c::decode_configuration(bad);
  });
  rejects([&] { c::decode_configuration(std::string(8193, ' ')); });
  const auto config = configuration();
  rejects([&] {
    auto bad = config;
    bad.width = 4096;
    c::validate_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.destination.width = 129;
    c::validate_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.destination.x = std::numeric_limits<unsigned>::max();
    c::validate_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.rotation_degrees = 45;
    c::validate_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.stream_identifier = "rear camera";
    c::validate_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.frame_rate = 1;
    bad.maximum_age_ms = 150;
    c::validate_configuration(bad);
  });
  rejects([&] {
    auto bad = config;
    bad.source = static_cast<c::SourceKind>(5);
    c::validate_configuration(bad);
  });
  c::Queue q(config);
  check(q.availability(0) == c::Availability::missing && !q.current(0),
        "missing source displayed a frame");
  auto value = image();
  rejects([&] { q.offer_image(value, 2, 1, true); });
  check(!value.pixels.empty(), "invalid future input consumed image");
  auto wrong = image(129);
  rejects([&] { q.offer_image(wrong, 1, 1, true); });
  check(q.offer_image(value, 100, 100, true) && value.pixels.empty(),
        "valid source image not transferred");
  rejects([&] { offer(q, 100); });
  rejects([&] { offer(q, 99); });
  check(q.availability(99) == c::Availability::clock_reversed && !q.stage(99),
        "reversed presentation clock accepted a future frame");
  auto staged = q.stage(100);
  check(staged && staged->sequence == 1 && !q.current(100),
        "staging falsely advanced current presentation");
  check(q.complete(1, false) && !q.current(100), "cancelled camera submission became current");
  check(!q.complete(1, true), "cancelled receipt later advanced current");
  staged.reset();
  check(offer(q, 200), "second camera capture failed");
  staged = q.stage(200);
  check(q.complete(staged->sequence, true) && q.current(200) == staged,
        "actual presentation failed to advance current");
  check(offer(q, 250) && !q.current(225) && q.availability(225) == c::Availability::clock_reversed,
        "reversed local clock retained an older current camera texture");
  // Withdraw the newer queued capture so the boundary checks below refer to
  // the current capture's own age, while retaining its immutable external lease.
  q.discard();
  staged.reset();
  check(offer(q, 300), "fresh camera capture after withdrawal failed");
  staged = q.stage(300);
  q.complete(staged->sequence, true);
  check(q.current(150000300) != nullptr && !q.current(150000301),
        "stale camera frame remained visible");
  check(q.availability(150000301) == c::Availability::stale, "stale camera source reported ready");
  check(!q.current(299), "future current camera frame remained visible");
  auto old = staged;
  q.discard();
  check(q.availability(300) == c::Availability::missing && !q.current(300),
        "discard kept displayed camera frame");
  check(old->image->pixels.front() == 0xffee1100 && q.statistics().outstanding_leases == 1,
        "discard destroyed an in-flight immutable frame");
  old.reset();
  staged.reset();
  check(offer(q, 350) && offer(q, 400), "latest capture replacement failed");
  staged = q.stage(400);
  check(staged->capture_monotonic_ns == 400 && q.statistics().superseded == 1,
        "old queued capture was staged");
  q.discard();
  check(!q.complete(staged->sequence, true), "old receipt after view exit changed state");
  const auto old_sequence = staged->sequence;
  staged.reset();
  check(offer(q, 500), "capture ordering frontier not retained");
  staged = q.stage(500);
  check(staged->sequence > old_sequence, "enter/exit recycled camera submission token");
  check(offer(q, 600), "second pending capture failed");
  auto second = q.stage(600);
  rejects([&] { q.complete(second->sequence, true); });
  rejects([&] { q.complete(9999, true); });
  check(offer(q, 700) && !q.stage(700), "more than two pending camera submissions staged");
  q.discard();
  check(!q.complete(staged->sequence, true), "late old camera receipt after discard was applied");
  staged.reset();
  second.reset();

  // A display may retain old frames through GPU fences. The producer must
  // backpressure even after all queue/current references have been discarded.
  std::vector<std::shared_ptr<const c::Frame>> gpu;
  for (unsigned i = 0; i < c::Queue::maximum_leases; ++i) {
    check(offer(q, 1000 + i), "lease pool filled early");
    auto f = q.stage(1000 + i);
    check(bool(f), "lease frame not staged");
    q.complete(f->sequence, true);
    gpu.push_back(std::move(f));
    q.discard();
  }
  auto blocked = image();
  check(!q.offer_image(blocked, 2000, 2000, true) && !blocked.pixels.empty(),
        "retained GPU leases allowed unbounded capture or consumed rejected image");
  check(q.statistics().outstanding_leases == 4 && q.statistics().maximum_outstanding_leases == 4,
        "camera lease bound not tracked");
  gpu.pop_back();
  check(q.offer_image(blocked, 2000, 2000, true), "released GPU lease did not unblock capture");
  gpu.clear();
  q.discard();
  check(q.statistics().outstanding_leases == 0, "camera frame lease leaked");
  check(offer(q, 3000) && !q.stage(150003001) && q.statistics().stale_dropped == 1,
        "stale queued camera frame composed");
  q.close();
  check(q.availability(4000) == c::Availability::closed && !q.can_offer(),
        "closed camera source accepts work");
  rejects([&] { offer(q, 4000); });

  auto external_config = config;
  external_config.source = c::SourceKind::external;
  c::Queue native(external_config);
  auto raw = std::make_shared<headunit::theme::video::NativeFrame>();
  raw->metadata = {0, 9876, 128, 96};
  raw->storage_width = 128;
  raw->storage_height = 96;
  raw->plane_count = 1;
  raw->planes[0] = {17, 0, 512};
  raw->lifetime = std::make_shared<int>(42);
  check(native.offer_native(raw, 500, 500), "external native frame not accepted");
  auto nf = native.stage(500);
  check(nf->native == raw && nf->capture_monotonic_ns == 500 && raw->metadata.sender_ntp == 0 &&
            raw->metadata.timestamp_ns == 9876,
        "local camera timestamp was written into phone-clock metadata");
  rejects([&] {
    auto cpu = image();
    native.offer_image(cpu, 600, 600, false);
  });
  auto invalid = std::make_shared<headunit::theme::video::NativeFrame>(*raw);
  invalid->lifetime.reset();
  rejects([&] { native.offer_native(invalid, 600, 600); });
  native.discard();
  check(nf->native->lifetime != nullptr, "discard released native GPU backing prematurely");
  nf.reset();

  c::SyntheticProvider provider(config);
  c::Queue synthetic(config);
  check(provider.poll(0, synthetic) && !provider.poll(1, synthetic), "synthetic cadence burst");
  auto test_frame = synthetic.stage(0);
  check(test_frame->simulated && test_frame->image->pixels.front() == 0xffdddddd,
        "test pattern not identified or incorrectly rendered");
  const auto original_marker = test_frame->image->pixels.back();
  synthetic.complete(test_frame->sequence, true);
  test_frame.reset();
  check(provider.poll(1000000000, synthetic) && !provider.poll(1000000001, synthetic),
        "delayed synthetic turn replayed a backlog");
  test_frame = synthetic.stage(1000000000);
  check(test_frame->capture_monotonic_ns == 1000000000 && test_frame->sequence == 2,
        "synthetic capture timestamp fabricated old frame time");
  check(test_frame->image->pixels.back() != original_marker, "synthetic marker did not animate");
  check(!synthetic.current(1000000000), "old synthetic camera frame remained visible after expiry");
  rejects([&] { provider.poll(999999999, synthetic); });
  auto disabled = config;
  disabled.enabled = false;
  c::Session off(disabled);
  check(!off.enter("carplay/ultra") && !off.turn(0).active, "disabled camera session activated");
  c::Session session(config);
  check(session.enter("carplay/ultra") && !session.enter("native/home"),
        "reentry replaced the saved main view");
  auto state = session.turn(10);
  check(state.active && state.previous_view == "carplay/ultra" &&
            state.availability == c::Availability::ready,
        "manual camera session not ready");
  auto active = session.frames().stage(10);
  check(!session.frames().current(10), "session poll falsely presented a camera image");
  check(session.leave() == "carplay/ultra" && !session.leave() && !session.turn(20).active,
        "camera exit did not restore prior view");
  check(!session.frames().complete(active->sequence, true),
        "delayed receipt after session exit became current");
  rejects([&] { session.enter("camera"); });
  rejects([&] { session.enter("native\nview"); });
  c::Session missing(external_config);
  check(missing.enter("native/home") && missing.turn(0).availability == c::Availability::missing,
        "phone-free external camera missing source silently reported ready");
  missing.leave();
  check(missing.enter("native/home") && missing.turn(1).availability == c::Availability::missing,
        "old source state leaked across manual entries");

  // Real capture callbacks and a display receipt loop share only the bounded
  // mailbox. They never touch VM, GPU state, or mutable submitted images.
  c::Queue concurrent(config);
  std::atomic<bool> finished{false};
  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= 1000; ++i) {
      auto v = image();
      while (!concurrent.offer_image(v, i, i, true))
        std::this_thread::yield();
    }
    finished = true;
  });
  while (!finished.load()) {
    if (auto f = concurrent.stage(1000))
      concurrent.complete(f->sequence, true);
    else
      std::this_thread::yield();
  }
  producer.join();
  if (auto f = concurrent.stage(1000)) concurrent.complete(f->sequence, true);
  check(concurrent.statistics().accepted == 1000 &&
            concurrent.statistics().maximum_outstanding_leases <= 4,
        "concurrent camera producer broke bounds");
  concurrent.discard();
  check(concurrent.statistics().outstanding_leases == 0, "concurrent camera leases leaked");
  std::cout << "Camera freshness, monotonic capture, bounded GPU leases, receipt ordering, "
               "synthetic cadence, view restoration and strict configuration passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
