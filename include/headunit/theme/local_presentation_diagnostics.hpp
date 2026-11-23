// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/presentation_clock.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <locale>
#include <sstream>
#include <time.h>

namespace headunit::theme::graphics {
// Observations on the existing rendering owner, never readiness or visibility.
// The caller keeps this fixed record alive until the complete local GPU/VM unwind.
struct LocalPresentationDiagnostics {
  struct Request {
    std::uint64_t token;
    std::optional<std::uint64_t> observed_ns;
  };
  struct SwapReturn {
    std::uint64_t token;
    bool success;
    std::optional<std::uint64_t> observed_ns;
  };
  struct Feedback {
    video::PresentationFeedback raw;
    bool discarded;
    std::optional<std::uint64_t> observed_ns;
  };
  struct Timeout {
    std::uint64_t oldest_token;
    unsigned pending_count;
    bool resolved;
    std::optional<std::uint64_t> observed_ns;
  };
  std::optional<std::uint64_t> entered_ns, last_dispatch_before_ns, last_dispatch_after_ns;
  std::optional<Request> first_request;
  std::optional<SwapReturn> first_swap_return;
  std::optional<Feedback> first_feedback;
  std::optional<video::PresentationFeedback> first_returned_receipt;
  std::optional<Timeout> timeout;
};

// Diagnostic failure is null data. Existing authoritative clock checks remain
// separate; this observer cannot throw, manufacture a date or retire an owner.
inline std::optional<std::uint64_t> local_diagnostic_clock(
    clockid_t clock = CLOCK_MONOTONIC) noexcept {
  timespec value{};
  if (::clock_gettime(clock, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 ||
      value.tv_nsec >= 1000000000)
    return {};
  const auto seconds = std::uint64_t(value.tv_sec);
  const auto nanoseconds = std::uint64_t(value.tv_nsec);
  if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) / 1000000000) return {};
  return seconds * 1000000000 + nanoseconds;
}

// One bounded numeric summary after unwind, never disk/JSON work in a callback.
inline void write_local_presentation_diagnostics(
    std::ostream& destination, const LocalPresentationDiagnostics& value) noexcept {
  try {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const auto date = [&](std::optional<std::uint64_t> sample) {
      if (sample)
        out << *sample;
      else
        out << "null";
    };
    const auto receipt = [&](const video::PresentationFeedback& raw, bool presented_event) {
      out << "{\"token\":" << raw.token << ",\"eventKind\":\""
          << (presented_event ? "presented" : "discarded") << "\",\"clockId\":" << raw.clock_id
          << ",\"clockIdSource\":\"connection_clock_id\",\"seconds\":";
      date(presented_event ? std::optional<std::uint64_t>{raw.seconds} : std::nullopt);
      out << ",\"nanoseconds\":";
      date(presented_event ? std::optional<std::uint64_t>{raw.nanoseconds} : std::nullopt);
      out << ",\"refreshNs\":";
      date(presented_event ? std::optional<std::uint64_t>{raw.refresh_ns} : std::nullopt);
      out << ",\"sequence\":";
      date(presented_event ? std::optional<std::uint64_t>{raw.sequence} : std::nullopt);
      out << ",\"flags\":";
      date(presented_event ? std::optional<std::uint64_t>{raw.flags} : std::nullopt);
      out << ",\"presented\":" << (raw.presented ? "true" : "false") << ",\"receivedMonotonicNs\":";
      if (raw.received_ns)
        out << raw.received_ns;
      else
        out << "null";
      out << ",\"correlation\":";
      if (presented_event && raw.correlation) {
        const auto& sample = *raw.correlation;
        out << "{\"clockId\":" << sample.clock_id << ",\"sourceNs\":" << sample.source_ns
            << ",\"beforeNs\":" << sample.before_ns << ",\"afterNs\":" << sample.after_ns << '}';
      } else
        out << "null";
      out << '}';
    };
    out << "Local Ultra presentation diagnostic {\"version\":1,\"observationClock\":"
           "\"CLOCK_MONOTONIC\",\"enteredNs\":";
    date(value.entered_ns);
    out << ",\"firstRequest\":";
    if (value.first_request) {
      out << "{\"token\":" << value.first_request->token << ",\"observedNs\":";
      date(value.first_request->observed_ns);
      out << '}';
    } else
      out << "null";
    out << ",\"firstSwapReturn\":";
    if (value.first_swap_return) {
      out << "{\"token\":" << value.first_swap_return->token
          << ",\"success\":" << (value.first_swap_return->success ? "true" : "false")
          << ",\"observedNs\":";
      date(value.first_swap_return->observed_ns);
      out << '}';
    } else
      out << "null";
    out << ",\"lastDispatchBeforeNs\":";
    date(value.last_dispatch_before_ns);
    out << ",\"lastDispatchAfterNs\":";
    date(value.last_dispatch_after_ns);
    out << ",\"firstFeedback\":";
    if (value.first_feedback) {
      out << "{\"discarded\":" << (value.first_feedback->discarded ? "true" : "false")
          << ",\"observedNs\":";
      date(value.first_feedback->observed_ns);
      out << ",\"raw\":";
      receipt(value.first_feedback->raw, !value.first_feedback->discarded);
      out << '}';
    } else
      out << "null";
    out << ",\"firstReturnedReceipt\":";
    if (value.first_returned_receipt)
      receipt(*value.first_returned_receipt, value.first_returned_receipt->presented);
    else
      out << "null";
    out << ",\"timeout\":";
    if (value.timeout) {
      out << "{\"oldestToken\":" << value.timeout->oldest_token
          << ",\"pendingCount\":" << value.timeout->pending_count
          << ",\"resolved\":" << (value.timeout->resolved ? "true" : "false") << ",\"observedNs\":";
      date(value.timeout->observed_ns);
      out << '}';
    } else
      out << "null";
    out << ",\"signedFrameCallbackNs\":null,\"occlusion\":null,\"opticalFrameNs\":null}\n";
    destination << out.str();
  } catch (...) {
    // A diagnostic sink failure cannot replace the original runtime outcome.
  }
}
}  // namespace headunit::theme::graphics
