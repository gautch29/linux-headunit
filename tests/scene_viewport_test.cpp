// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/scene_viewport.hpp>
#include <iostream>
using namespace headunit::theme::graphics;
void check(bool ok) {
  if (!ok) throw std::runtime_error("scene viewport test failed");
}
int main() try {
  auto root = std::make_shared<Layer>();
  root->set_frame({{0, 0}, {1920, 720}});
  // Synthetic phone picture with a square and a right-edge marker. The asset
  // places its 1280x720 texture across 1920x720, just as the reference main pack.
  auto image = std::make_shared<Image>();
  image->width = 1280;
  image->height = 720;
  image->pixels.assign(1280 * 720, 0xff000000);
  for (unsigned y = 200; y < 300; ++y)
    for (unsigned x = 200; x < 300; ++x)
      image->pixels[y * 1280 + x] = 0xff00ff00;
  for (unsigned y = 0; y < 720; ++y)
    for (unsigned x = 1260; x < 1280; ++x)
      image->pixels[y * 1280 + x] = 0xffff0000;
  auto picture = std::make_shared<Layer>(Kind::image);
  picture->image = image;
  picture->set_frame({{0, 0}, {1920, 720}});
  root->insert(picture, 0);
  auto mapped = map_scene_canvas(root, {1920, 720}, {1280, 720});
  // render takes a mutable pointer but does not mutate the scene.
  const auto actual = render(std::const_pointer_cast<Layer>(mapped), 1280, 720);
  check(actual.pixels == image->pixels);  // No cropping, stretching or bars.
  check(root->bounds.size.width == 1920 && root->transform.a == 1 && root->position.x == 960);
  check(picture->parent.lock() == root && mapped->children[0] == picture);
  check(map_scene_canvas(root, {1920, 720}, {1920, 720}) == root);
  auto bad = false;
  try {
    map_scene_canvas(root, {0, 720}, {1280, 720});
  } catch (const std::invalid_argument&) {
    bad = true;
  }
  check(bad);
  // Scale after the complete original transform, including nonzero position,
  // origin and anchor. Preserve the original hierarchy's world coordinates.
  root->bounds.origin = {12, 7};
  root->position = {200, 100};
  root->transform = {1, .2, .1, 1, 8, 5};
  mapped = map_scene_canvas(root, {1920, 720}, {1280, 720});
  const auto before = root->local_transform().apply({70, 30});
  const auto after = mapped->local_transform().apply({70, 30});
  check(std::abs(after.x - before.x * 2 / 3) < 1e-9 && std::abs(after.y - before.y) < 1e-9);
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
