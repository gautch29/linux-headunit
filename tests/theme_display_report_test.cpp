// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/display_report.hpp>
#include <headunit/theme/input_presentation_report.hpp>
#include <iostream>
#include <stdexcept>
#include <sstream>

using headunit::theme::script::DisplayReportSeries;
using namespace std::chrono_literals;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
int main() try {
  std::ostringstream absent;
  headunit::theme::video::write_input_presentation(absent, {});
  CHECK(absent.str() == " videoPresentation=null");
  headunit::theme::video::NativeInputPresentation numeric;
  numeric.stream_token = 0xfedcba9876543210ULL;
  numeric.owner_connection_id = 0;
  numeric.source_pts = 899.75;
  numeric.receipt.feedback.token = 92;
  numeric.receipt.feedback.clock_id = 4;
  numeric.receipt.feedback.correlation =
      headunit::theme::video::ClockCorrelation{4, 2000, 3000, 3010};
  numeric.receipt.feedback.received_ns = 3200;
  numeric.receipt.mapped.monotonic_ns = 3100;
  numeric.decoded = headunit::theme::video::DecodedMetadata{0xfedcba9876543210ULL, 17, 2, 2};
  std::ostringstream encoded;
  encoded.precision(6);
  headunit::theme::video::write_input_presentation(encoded, numeric);
  CHECK(encoded.precision() == 6);
  CHECK(encoded.str().size() < 2048);
  numeric.decoded.reset();
  std::ostringstream cpu;
  headunit::theme::video::write_input_presentation(cpu, numeric);
  CHECK(cpu.str().find(R"("decodedSource":null)") != std::string::npos);
  CHECK(encoded.str().find(R"("streamToken":"18364758544493064720")") != std::string::npos);
  CHECK(encoded.str().find(R"("ownerConnectionId":"0")") != std::string::npos);
  CHECK(encoded.str().find(R"("presentedMonoNs":"3100","pollMonoNs":"3200")") != std::string::npos);
  CHECK(encoded.str().find(R"("senderClockMapping":null,"phoneAck":null,"optical":null})") !=
        std::string::npos);
  // Explicit synthetic steady dates; no phone, source frame or presentation.
  const auto origin = DisplayReportSeries::Clock::time_point{100s};
  DisplayReportSeries main, cluster;
  CHECK(!main.observe(origin, 4, 120));
  CHECK(!main.observe(origin, 5, 0));
  auto first = main.observe(origin + 123ns, 5, 120);
  CHECK(first && first->sequence == 1 && first->at == origin + 123ns && !first->previous &&
        !first->previous_sequence);
  std::ostringstream first_line;
  first->write_identity(first_line, 99000000000, {}, 17, 0);
  CHECK(first_line.str().find("mono_ns=100000000123 reportSequence=1 previousReportSequence=null "
                              "previousReportMonoNs=null") != std::string::npos);
  CHECK(first_line.str().find("publisherNoncePrivate=null publisherConnectionGeneration=null") !=
        std::string::npos);
  CHECK(first_line.str().find("outputRole=main frameCorrelation=null") != std::string::npos);
  CHECK(!main.observe(origin + 5s, 60, 120));
  // A late reporting turn must preserve the last producer date, not invent a
  // nominal five-second start or borrow the other panel's reporting date.
  auto other = cluster.observe(origin + 2s, 5, 2);
  CHECK(other && !other->previous);
  auto delayed = main.observe(origin + 17s + 42ns, 60, 120);
  CHECK(delayed && delayed->sequence == 2 && delayed->previous_sequence == 1 &&
        delayed->previous == first->at && delayed->at == origin + 17s + 42ns);
  std::ostringstream authenticated;
  delayed->write_identity(authenticated, 99000000000, "0123456789abcdef0123456789abcdef", 0, 0);
  CHECK(authenticated.str().find("previousReportSequence=1 previousReportMonoNs=100000000123") !=
        std::string::npos);
  CHECK(authenticated.str().find("publisherNoncePrivate=0123456789abcdef0123456789abcdef "
                                 "publisherConnectionGeneration=0") != std::string::npos);
  std::ostringstream invalid;
  other->write_identity(invalid, 99000000000, "injected\nscenes=999", 23, 1);
  CHECK(invalid.str().find("injected") == std::string::npos);
  CHECK(invalid.str().find(
            "publisherNoncePrivate=null publisherConnectionGeneration=null outputRole=cluster") !=
        std::string::npos);
  auto other_second = cluster.observe(origin + 9s, 70, 2);
  CHECK(other_second && other_second->previous == other->at && other_second->sequence == 2);
  CHECK(!cluster.observe(origin + 50s, 800, 2));
  CHECK(!main.observe(origin + 16s, 90, 120));
  // Budget exhaustion does not restart the series or permit a stale redraw to
  // create an additional window. Counters may stay unchanged in real idle scenes.
  auto previous = delayed;
  for (unsigned sequence = 3; sequence <= 120; ++sequence) {
    const auto at = origin + std::chrono::seconds(sequence * 7 + 17);
    auto report = main.observe(at, 60, 120);
    CHECK(report && report->sequence == sequence && report->previous == previous->at &&
          report->previous_sequence == sequence - 1);
    previous = report;
  }
  CHECK(!main.observe(origin + 1h, 60, 120));
  // A successor owner has no predecessor even with identical aggregate values.
  DisplayReportSeries successor;
  auto renewed = successor.observe(origin + 2h, 60, 120);
  CHECK(renewed && renewed->sequence == 1 && !renewed->previous && !renewed->previous_sequence);
  std::cout << "display report owner predecessor/cadence/budget/replacement checks passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
