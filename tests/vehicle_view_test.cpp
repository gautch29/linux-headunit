// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_view.hpp>
#include <headunit/theme/scene.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace c = headunit::camera;
namespace v = headunit::theme::vehicle;
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
  check(rejected, "invalid vehicle observation accepted");
}
std::uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
template <class F>
void until(F f) {
  const auto end = now_ns() + 2000000000ULL;
  while (!f()) {
    check(now_ns() < end, "vehicle view worker timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
int main(int argc, char** argv) try {
  v::ViewObservation o{1, 1, now_ns(), v::ViewProvenance::simulated,
                       v::LaneView{-1.75, 1.75, false}};
  const auto lanes = v::render_vehicle_view(o);
  check(lanes.width == 600 && lanes.height == 480, "signed ADAS size changed");
  o.content = v::LaneView{};
  const auto unknown = v::render_vehicle_view(o);
  check(unknown.pixels != lanes.pixels, "unknown lanes depicted as detected");
  o.provenance = v::ViewProvenance::physical;
  check(v::render_vehicle_view(o).pixels != unknown.pixels, "simulation not labelled in pixels");
  o.provenance = v::ViewProvenance::simulated;
  o.content = v::CabinView{};
  const auto unavailable = v::render_vehicle_view(o);
  auto known_closed = v::CabinView{};
  known_closed.doors_open.fill(false);
  o.content = known_closed;
  check(v::render_vehicle_view(o).pixels != unavailable.pixels, "unknown doors depicted as closed");
  auto cabin = v::CabinView{};
  cabin.seats[0] = {true, false};
  cabin.seats[1] = {true, true};
  cabin.doors_open[0] = true;
  o.content = cabin;
  const auto seats = v::render_vehicle_view(o);
  check(seats.pixels != unavailable.pixels, "unknown occupancy depicted as safe");
  if (argc == 2) {
    headunit::theme::graphics::write_png(lanes, std::string(argv[1]) + "-lanes.png");
    headunit::theme::graphics::write_png(seats, std::string(argv[1]) + "-seats.png");
  }
  auto invalid = o;
  invalid.generation = 0;
  rejects([&] { v::render_vehicle_view(invalid); });
  invalid = o;
  invalid.content = v::LaneView{2, -2, false};
  rejects([&] { v::render_vehicle_view(invalid); });
  c::Configuration cfg;
  cfg.enabled = true;
  cfg.source = c::SourceKind::external;
  cfg.width = cfg.canvas_width = 600;
  cfg.height = cfg.canvas_height = 480;
  cfg.destination = {0, 0, 600, 480};
  c::Queue queue(cfg);
  v::VehicleViewProvider provider(queue, true);
  o.acquired_ns = now_ns() - 1000000;
  const auto acquired = o.acquired_ns;
  check(provider.submit(o, now_ns()), "dated cabin observation rejected");
  until([&] { return provider.statistics().published == 1; });
  auto frame = queue.stage(now_ns());
  check(frame && frame->simulated && frame->capture_monotonic_ns == acquired && frame->image &&
            !frame->native,
        "render worker lost original date or simulation provenance");
  queue.complete(frame->sequence, true);
  check(!queue.current(acquired + 150000001), "render renewed stale acquisition");
  rejects([&] { provider.submit(o, now_ns()); });
  invalid = o;
  invalid.sequence = 2;
  invalid.acquired_ns = now_ns() + 1000000000;
  rejects([&] { provider.submit(invalid, now_ns()); });
  invalid = o;
  invalid.sequence = 2;
  invalid.acquired_ns = now_ns();
  invalid.generation = 2;
  rejects([&] { provider.submit(invalid, now_ns()); });
  invalid = o;
  invalid.content = v::LaneView{};
  rejects([&] { provider.submit(invalid, now_ns()); });
  // A new publisher cannot smuggle an old acquisition in by increasing its sequence.
  c::Queue stale_queue(cfg);
  v::VehicleViewProvider stale_provider(stale_queue, true);
  invalid = o;
  invalid.acquired_ns = now_ns() - 200000000;
  check(!stale_provider.submit(invalid, now_ns()) && !stale_queue.stage(now_ns()),
        "expired observation reached a visible queue");
  provider.request_stop();
  check(queue.availability(now_ns()) == c::Availability::closed && !queue.current(now_ns()),
        "retired vehicle view retained eligible pixels");
  check(frame->image && frame->image->width == 600, "retirement invalidated GPU-held backing");
  until([&] { return provider.stopped(); });
  o.sequence = 2;
  o.acquired_ns = now_ns();
  check(!provider.submit(o, now_ns()), "retired observation owner resumed");
  check(!provider.failure(), "vehicle view owner unexpectedly failed");
  std::cout << "vehicle views: original artwork, unknown states, provenance, acquisition age and "
               "retirement passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
