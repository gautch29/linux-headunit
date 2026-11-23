// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/scene.hpp>

namespace headunit::theme::graphics {
// Replace only the manufacturer image in the admitted co-brand scene. The
// signed parent owns its movement/fades; Apple and the divider are borrowed
// unchanged. No script, resource, readiness or completion is modified.
class BrandGreeting {
 public:
  std::shared_ptr<const Layer> scene(const std::shared_ptr<const Layer>&, bool logos,
                                     std::uint64_t monotonic_ns);

 private:
  std::optional<std::uint64_t> last_;
  bool retired_ = false;
  std::shared_ptr<Layer> artwork_;
};
}  // namespace headunit::theme::graphics
