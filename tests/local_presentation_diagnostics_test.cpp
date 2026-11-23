// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/local_presentation_diagnostics.hpp>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace headunit::theme::graphics;
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int main() try {
  const auto before = local_diagnostic_clock();
  const auto after = local_diagnostic_clock();
  check(before && after && *after >= *before, "original monotonic clock unavailable");
  if constexpr (std::numeric_limits<clockid_t>::is_specialized) {
    check(!local_diagnostic_clock(std::numeric_limits<clockid_t>::max()),
          "failed diagnostic clock became a date");
  } else {
    // Darwin clockid_t is an enum: numeric_limits::max() is zero, a VALID
    // realtime clock, not an invalid syscall fixture. Linux exercises failure.
    std::cout << "Invalid clock-ID syscall fixture unavailable for enum clockid_t\n";
  }
  LocalPresentationDiagnostics empty;
  std::ostringstream absent;
  write_local_presentation_diagnostics(absent, empty);
  check(absent.str().find("\"firstRequest\":null") != std::string::npos &&
            absent.str().find("\"firstReturnedReceipt\":null") != std::string::npos &&
            absent.str().find("\"occlusion\":null") != std::string::npos,
        "absent observations acquired authority");

  // Explicit synthetic records exercise encoding only, not real presentation.
  LocalPresentationDiagnostics fixture;
  fixture.first_request = LocalPresentationDiagnostics::Request{17, {}};
  fixture.first_swap_return = LocalPresentationDiagnostics::SwapReturn{17, false, {}};
  fixture.timeout = LocalPresentationDiagnostics::Timeout{17, 2, false, {}};
  fixture.first_feedback = LocalPresentationDiagnostics::Feedback{{}, true, {}};
  fixture.first_returned_receipt = headunit::theme::video::PresentationFeedback{};
  std::ostringstream encoded;
  encoded << std::hex;
  write_local_presentation_diagnostics(encoded, fixture);
  check(encoded.str().find("\"token\":17") != std::string::npos &&
            encoded.str().find("\"success\":false,\"observedNs\":null") != std::string::npos &&
            encoded.str().find("\"discarded\":true") != std::string::npos &&
            encoded.str().find("\"eventKind\":\"discarded\"") != std::string::npos &&
            encoded.str().find("\"seconds\":null,\"nanoseconds\":null,\"refreshNs\":null,"
                               "\"sequence\":null,\"flags\":null") != std::string::npos,
        "swap/discard/timeout invented an accepted receipt or a missing date");
  check(encoded.str().size() < 4096, "fixed diagnostic encoding exceeded its bound");
  encoded << 17;
  check(encoded.str().ends_with("11"), "diagnostic changed its caller's format");
  // A malformed presented EVENT still supplied raw fields. Its validation
  // boolean must not erase genuine zero or turn that event into a discard.
  fixture.first_feedback->discarded = false;
  fixture.first_feedback->raw.flags = 16;
  fixture.first_returned_receipt.reset();
  std::ostringstream malformed;
  write_local_presentation_diagnostics(malformed, fixture);
  check(malformed.str().find("\"eventKind\":\"presented\"") != std::string::npos &&
            malformed.str().find("\"seconds\":0,\"nanoseconds\":0,\"refreshNs\":0,"
                                 "\"sequence\":0,\"flags\":16,\"presented\":false") !=
                std::string::npos &&
            malformed.str().find("\"firstReturnedReceipt\":null") != std::string::npos,
        "malformed presented event lost raw fields or acquired accepted authority");
  std::ostringstream failed;
  failed.exceptions(std::ios::badbit | std::ios::failbit);
  try {
    failed.setstate(std::ios::badbit);
  } catch (const std::ios_base::failure&) {
  }
  write_local_presentation_diagnostics(failed, fixture);
  std::cout << "Original clock/null failure and bounded encoding preserve observation authority\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
