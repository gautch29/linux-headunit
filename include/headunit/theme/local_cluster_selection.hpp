// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/script_runtime.hpp>

namespace headunit::theme::script {
struct LocalClusterSelection {
  std::string layout, palette;
};
// Recheck pack-scoped saved choices in an isolated metadata VM before starting
// the display VM. An invalid cache preserves the configured signed default;
// an invalid configured default still rejects startup to native instruments.
LocalClusterSelection local_cluster_selection(ResourceReader, std::string configured_layout,
                                              std::optional<std::string_view> saved);
}  // namespace headunit::theme::script
