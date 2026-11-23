// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/local_vehicle_profile.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace headunit::theme::vehicle {
// Owner-thread adapter for the installed pack's local camera visibility state.
// The UID and on fields must share one repeated criticalInputStream instance.
// These are local semantic bindings, never phone-facing numeric resource IDs.
class CameraVisibility {
 public:
  CameraVisibility(const LocalProfile&, std::string_view stream_identifier,
                   std::uint64_t maximum_frame_age_ms, std::uint64_t now_ns);
  // Publish true only for a fresh, strictly newer capture timestamp. A repeat
  // never renews that true value. Inactive, future/reversed or stale
  // input explicitly publishes false; it never sends CP.INVALIDVALUE.
  // Returns true only when a fresh capture was published as visible.
  bool refresh(bool active, std::uint64_t capture_ns, std::uint64_t now_ns);
  // OFF is an owner presentation decision, not a new capture. It is immediate
  // even in the same millisecond as the last true publication.
  void withdraw(std::uint64_t now_ns);
  std::int64_t on_iid() const { return on_iid_; }
  bool visible() const { return visible_; }

 private:
  std::shared_ptr<ReadOnlyService> source_;
  std::int64_t on_iid_ = 0;
  std::uint64_t maximum_age_ns_ = 0, last_now_ns_ = 0, last_published_ms_ = 0, off_cutoff_ns_ = 0;
  std::optional<std::uint64_t> last_capture_ns_;
  bool visible_ = false;
  void observe(std::uint64_t now_ns);
};
}  // namespace headunit::theme::vehicle
