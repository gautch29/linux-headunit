// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/brand_greeting.hpp>
#include <headunit/theme/scene_viewport.hpp>
#include <iostream>
#include <stdexcept>

namespace g = headunit::theme::graphics;
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
std::shared_ptr<g::Layer> image(double width, double height, g::Point center) {
  auto layer = std::make_shared<g::Layer>(g::Kind::image);
  layer->set_frame({{center.x - width / 2, center.y - height / 2}, {width, height}});
  layer->image = std::make_shared<g::Image>();
  layer->image->width = std::uint32_t(width);
  layer->image->height = std::uint32_t(height);
  layer->image->pixels.assign(std::size_t(width * height), 0xffffffff);
  return layer;
}
std::shared_ptr<g::Layer> fixture() {
  auto root = std::make_shared<g::Layer>();
  root->set_frame({{0, 0}, {1920, 720}});
  root->background = {0, 0, 0, 1};
  auto group = std::make_shared<g::Layer>();
  group->set_frame({{0, 0}, {1920, 720}});
  auto brand = image(376, 86, {708, 360});
  auto divider = std::make_shared<g::Layer>(g::Kind::shape);
  divider->set_frame({{970, 295}, {0, 130}});
  divider->path = std::make_shared<g::Path>();
  divider->path->append({g::SegmentType::move, {0, 0}});
  divider->path->append({g::SegmentType::line, {0, 130}});
  divider->stroke = {1, 1, 1, 1};
  divider->line_width = 4;
  auto apple = image(494, 86, {1288, 360});
  group->insert(brand, 0);
  group->insert(divider, 1);
  group->insert(apple, 2);
  root->insert(group, 0);
  return root;
}
int main() try {
  g::BrandGreeting greeting;
  auto source = fixture();
  auto group = source->children[0];
  auto brand = group->children[0];
  auto apple = group->children[2];
  auto divider = group->children[1];
  check(greeting.scene(source, false, 100) == source, "branding appeared outside logos");
  const auto first = greeting.scene(source, true, 200);
  check(first != source, "manufacturer image was not replaced");
  auto replaced = first->children[0]->children[0];
  check(replaced->kind == g::Kind::layer && !replaced->image && replaced->children.size() == 1,
        "replacement retained the Aston image");
  check(group->children[0] == brand && brand->kind == g::Kind::image && brand->image,
        "signed source was mutated");
  check(first->children[0]->children[1] == divider && first->children[0]->children[2] == apple &&
            apple->parent.lock() == group && divider->parent.lock() == group,
        "Apple or divider was changed or reparented");
  const auto original_pixels = g::render(source, 1920, 720);
  const auto decorated_pixels = g::render(std::const_pointer_cast<g::Layer>(first), 1920, 720);
  std::size_t changed = 0;
  for (std::size_t y = 0; y < 720; ++y) {
    for (std::size_t x = 0; x < 1920; ++x) {
      const auto index = y * 1920 + x;
      if (original_pixels.pixels[index] == decorated_pixels.pixels[index]) continue;
      ++changed;
      check(x >= 520 && x < 896 && y >= 295 && y < 425,
            "replacement painted outside manufacturer space");
    }
  }
  check(changed > 10000, "replacement geometry is missing");
  // Later signed turns own both the manufacturer's slide/fade and Apple timing.
  brand->opacity = .25;
  brand->position.x += 17;
  group->opacity = .6;
  group->transform.tx = -20;
  apple->opacity = .4;
  const auto later = greeting.scene(source, true, 300);
  auto later_brand = later->children[0]->children[0];
  check(later_brand->opacity == .25 && later_brand->position.x == brand->position.x &&
            later->children[0]->opacity == .6 && later->children[0]->transform.tx == -20 &&
            later->children[0]->children[2] == apple && apple->opacity == .4,
        "signed motion/fade was not inherited");
  check(replaced->opacity == 1 && first->children[0]->opacity == 1 &&
            later_brand->children[0] == replaced->children[0],
        "submitted manufacturer changed or immutable vectors were rebuilt");
  auto resized = g::map_scene_canvas(later, {1920, 720}, {1280, 480});
  check(resized->transform.a == 2. / 3. && resized->transform.d == 2. / 3.,
        "decoration bypassed the authored canvas mapping");
  auto warning = std::make_shared<g::Layer>();
  warning->set_frame({{0, 0}, {10, 10}});
  warning->background = {1, 0, 0, 1};
  auto combined = g::with_scene_overlay(first, warning);
  const auto warning_pixels = g::render(std::const_pointer_cast<g::Layer>(combined), 1920, 720);
  check(warning_pixels.pixels.front() == 0xffff0000, "branding suppressed warnings");
  check(greeting.scene(source, false, 400) == source, "branding covered Drive");
  check(greeting.scene(source, true, 500) == source, "retired branding replayed");
  g::BrandGreeting reversed;
  check(reversed.scene(source, true, 100) != source, "fresh logos missing");
  check(reversed.scene(source, true, 99) == source && reversed.scene(source, true, 101) == source,
        "reversed clock retained or replayed decoration");
  g::BrandGreeting unknown;
  auto foreign = fixture();
  foreign->children[0]->children[2]->bounds.size.width = 493;
  check(unknown.scene(foreign, true, 1) == foreign, "unknown co-brand artwork was replaced");
  auto ambiguous = fixture();
  ambiguous->insert(fixture()->children[0], 1);
  check(unknown.scene(ambiguous, true, 2) == ambiguous, "ambiguous branding was replaced");
  auto deep = fixture();
  for (unsigned i = 0; i < 70; ++i) {
    auto parent = std::make_shared<g::Layer>();
    parent->insert(deep, 0);
    deep = parent;
  }
  check(unknown.scene(deep, true, 3) == deep, "unbounded scene traversal was accepted");
  auto wide = std::make_shared<g::Layer>();
  wide->children.assign(8193, std::make_shared<g::Layer>());
  check(unknown.scene(wide, true, 4) == wide, "node bound was ignored");
  check(unknown.scene({}, true, 5) == nullptr, "missing scene was manufactured");
  std::cout << "Linux Head Unit-only replacement, Apple pixels, signed motion, bounds and "
               "retirement passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
