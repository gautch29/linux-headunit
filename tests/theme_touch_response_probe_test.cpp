// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/touch_response_probe.hpp>
#include <iostream>
#include <stdexcept>

using headunit::theme::TouchResponseProbe;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)

namespace {
constexpr std::uint64_t ms = 1000000;
}

int main() try {
  {
    // Idle panel: press, response decoded, then presented.
    TouchResponseProbe probe;
    probe.press(1000 * ms);
    probe.decoded(1060 * ms);
    probe.presented(1075 * ms);
    const auto& s = probe.stats();
    CHECK(s.responses == 1 && s.unanswered == 0 && s.skipped_busy == 0);
    CHECK(s.decode_sum_ns == 60 * ms && s.decode_max_ns == 60 * ms);
    CHECK(s.present_sum_ns == 75 * ms && s.present_max_ns == 75 * ms);
  }
  {
    // A presentation before any post-press decode is not the response.
    TouchResponseProbe probe;
    probe.press(1000 * ms);
    probe.presented(1010 * ms);
    CHECK(probe.stats().responses == 0);
    probe.decoded(1050 * ms);
    probe.presented(1040 * ms);  // Receipt older than the decode is ignored.
    CHECK(probe.stats().responses == 0);
    probe.presented(1070 * ms);
    CHECK(probe.stats().responses == 1 && probe.stats().present_sum_ns == 70 * ms);
  }
  {
    // A busy panel (recent frames) cannot attribute a response to the press.
    TouchResponseProbe probe;
    probe.decoded(1000 * ms);
    probe.press(1100 * ms);
    CHECK(probe.stats().skipped_busy == 1);
    probe.decoded(1150 * ms);
    probe.presented(1160 * ms);
    CHECK(probe.stats().responses == 0);
    // After the idle interval a new press is measured again.
    probe.press(1150 * ms + TouchResponseProbe::idle_ns);
    probe.decoded(1150 * ms + TouchResponseProbe::idle_ns + 40 * ms);
    probe.presented(1150 * ms + TouchResponseProbe::idle_ns + 50 * ms);
    CHECK(probe.stats().responses == 1 && probe.stats().decode_max_ns == 40 * ms);
  }
  {
    // No response within the timeout counts as unanswered; a second press
    // while one is pending does not restart the measurement.
    TouchResponseProbe probe;
    probe.press(1000 * ms);
    probe.press(1100 * ms);
    probe.decoded(1200 * ms);
    probe.presented(1210 * ms);
    CHECK(probe.stats().responses == 1 && probe.stats().present_sum_ns == 210 * ms);
    probe.press(3000 * ms);
    probe.expire(3000 * ms + TouchResponseProbe::timeout_ns);
    CHECK(probe.stats().unanswered == 0);  // Exactly at the bound is still pending.
    probe.expire(3001 * ms + TouchResponseProbe::timeout_ns);
    CHECK(probe.stats().unanswered == 1);
    probe.decoded(5000 * ms);
    probe.presented(5010 * ms);
    CHECK(probe.stats().responses == 1);  // Late frame is not attributed.
  }
  std::cout << "touch response probe: idle attribution, ordering, busy skip and timeout checks "
               "passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
