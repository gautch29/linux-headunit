// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/vehicle_configuration.hpp>
#include <headunit/theme/local_vehicle_profile.hpp>
#include <mutex>

namespace headunit::theme::vehicle {
// Copied connection-owned intent only. The scene consumes this on its worker;
// no script object crosses into the receiver thread. It is not a view receipt.
class ContentStateSnapshot {
 public:
  struct State {
    bool on = false;
    std::string url;
    std::uint64_t decided_ms = 0, expires_ms = 0;
    std::uint64_t command_sequence = 0;
  };
  explicit ContentStateSnapshot(ContentRequestCapability capability);
  void update(State);
  void close(std::uint64_t now_ms);
  void dismiss(std::uint64_t now_ms);
  std::uint64_t dismissal_generation() const;
  void allow_content(bool);
  State read(std::uint64_t now_ms, bool include_hidden = false) const;
  const ContentRequestCapability& capability() const { return capability_; }

 private:
  const ContentRequestCapability capability_;
  mutable std::mutex mutex_;
  State state_;
  bool closed_ = false;
  bool allowed_ = false;
  std::optional<std::uint64_t> retired_command_;
  std::uint64_t dismissals_ = 0;
};
// Explicit local RequestContent bindings are required in the sealed profile.
// Validate and publish them together; no camera/ADAS identity is repurposed.
class ContentLocalInput {
 public:
  ContentLocalInput(const LocalProfile&, std::shared_ptr<ContentStateSnapshot>,
                    std::uint64_t now_ms = 0);
  void pump(std::uint64_t now_ms, bool allow_content = true);
  ScriptView::Dismissals dismissals() const;

 private:
  std::shared_ptr<ReadOnlyService> source_;
  std::shared_ptr<ContentStateSnapshot> state_;
  std::int64_t on_ = 0, url_ = 0;
  std::optional<std::uint64_t> last_published_;
};
// Existing receiver/control owner only. A bounded private request carries a
// launch nonce, control-connection generation, explicit command sequence and
// short monotonic lease. It requests Remote UI; it never confirms presentation.
// No VM/GPU, car I/O, socket or additional worker lives in this adapter.
std::shared_ptr<Provider> content_request_provider(std::shared_ptr<Provider>,
                                                   const ContentRequestCapability&,
                                                   std::string launch_nonce,
                                                   std::uint64_t connection_generation,
                                                   std::shared_ptr<ContentStateSnapshot> = {});
void validate_content_request_endpoint(std::string_view);
}  // namespace headunit::theme::vehicle
