// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_view.hpp>
#include <cairo.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace headunit::theme::vehicle {
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
std::uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void validate(const ViewObservation& o) {
  check(o.generation && o.sequence && o.acquired_ns,
        "vehicle view requires original acquisition identity");
  check(o.provenance == ViewProvenance::physical || o.provenance == ViewProvenance::simulated,
        "unknown vehicle view provenance");
  if (const auto* lane = std::get_if<LaneView>(&o.content)) {
    for (const auto& boundary : {lane->left_boundary_m, lane->right_boundary_m})
      check(!boundary || (std::isfinite(*boundary) && std::abs(*boundary) <= 8),
            "lane boundary exceeds bounded metres");
    check(!lane->left_boundary_m || !lane->right_boundary_m ||
              *lane->left_boundary_m < *lane->right_boundary_m,
          "lane boundaries are reversed");
  }
}
void text(cairo_t* c, double x, double y, const char* value, double size) {
  cairo_set_font_size(c, size);
  cairo_move_to(c, x, y);
  cairo_show_text(c, value);
}
void rectangle(cairo_t* c, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(c);
  cairo_arc(c, x + w - r, y + r, r, -1.5707963268, 0);
  cairo_arc(c, x + w - r, y + h - r, r, 0, 1.5707963268);
  cairo_arc(c, x + r, y + h - r, r, 1.5707963268, 3.1415926536);
  cairo_arc(c, x + r, y + r, r, 3.1415926536, 4.7123889804);
  cairo_close_path(c);
}
void car(cairo_t* c, double x, double y, double w, double h) {
  // Neutral original vehicle silhouette with wheels and a lit body contour.
  cairo_set_source_rgb(c, .015, .025, .03);
  for (const double side : {x - w * .06, x + w * .94})
    for (const double axle : {y + h * .21, y + h * .69}) {
      rectangle(c, side, axle, w * .12, h * .16, w * .03);
      cairo_fill(c);
    }
  auto* paint = cairo_pattern_create_linear(x, y, x + w, y);
  cairo_pattern_add_color_stop_rgb(paint, 0, .24, .34, .40);
  cairo_pattern_add_color_stop_rgb(paint, .25, .66, .77, .81);
  cairo_pattern_add_color_stop_rgb(paint, .55, .43, .56, .62);
  cairo_pattern_add_color_stop_rgb(paint, 1, .16, .27, .32);
  cairo_set_source(c, paint);
  rectangle(c, x, y, w, h, w * .22);
  cairo_fill(c);
  cairo_pattern_destroy(paint);
  cairo_set_source_rgb(c, .03, .09, .12);
  rectangle(c, x + w * .13, y + h * .18, w * .74, h * .25, w * .07);
  cairo_fill(c);
  rectangle(c, x + w * .16, y + h * .72, w * .68, h * .13, w * .07);
  cairo_fill(c);
  cairo_set_source_rgb(c, .8, .95, 1);
  cairo_set_line_width(c, 2);
  rectangle(c, x, y, w, h, w * .22);
  cairo_stroke(c);
}
void lane_scene(cairo_t* c, const LaneView& lane) {
  cairo_set_source_rgb(c, .025, .065, .085);
  cairo_move_to(c, 270, 100);
  cairo_line_to(c, 330, 100);
  cairo_line_to(c, 555, 450);
  cairo_line_to(c, 45, 450);
  cairo_close_path(c);
  cairo_fill(c);
  const std::array<std::optional<double>, 2> boundaries{lane.left_boundary_m,
                                                        lane.right_boundary_m};
  for (unsigned side = 0; side < 2; ++side) {
    const auto value = boundaries[side];
    const double offset = value ? *value : (side ? 1.75 : -1.75);
    cairo_set_source_rgb(c, value ? .10 : .25, value ? .78 : .29, value ? .94 : .32);
    cairo_set_line_width(c, value ? 4 : 2);
    const double dash = 9;
    cairo_set_dash(c, value ? nullptr : &dash, value ? 0 : 1, 0);
    cairo_move_to(c, 300 + offset * 6, 100);
    cairo_line_to(c, 300 + offset * 95, 450);
    cairo_stroke(c);
  }
  cairo_set_dash(c, nullptr, 0, 0);
  car(c, 261, 291, 78, 131);
  cairo_set_source_rgb(c, .78, .85, .88);
  text(c, 28, 72, "Lane view", 22);
  text(c, 28, 465,
       !lane.left_boundary_m && !lane.right_boundary_m ? "Lane data unavailable"
       : !lane.assistance_engaged                      ? "Assistance unavailable"
       : *lane.assistance_engaged                      ? "Assistance active"
                                                       : "Assistance off",
       16);
}
void cabin_scene(cairo_t* c, const CabinView& cabin) {
  car(c, 219, 79, 162, 330);
  constexpr std::array<graphics::Point, 5> positions{
      {{255, 225}, {320, 225}, {246, 296}, {285, 296}, {324, 296}}};
  for (unsigned i = 0; i < positions.size(); ++i) {
    const auto& seat = cabin.seats[i];
    const bool warning = seat.occupied == true && seat.belt_fastened == false;
    const bool unknown = !seat.occupied || (seat.occupied == true && !seat.belt_fastened);
    cairo_set_source_rgb(c,
                         warning   ? 1
                         : unknown ? .28
                                   : .12,
                         warning   ? .37
                         : unknown ? .32
                                   : .64,
                         warning   ? .05
                         : unknown ? .36
                                   : .52);
    rectangle(c, positions[i].x, positions[i].y, i < 2 ? 25 : 22, 33, 5);
    cairo_fill(c);
    if (unknown) {
      cairo_set_source_rgb(c, .95, .95, .95);
      text(c, positions[i].x + 6, positions[i].y + 23, "?", 18);
    }
  }
  for (unsigned i = 0; i < 4; ++i) {
    const auto open = cabin.doors_open[i];
    cairo_set_source_rgb(c, open == true ? 1 : .28, open == true ? .37 : .32,
                         open == true ? .05 : .36);
    cairo_set_line_width(c, 7);
    const double x = i % 2 ? 387 : 213, y = i < 2 ? 205 : 295;
    cairo_move_to(c, x, y);
    cairo_line_to(c, x + (open == true ? (i % 2 ? 35 : -35) : 0), y + 52);
    cairo_stroke(c);
    if (!open) {
      cairo_set_source_rgb(c, .78, .85, .88);
      text(c, x + (i % 2 ? 12 : -24), y + 32, "?", 18);
    }
  }
  cairo_set_source_rgb(c, .78, .85, .88);
  text(c, 28, 54, "Seats and doors", 22);
  text(c, 28, 456, "? = unavailable", 16);
}
}  // namespace
graphics::Image render_vehicle_view(const ViewObservation& o) {
  validate(o);
  graphics::Image result{600, 480, std::vector<std::uint32_t>(600 * 480)};
  auto* surface =
      cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(result.pixels.data()),
                                          CAIRO_FORMAT_ARGB32, 600, 480, 600 * 4);
  auto* c = cairo_create(surface);
  cairo_set_source_rgb(c, .015, .03, .04);
  cairo_paint(c);
  cairo_select_font_face(c, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  if (const auto* lane = std::get_if<LaneView>(&o.content))
    lane_scene(c, *lane);
  else
    cabin_scene(c, std::get<CabinView>(o.content));
  if (o.provenance == ViewProvenance::simulated) {
    cairo_set_source_rgb(c, .95, .63, .12);
    text(c, 342, 30, "BENCH - SIMULATED", 19);
  }
  const auto status = cairo_status(c), surface_status = cairo_surface_status(surface);
  cairo_surface_flush(surface);
  cairo_destroy(c);
  cairo_surface_destroy(surface);
  if (status != CAIRO_STATUS_SUCCESS || surface_status != CAIRO_STATUS_SUCCESS)
    throw std::runtime_error("vehicle view rendering failed");
  return result;
}
struct VehicleViewProvider::Impl {
  camera::Queue& queue;
  bool cabin;
  std::uint64_t maximum_age;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::optional<ViewObservation> pending;
  std::optional<ViewObservation> last;  // Bounded fixed-size observation; no pixels.
  ViewStatistics stats;
  std::optional<std::string> error;
  std::atomic<bool> stopping{false}, finished{false};
  std::thread worker;
  Impl(camera::Queue& q, bool cb, unsigned age)
      : queue(q), cabin(cb), maximum_age(std::uint64_t(age) * 1000000) {
    check(age >= 30 && age <= 1000, "vehicle view freshness exceeds bounds");
    worker = std::thread([this] { run(); });
  }
  void stop() noexcept {
    {
      std::lock_guard lock(mutex);
      stopping = true;
      pending.reset();
      queue.close();  // Withdraw now; outstanding GPU leases keep immutable backing.
    }
    wake.notify_all();
  }
  ~Impl() {
    stop();
    if (worker.joinable()) worker.join();
  }
  bool submit(ViewObservation o, std::uint64_t at) {
    validate(o);
    check(std::holds_alternative<CabinView>(o.content) == cabin,
          "vehicle view changed content kind");
    check(o.acquired_ns <= at, "vehicle view acquisition is in the future");
    std::lock_guard lock(mutex);
    if (stopping || error) return false;
    check(!last || (o.generation == last->generation && o.sequence > last->sequence &&
                    o.acquired_ns > last->acquired_ns && o.provenance == last->provenance),
          "vehicle view changed generation/provenance or repeated acquisition");
    if (at - o.acquired_ns > maximum_age) {
      ++stats.stale;
      return false;
    }
    if (pending) ++stats.superseded;
    last = o;
    pending = std::move(o);
    ++stats.accepted;
    wake.notify_one();
    return true;
  }
  void run() noexcept {
    try {
      for (;;) {
        ViewObservation o;
        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [this] { return stopping || pending.has_value(); });
          if (stopping) break;
          o = std::move(*pending);
          pending.reset();
        }
        auto image = render_vehicle_view(o);
        const auto at = now_ns();
        std::lock_guard lock(mutex);
        ++stats.rendered;
        if (stopping) break;
        if (o.acquired_ns > at || at - o.acquired_ns > maximum_age) {
          ++stats.stale;
          continue;
        }
        if (pending) {
          ++stats.superseded;
          continue;
        }
        if (queue.offer_image(image, o.acquired_ns, at, o.provenance == ViewProvenance::simulated))
          ++stats.published;
        else
          ++stats.backpressure;
      }
    } catch (const std::exception& e) {
      queue.close();
      std::lock_guard lock(mutex);
      error = std::string(e.what()).substr(0, 256);
    } catch (...) {
      queue.close();
      std::lock_guard lock(mutex);
      error = "vehicle view worker failed";
    }
    finished = true;
  }
};
VehicleViewProvider::VehicleViewProvider(camera::Queue& q, bool cabin, unsigned age)
    : impl_(std::make_unique<Impl>(q, cabin, age)) {}
VehicleViewProvider::~VehicleViewProvider() = default;
bool VehicleViewProvider::submit(ViewObservation o, std::uint64_t at) {
  return impl_->submit(std::move(o), at);
}
void VehicleViewProvider::request_stop() noexcept {
  impl_->stop();
}
bool VehicleViewProvider::stopped() const noexcept {
  return impl_->finished;
}
ViewStatistics VehicleViewProvider::statistics() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats;
}
std::optional<std::string> VehicleViewProvider::failure() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->error;
}
}  // namespace headunit::theme::vehicle
