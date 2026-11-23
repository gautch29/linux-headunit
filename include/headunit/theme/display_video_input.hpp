// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/video_stream.hpp>
#include <headunit/theme/media_clock.hpp>
#include <headunit/theme/presentation_clock.hpp>

namespace headunit::theme::video {
// Copied only after the display owner accepts real scene feedback.
// feedback.token is the Wayland submission, distinct from the Stream token.
// received_ns is the poll observation; mapped.monotonic_ns is presentation.
struct InputPresentationReceipt {
  std::uint64_t submission_epoch = 0, current_epoch = 0, submitted_ns = 0;
  PresentationFeedback feedback;
  PresentationTime mapped{};
};
// A display-owner interface; transport producers never call these methods.
// A null clock means keep decoding references but discard decoded output.
class DisplayVideoInput {
 public:
  virtual ~DisplayVideoInput() = default;
  virtual const std::shared_ptr<Stream>& stream() const = 0;
  virtual void pump(const MediaClock*) = 0;
  // Every newly staged token belongs to one display submission. A later draw
  // may stage a different frame while the earlier receipt is outstanding.
  virtual std::optional<std::uint64_t> stage(const MediaClock&) = 0;
  virtual void complete(std::uint64_t token, bool presented) = 0;
  // Legacy consumers retain their completion policy. Null means no accepted
  // mapped receipt, including explicit discard/retirement.
  virtual void complete(std::uint64_t token, bool presented, const InputPresentationReceipt*) {
    complete(token, presented);
  }
  virtual void suspend() = 0;
  // Local captures can expire during cold/slow GPU composition. The display
  // rechecks before submitting pixels, independently of clock qualification.
  virtual bool composition_fresh() const { return true; }
};
}  // namespace headunit::theme::video
