// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace headunit::camera {
inline constexpr std::size_t maximum_activation_request_bytes = 24;
enum class ActivationRequest { unavailable, off, on };
// Optional camera activation. The configured request lives in a real
// owner-only directory; an owned single-link 0600 regular file contains exactly
// on\n, off\n, or on <positive host-monotonic deadline in ns>\n. Managed requests
// expire and cannot promise more than one second. No missing, malformed,
// redirected or unsafe request can activate it.
// Construct before the receiver listens. This reader does not activate a view
// or own a provider; its caller applies the returned request on the display owner.
class ActivationControl {
 public:
  explicit ActivationControl(std::string absolute_path);
  ~ActivationControl();
  ActivationControl(const ActivationControl&) = delete;
  ActivationControl& operator=(const ActivationControl&) = delete;
  // No remembered 'on' state: every call reads a bounded current request.
  // Request errors return false; replacing/changing the parent fails closed.
  bool poll() const noexcept;
  // Explicit host-clock input for owners/fixtures. A reversed/nonpositive
  // clock retires the reader; a new clock domain requires a new reader.
  bool poll(std::uint64_t now_ns) const noexcept;
  // Preserve the distinction between a valid OFF/expired lease and an unreadable
  // input. Both suppress activation, but unavailable input is not an intentional
  // off edge that may clear a managed failed-attempt latch.
  ActivationRequest read(std::uint64_t now_ns) const noexcept;
  ActivationRequest read() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace headunit::camera
