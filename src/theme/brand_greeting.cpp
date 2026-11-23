// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/brand_greeting.hpp>
#include <headunit/theme/brand_artwork.hpp>

namespace headunit::theme::graphics {
namespace {
// Exact first-party polygon geometry from assets/branding/headunit/logo.svg.
// No signed asset lookup is overridden; no runtime file/parser is introduced.
std::shared_ptr<Layer> shape(const std::vector<Point>& points) {
  auto layer = std::make_shared<Layer>(Kind::shape);
  layer->anchor = {0, 0};
  layer->path = std::make_shared<Path>();
  layer->path->append({SegmentType::move, points.front()});
  for (std::size_t i = 1; i < points.size(); ++i)
    layer->path->append({SegmentType::line, points[i]});
  layer->path->append({SegmentType::close});
  return layer;
}
bool dimensions(const Layer& layer, double width, double height) {
  return layer.bounds.origin.x == 0 && layer.bounds.origin.y == 0 &&
         layer.bounds.size.width == width && layer.bounds.size.height == height;
}
bool co_brand(const Layer& layer) {
  if (layer.kind != Kind::layer || layer.children.size() != 3) return false;
  const auto& brand = layer.children[0];
  const auto& divider = layer.children[1];
  const auto& apple = layer.children[2];
  return brand && divider && apple && brand->kind == Kind::image && brand->image &&
         brand->children.empty() && dimensions(*brand, 376, 86) && divider->kind == Kind::shape &&
         dimensions(*divider, 0, 130) && apple->kind == Kind::image && apple->image &&
         dimensions(*apple, 494, 86);
}
struct Search {
  unsigned nodes = 0, matches = 0;
  bool bounded = true;
  std::vector<std::size_t> path, match;
  void visit(const std::shared_ptr<const Layer>& node) {
    if (!node || !bounded) return;
    if (++nodes > 8192 || path.size() > 64) {
      bounded = false;
      return;
    }
    if (co_brand(*node)) {
      ++matches;
      match = path;
      match.push_back(0);
    }
    for (std::size_t i = 0; i < node->children.size(); ++i) {
      path.push_back(i);
      visit(node->children[i]);
      path.pop_back();
      if (!bounded) break;
    }
  }
};
std::shared_ptr<Layer> artwork() {
  auto logo = std::make_shared<Layer>();
  logo->name = "HEADUNIT manufacturer greeting";
  logo->anchor = {0, 0};
  // Match the divider's height while retaining the manufacturer's original
  // 376 x 86 image frame and center. The original image did not clip children.
  constexpr double scale = 130. / 304.;
  logo->transform = {scale, 0, 0, scale, (376 - 430 * scale) / 2, (86 - 304 * scale) / 2};
  for (const auto& polygon : brand_polygons) {
    auto fill = shape(polygon);
    fill->fill = {1, 1, 1, 1};
    logo->insert(fill, logo->children.size());
  }
  return logo;
}
std::shared_ptr<Layer> replace(const std::shared_ptr<const Layer>& node,
                               const std::vector<std::size_t>& path, std::size_t depth,
                               const std::shared_ptr<Layer>& logo) {
  auto copy = std::make_shared<Layer>(*node);
  copy->parent.reset();
  if (depth == path.size()) {
    copy->kind = Kind::layer;
    copy->image.reset();
    copy->children = {logo};
  } else {
    const auto index = path[depth];
    copy->children[index] = replace(node->children[index], path, depth + 1, logo);
  }
  return copy;
}
}  // namespace
std::shared_ptr<const Layer> BrandGreeting::scene(const std::shared_ptr<const Layer>& root,
                                                  bool logos, std::uint64_t now) {
  if (retired_) return root;
  if (!logos || (last_ && now < *last_)) {
    if (last_) retired_ = true;
    return root;
  }
  last_ = now;
  Search search;
  search.visit(root);
  // A different or ambiguous scene remains untouched. No guessed rectangle
  // can erase Apple, a gauge, warning, phone picture or a later asset layout.
  if (!search.bounded || search.matches != 1) return root;
  if (!artwork_) artwork_ = artwork();
  return replace(root, search.match, 0, artwork_);
}
}  // namespace headunit::theme::graphics
