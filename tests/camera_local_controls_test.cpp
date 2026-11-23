// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera_local_controls.hpp>
#include <headunit/camera_display.hpp>
#include <headunit/preview_touch.hpp>
#include <iostream>
#include <stdexcept>

namespace c = headunit::camera;
namespace g = headunit::theme::graphics;
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
int main() try {
  c::ManualCloseGesture gesture(1920, 720);
  const auto frame = c::manual_close_frame(1920, 720);
  const headunit::TouchInput down{true, std::uint16_t(frame.origin.x + 100),
                                  std::uint16_t(frame.origin.y + 32)};
  check(gesture.consume(down, true), "button did not activate on a new press");
  for (unsigned i = 0; i < 64; ++i)
    check(!gesture.consume(down, true), "held button duplicated its request");
  check(!gesture.consume({false, down.x, down.y}, true), "release became a command");
  check(!gesture.consume({true, 5, 5}, true) && !gesture.consume(down, true),
        "drag into button became a command");
  gesture.consume({false, down.x, down.y}, true);
  check(!gesture.consume(down, false) && !gesture.consume(down, true),
        "enabled held gesture became a command");
  gesture.consume({false, down.x, down.y}, true);
  check(gesture.consume(down, true), "new gesture was lost");
  headunit::PreviewTouch touch(1920, 720);
  touch.resize(1280, 720);
  touch.down(1, (frame.origin.x + 100) * 2 / 3, 120 + (frame.origin.y + 32) * 2 / 3);
  const auto fitted = touch.take();
  check(fitted.size() == 1, "fitted touch missing");
  c::ManualCloseGesture fitted_gesture(1920, 720);
  check(fitted_gesture.consume(fitted.front(), true),
        "letterboxed touchscreen disagrees with camera control");
  touch.up(1);
  touch.take();
  touch.down(2, 1200, 20);
  check(touch.take().empty(), "fit bars create a camera command");
  c::Configuration config;
  config.enabled = true;
  config.manual_activation = true;
  config.width = 128;
  config.height = 96;
  config.canvas_width = 400;
  config.canvas_height = 240;
  config.destination = {80, 56, 240, 160};
  c::SceneBuilder plain(config), controls(config, true);
  const auto plain_scene = plain.build({}, c::Availability::missing);
  auto pixels = g::render(std::const_pointer_cast<g::Layer>(plain_scene.root), 400, 240);
  const auto original = pixels.pixels;
  std::shared_ptr<g::Image> previous;
  for (const auto state : {c::ManualCloseState::idle, c::ManualCloseState::pending,
                           c::ManualCloseState::accepted, c::ManualCloseState::failed}) {
    const auto scene = controls.build({}, c::Availability::missing, state);
    check(scene.root->children.size() == plain_scene.root->children.size() + 1,
          "local control changed source scene");
    const auto button = scene.root->children.back();
    check(button->image && button->clip && button->corner_radius == 24,
          "control lost rounded styling");
    check(button->image != previous, "control state has no separate label");
    previous = button->image;
    check(controls.build({}, c::Availability::missing, state).root->children.back()->image ==
              button->image,
          "unchanged control re-rasterizes text");
    const auto image = g::render(std::const_pointer_cast<g::Layer>(scene.root), 400, 240);
    check(image.pixels[5 * 400 + 5] == original[5 * 400 + 5] &&
              image.pixels[230 * 400 + 200] == original[230 * 400 + 200],
          "control covers unrelated camera pixels");
    check(image.pixels[50 * 400 + 200] != original[50 * 400 + 200], "native control is invisible");
  }
  bool refused = false;
  try {
    plain.build({}, c::Availability::missing, c::ManualCloseState::idle);
  } catch (const std::exception&) {
    refused = true;
  }
  check(refused, "camera-only builder accepts an undeclared control");
  std::cout << "Camera local controls: single-press routing, fitted input and cached scene labels "
               "passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
