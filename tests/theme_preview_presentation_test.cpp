// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_compositor.hpp>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace headunit::theme::graphics;
using Clock = std::chrono::steady_clock;
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
PresentationReceipt collect(GpuCompositor& output) {
  const auto end = Clock::now() + std::chrono::seconds(2);
  while (Clock::now() < end) {
    if (auto receipt = output.poll_presentation()) return *receipt;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("preview did not recover actual presentation feedback");
}
int main() {
  try {
    const auto* raw = std::getenv("HEADUNIT_THEME_TEST_WESTON_PID");
    check(raw, "isolated compositor identity missing");
    const std::string_view text(raw);
    int pid = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), pid);
    check(error == std::errc{} && end == text.data() + text.size() && pid > 1 && pid != ::getpid(),
          "invalid isolated compositor identity");
    struct Resume {
      int pid;
      ~Resume() { ::kill(pid, SIGCONT); }
    } resume{pid};
    GpuCompositor output(128, 128,
                         WaylandOptions{"Headunit preview feedback test", "/dev/dri/renderD128"});
    const Image red{128, 128, std::vector<std::uint32_t>(128 * 128, 0xffff0000)},
        blue{128, 128, std::vector<std::uint32_t>(128 * 128, 0xff0000ff)};
    output.compose_reference(red);
    check(output.present(1).presented, "initial preview not presented");
    check(::kill(pid, SIGSTOP) == 0, "cannot suspend isolated test compositor");
    output.compose_reference(blue);
    output.submit(2);
    const auto until = Clock::now() + std::chrono::milliseconds(3300);
    while (Clock::now() < until) {
      check(!output.poll_presentation(),
            "preview invented a presentation while compositor was stopped");
      check(output.presentation_pending(), "preview lost its unresolved content update");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(output.preview_feedback_delayed(), "preview did not expose its missing feedback state");
    check(::kill(pid, SIGCONT) == 0, "cannot resume isolated test compositor");
    const auto second = collect(output);
    check(second.presented && second.token == 2,
          "preview did not receive its real pending presentation");
    output.compose_reference(red);
    const auto third = output.present(3);
    check(third.presented && third.token == 3 && third.received_ns >= second.received_ns,
          "preview could not present fresh content after recovery");
    std::cout << "Preview retained one pending update through missing feedback and resumed with "
                 "actual receipts\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
