// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/connection_diagnostics.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void check(bool value) {
  if (!value) throw std::runtime_error("connection diagnostic test failed");
}
struct Unwritable final : std::streambuf {
  int_type overflow(int_type) override {
    throw std::runtime_error("Explicit unavailable diagnostic output");
  }
};
}  // namespace
int main() {
  using Trace = headunit::ConnectionDiagnostics;
  using Stage = Trace::Stage;
  std::ostringstream output;
  std::ostringstream startup_output;
  Trace startup(Trace::Scope::startup, 0, true);
  check(startup.observe(Stage::control_listening, 100, 0, 0, startup_output));
  check(startup.observe(Stage::iap_profile_ready, 120, 0, 0, startup_output));
  check(!startup.observe(Stage::iap_profile_ready, 130, 0, 0, startup_output));
  check(startup_output.str() ==
        "CarPlay connection diagnostic mono_ns=100 scope=startup generation=0 "
        "stage=control-listening elapsed_ns=0 result=0 linkGeneration=0\n"
        "CarPlay connection diagnostic mono_ns=120 scope=startup generation=0 "
        "stage=iap-profile-ready elapsed_ns=20 result=0 linkGeneration=0\n");
  Trace disabled(Trace::Scope::control, 1, false);
  check(!disabled.observe(Stage::accepted, 100, 0, 0, output) && output.str().empty());
  Trace trace(Trace::Scope::control, 7, true);
  check(!trace.observe(Stage::accepted, 0, 0, 0, output));
  check(trace.observe(Stage::accepted, 100, 0, 0, output));
  check(!trace.observe(Stage::accepted, 200, 0, 0, output));
  check(!trace.observe(Stage::encrypted_control, 99, 0, 0, output));
  check(trace.observe(Stage::encrypted_control, 110, 0, 0, output));
  check(trace.observe(Stage::initial_setup_end, 200, 400, 2, output));
  check(trace.observe(Stage::retired, 210,
                      static_cast<unsigned>(Trace::ControlRetirement::peer_eof), 2, output));
  check(output.str() ==
        "CarPlay connection diagnostic mono_ns=100 scope=control generation=7 stage=accepted "
        "elapsed_ns=0 result=0 linkGeneration=0\n"
        "CarPlay connection diagnostic mono_ns=110 scope=control generation=7 "
        "stage=encrypted-control elapsed_ns=10 result=0 linkGeneration=0\n"
        "CarPlay connection diagnostic mono_ns=200 scope=control generation=7 "
        "stage=initial-setup-end elapsed_ns=100 result=400 linkGeneration=2\n"
        "CarPlay connection diagnostic mono_ns=210 scope=control generation=7 stage=retired "
        "elapsed_ns=110 result=1 linkGeneration=2\n");
  check(!trace.observe(static_cast<Stage>(999), 300, 0, 0, output));
  check(!trace.observe(Stage::video_frame, 300, 0, 0, output));
  // A new owner has an independent timeline; no prior state is inherited.
  Trace replacement(Trace::Scope::control, 8, true);
  check(replacement.observe(Stage::accepted, 220, 0, 0, output));
  check(output.str().find("generation=8 stage=accepted elapsed_ns=0") != std::string::npos);
  check(replacement.observe(Stage::negotiated_standard, 230, 0, 3, output));
  check(replacement.observe(Stage::main_stream_setup, 240, 0, 3, output));
  check(!replacement.observe(Stage::negotiated_standard, 250, 0, 3, output));
  check(output.str().find(
            "generation=8 stage=negotiated-standard elapsed_ns=10 result=0 linkGeneration=3") !=
        std::string::npos);
  check(output.str().find(
            "generation=8 stage=main-stream-setup elapsed_ns=20 result=0 linkGeneration=3") !=
        std::string::npos);
  Unwritable buffer;
  std::ostream bad(&buffer);
  bad.exceptions(std::ios::badbit | std::ios::failbit);
  Trace unavailable(Trace::Scope::iap, 2, true);
  check(!unavailable.observe(Stage::accepted, 100, 0, 0, bad));
  check(!unavailable.observe(Stage::accepted, 101, 0, 0, output));
  // Fixed per-owner observations and the process ceiling both bound storage/log
  // volume even when a peer repeatedly creates and retires generations.
  std::ostringstream bounded;
  unsigned emitted = 0;
  for (unsigned generation = 0; generation < 1000; ++generation) {
    Trace next(Trace::Scope::control, generation, true);
    for (unsigned stage = 0; stage < static_cast<unsigned>(Stage::count); ++stage)
      emitted += next.observe(static_cast<Stage>(stage), 100 + stage, 0, 0, bounded);
  }
  check(emitted == 502);  // Ten prior observations consumed the same real budget.
  check(bounded.str().size() < 100000);
}
