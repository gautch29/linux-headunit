// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_display.hpp>
#include <headunit/camera_local_controls.hpp>
#include <cairo.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace headunit::camera {
namespace g = theme::graphics;
namespace v = theme::video;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
std::shared_ptr<g::Image> label(std::string_view message, g::Color background, unsigned width = 512,
                                unsigned height = 80) {
  auto image = std::make_shared<g::Image>(
      g::Image{width, height, std::vector<std::uint32_t>(std::size_t(width) * height)});
  auto* surface = cairo_image_surface_create_for_data(
      reinterpret_cast<unsigned char*>(image->pixels.data()), CAIRO_FORMAT_ARGB32, int(width),
      int(height), int(width * 4));
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    throw std::runtime_error("camera label surface failed");
  }
  auto* canvas = cairo_create(surface);
  cairo_set_source_rgba(canvas, background.r, background.g, background.b, background.a);
  cairo_paint(canvas);
  cairo_select_font_face(canvas, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(canvas, 30);
  cairo_text_extents_t extents{};
  const std::string text(message);
  cairo_text_extents(canvas, text.c_str(), &extents);
  cairo_set_source_rgb(canvas, 1, 1, 1);
  cairo_move_to(canvas, (width - extents.width) / 2 - extents.x_bearing,
                (height - extents.height) / 2 - extents.y_bearing);
  cairo_show_text(canvas, text.c_str());
  const auto status = cairo_status(canvas);
  cairo_surface_flush(surface);
  cairo_destroy(canvas);
  cairo_surface_destroy(surface);
  if (status != CAIRO_STATUS_SUCCESS) throw std::runtime_error("camera label rendering failed");
  return image;
}
constexpr std::uint64_t second = 1000000000, maximum_age = second, maximum_span = 2000000;
std::uint64_t midpoint(const v::ClockCorrelation& c) {
  return c.before_ns + (c.after_ns - c.before_ns) / 2;
}
std::uint64_t halfspan(const v::ClockCorrelation& c) {
  return (c.after_ns - c.before_ns + 1) / 2;
}
std::uint64_t distance(std::uint64_t a, std::uint64_t b) {
  return a > b ? a - b : b - a;
}
std::uint64_t drift(std::uint64_t duration) {
  return (duration * 2000 + 999999) / 1000000;
}
}  // namespace
g::Transform capture_transform(const Configuration& c) {
  validate_configuration(c);
  double width = c.width, height = c.height;
  g::Transform rotate;
  switch (c.rotation_degrees) {
    case 0:
      break;
    case 90:
      rotate = {0, 1, -1, 0, height, 0};
      std::swap(width, height);
      break;
    case 180:
      rotate = {-1, 0, 0, -1, width, height};
      break;
    case 270:
      rotate = {0, -1, 1, 0, 0, width};
      std::swap(width, height);
      break;
  }
  const g::Transform mirror =
      c.mirror_horizontal ? g::Transform{-1, 0, 0, 1, width, 0} : g::Transform{};
  const double fit =
      std::min(double(c.destination.width) / width, double(c.destination.height) / height);
  const g::Transform scale{fit, 0, 0, fit, 0, 0};
  const g::Transform translate{1,
                               0,
                               0,
                               1,
                               c.destination.x + (c.destination.width - width * fit) / 2,
                               c.destination.y + (c.destination.height - height * fit) / 2};
  return translate.multiply(scale).multiply(mirror).multiply(rotate);
}
SceneBuilder::SceneBuilder(Configuration c, bool manual_controls) : configuration_(std::move(c)) {
  validate_configuration(configuration_);
  simulated_ = label("SIMULATED CAMERA", {.45, .20, .015, 1});
  live_ = label("REAR CAMERA", {.03, .08, .11, 1});
  missing_ = label("CAMERA UNAVAILABLE", {.075, .075, .085, 1});
  stale_ = label("CAMERA SIGNAL STALE", {.40, .12, .035, 1});
  clock_reversed_ = label("CAMERA CLOCK INVALID", {.40, .08, .08, 1});
  closed_ = label("CAMERA SOURCE CLOSED", {.075, .075, .085, 1});
  if (manual_controls)
    close_labels_ = {label("Close", {.08, .10, .14, 1}, 320, 96),
                     label("Closing…", {.08, .10, .14, 1}, 320, 96),
                     label("Manual off", {.08, .10, .14, 1}, 320, 96),
                     label("Retry close", {.25, .09, .08, 1}, 320, 96)};
}
CameraScene SceneBuilder::build(const std::shared_ptr<const Frame>& frame,
                                Availability availability,
                                std::optional<ManualCloseState> manual_close) const {
  if (availability == Availability::ready && !frame) availability = Availability::missing;
  const auto& c = configuration_;
  CameraScene scene;
  auto root = std::make_shared<g::Layer>();
  root->set_frame({{0, 0}, {double(c.canvas_width), double(c.canvas_height)}});
  root->background = {.006, .009, .015, 1};
  if (availability == Availability::ready && frame) {
    check(bool(frame->image) != bool(frame->native), "camera scene needs exactly one storage kind");
    auto capture = std::make_shared<g::Layer>(frame->native ? g::Kind::video : g::Kind::image);
    capture->anchor = {0, 0};
    capture->bounds = {{0, 0}, {double(c.width), double(c.height)}};
    capture->transform = capture_transform(c);
    if (frame->image) {
      check(frame->image->width == c.width && frame->image->height == c.height &&
                frame->image->pixels.size() == std::size_t(c.width) * c.height,
            "camera scene image dimensions changed");
      // Layer's historical image API is mutable; this owner only reads the
      // provider's immutable backing and never edits its pixels.
      // Retain the OUTER camera frame permit too. A scene can outlive its
      // caller's separate staged-frame reference, including standalone probes.
      capture->image = std::const_pointer_cast<g::Image>(
          std::shared_ptr<const g::Image>(frame, frame->image.get()));
    } else {
      check(frame->native->metadata.width == c.width && frame->native->metadata.height == c.height,
            "camera scene native dimensions changed");
      scene.native_stream = std::make_shared<v::Stream>(
          v::Input{c.stream_identifier, c.width, c.height, 1. / c.frame_rate, v::InputType::camera},
          std::make_shared<v::Memory>());
      const auto native_lease = std::shared_ptr<const v::NativeFrame>(frame, frame->native.get());
      check(scene.native_stream->offer_native(double(frame->capture_monotonic_ns) / second,
                                              native_lease),
            "camera scene stream bound exceeded");
      const auto prepared = scene.native_stream->prepare_next();
      check(bool(prepared), "camera scene texture not staged");
      scene.native_token = prepared->token;
      capture->video = std::make_shared<v::View>(v::View{scene.native_stream, true});
    }
    root->insert(capture, 0);
  }
  const auto overlay = status_overlay(true, availability, frame && frame->simulated,
                                      {double(c.canvas_width), double(c.canvas_height)});
  if (overlay) root->insert(std::const_pointer_cast<g::Layer>(overlay), root->children.size());
  if (manual_close) {
    const auto index = std::size_t(*manual_close);
    check(index < close_labels_.size() && bool(close_labels_[index]),
          "invalid or disabled native camera close state");
    auto button = std::make_shared<g::Layer>(g::Kind::image);
    button->set_frame(manual_close_frame(c.canvas_width, c.canvas_height));
    button->corner_radius = 24;
    button->clip = true;
    button->image = close_labels_[index];
    root->insert(button, root->children.size());
  }
  scene.root = std::move(root);
  return scene;
}
std::shared_ptr<const g::Layer> SceneBuilder::status_overlay(bool active, Availability availability,
                                                             bool simulated, g::Size output) const {
  if (!active) return {};
  check(std::isfinite(output.width) && std::isfinite(output.height) && output.width > 0 &&
            output.width <= 4096 && output.height > 0 && output.height <= 2160,
        "invalid camera status output dimensions");
  const auto& c = configuration_;
  auto root = std::make_shared<g::Layer>();
  root->anchor = {0, 0};
  root->group_blending = false;
  root->transform = {output.width / c.canvas_width, 0, 0, output.height / c.canvas_height, 0, 0};
  if (availability == Availability::ready) {
    auto badge = std::make_shared<g::Layer>(g::Kind::image);
    const double width = std::min(360., std::max(64., double(c.destination.width) * .42));
    const double height = width * 80 / 512;
    badge->set_frame(
        {{c.destination.x + (c.destination.width - width) / 2, double(c.destination.y) + 12},
         {width, height}});
    badge->image = simulated ? simulated_ : live_;
    root->insert(badge, 0);
  } else {
    auto unavailable = std::make_shared<g::Layer>();
    unavailable->set_frame({{double(c.destination.x), double(c.destination.y)},
                            {double(c.destination.width), double(c.destination.height)}});
    unavailable->background = {.006, .009, .015, 1};
    root->insert(unavailable, 0);
    auto message = std::make_shared<g::Layer>(g::Kind::image);
    const double width = std::min(512., double(c.destination.width) * .9);
    const double height = width * 80 / 512;
    message->set_frame({{c.destination.x + (c.destination.width - width) / 2,
                         c.destination.y + (c.destination.height - height) / 2},
                        {width, height}});
    switch (availability) {
      case Availability::stale:
        message->image = stale_;
        break;
      case Availability::clock_reversed:
        message->image = clock_reversed_;
        break;
      case Availability::closed:
        message->image = closed_;
        break;
      default:
        message->image = missing_;
        break;
    }
    root->insert(message, 1);
  }
  return root;
}
std::optional<LocalPresentation> LocalPresentationClock::accept(const v::PresentationFeedback& f) {
  check(!failed_, "local camera presentation clock faulted");
  try {
    check(f.token > last_token_ && f.received_ns >= last_received_,
          "camera presentation feedback reordered");
    check(!clock_id_ || *clock_id_ == f.clock_id, "camera presentation clock changed");
    clock_id_ = f.clock_id;
    last_token_ = f.token;
    last_received_ = f.received_ns;
    if (!f.presented) return {};
    check(f.nanoseconds < second && !(f.flags & ~15u) &&
              f.seconds <= (std::numeric_limits<std::uint64_t>::max() - f.nanoseconds) / second,
          "invalid camera presentation timestamp");
    check(bool(f.correlation), "camera presentation missing real clock correlation");
    const auto& c = *f.correlation;
    check(c.clock_id == f.clock_id && c.after_ns >= c.before_ns && c.after_ns <= f.received_ns &&
              c.after_ns - c.before_ns <= maximum_span,
          "invalid camera presentation clock sample");
    const auto timestamp = f.seconds * second + f.nanoseconds;
    check(timestamp <= c.source_ns && c.source_ns - timestamp <= maximum_age &&
              f.received_ns - c.before_ns <= maximum_age,
          "stale or future camera presentation feedback");
    const auto mid = midpoint(c), delta = c.source_ns - timestamp;
    check(delta <= mid, "camera presentation precedes clock origin");
    if (previous_) {
      check(c.before_ns >= previous_->after_ns && c.source_ns >= previous_->source_ns,
            "camera clock correlation regressed");
      const auto elapsed = mid - midpoint(*previous_);
      // A static unavailable screen can remain idle indefinitely. Each new
      // feedback still supplies its real bounded sample; continuity between
      // short adjacent samples is checked without inventing idle receipts.
      if (elapsed <= maximum_age)
        check(distance(c.source_ns - previous_->source_ns, elapsed) <=
                  halfspan(c) + halfspan(*previous_) + drift(elapsed),
              "camera clock correlation jumped");
    }
    const auto monotonic = mid - delta, uncertainty = halfspan(c) + drift(delta);
    check(monotonic <= f.received_ns && f.received_ns - monotonic <= maximum_age &&
              uncertainty <= 25000000,
          "camera presentation sample too old or uncertain");
    check(!last_ || monotonic >= last_->monotonic_ns, "camera presentation timeline regressed");
    previous_ = c;
    last_ = LocalPresentation{monotonic, uncertainty};
    return last_;
  } catch (...) {
    failed_ = true;
    throw;
  }
}
}  // namespace headunit::camera
