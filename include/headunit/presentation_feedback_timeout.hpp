// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <stdexcept>

namespace headunit {
// An unresolved submitted buffer exceeded its existing feedback deadline.
// Configuration, malformed feedback, output loss and GPU/VM faults are separate.
class PresentationFeedbackTimeout final : public std::runtime_error {
 public:
  PresentationFeedbackTimeout() : std::runtime_error("Wayland video feedback deadline exceeded") {}
};
}  // namespace headunit
