// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/scene.hpp>
#include <cmath>
#include <stdexcept>

namespace headunit::theme::graphics {
// Map an authored asset canvas into the receiver's actual output coordinates.
// The phone video already uses output pixels; the asset places that texture in
// its authored rectangle, so this cancels that placement's extra scaling.
// Shallow-copy only the root: never mutate a signed script's scene or copy pixels.
inline std::shared_ptr<const Layer> map_scene_canvas(const std::shared_ptr<const Layer>& root,
                                                     Size authored, Size output) {
  for (const auto n : {authored.width, authored.height, output.width, output.height})
    if (!std::isfinite(n) || n <= 0 || n > 4096)
      throw std::invalid_argument("invalid scene canvas dimensions");
  if (!root || (authored.width == output.width && authored.height == output.height)) return root;
  const double x = output.width / authored.width, y = output.height / authored.height;
  auto mapped = std::make_shared<Layer>(*root);
  mapped->parent.reset();
  mapped->position = {root->position.x * x, root->position.y * y};
  mapped->transform = Transform{x, 0, 0, y, 0, 0}.multiply(root->transform);
  return mapped;
}
// Output-space overlay is a sibling above the entire asset, outside its own
// clipping, masks, opacity and backdrop filters. Borrow nodes without calling
// insert(): signed scenes and cached overlays retain their original hierarchy.
inline std::shared_ptr<const Layer> with_scene_overlay(
    const std::shared_ptr<const Layer>& scene, const std::shared_ptr<const Layer>& overlay) {
  if (!overlay) return scene;
  auto root = std::make_shared<Layer>();
  root->anchor = {0, 0};
  root->group_blending = false;
  if (scene) root->children.push_back(std::const_pointer_cast<Layer>(scene));
  root->children.push_back(std::const_pointer_cast<Layer>(overlay));
  return root;
}
}  // namespace headunit::theme::graphics
