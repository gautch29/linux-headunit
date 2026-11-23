// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/gpu_compositor.hpp>
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <string>

using namespace headunit::theme::graphics;
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int main() {
  try {
    // The asset retains 1920x720 coordinates; a compositor-selected output
    // must show the whole canvas with its aspect ratio preserved.
    WaylandOptions options{"Headunit kiosk fit regression"};
    const char* socket = std::getenv("WAYLAND_DISPLAY");
    check(socket, "missing test socket");
    options.display_name = socket;
    if (const char* requested = std::getenv("HEADUNIT_TEST_OUTPUT"))
      options.output_name = requested;
    const auto expected_width = std::getenv("HEADUNIT_TEST_WIDTH")
                                    ? std::stoul(std::getenv("HEADUNIT_TEST_WIDTH"))
                                    : 1280UL;
    const auto expected_height = std::getenv("HEADUNIT_TEST_HEIGHT")
                                     ? std::stoul(std::getenv("HEADUNIT_TEST_HEIGHT"))
                                     : 720UL;
    check(setenv("WAYLAND_DISPLAY", "deliberately-unavailable-test-socket", 1) == 0,
          "cannot set test environment");
    GpuCompositor output(1920, 720, options);
    for (std::uint64_t frame = 1; frame <= 3; ++frame) {
      const std::uint32_t color = frame % 2 ? 0xff00ff00 : 0xffff0000;
      output.compose_reference(Image{1920, 720, std::vector<std::uint32_t>(1920 * 720, color)});
      Image actual;
      const auto receipt = output.present(frame, &actual);
      check(receipt.presented && receipt.token == frame && receipt.correlation,
            "kiosk presentation lost its real receipt");
      if (actual.width != expected_width || actual.height != expected_height)
        throw std::runtime_error("requested output size was ignored: got " +
                                 std::to_string(actual.width) + "x" +
                                 std::to_string(actual.height));
      const double fit = std::min(double(expected_width) / 1920, double(expected_height) / 720);
      const auto fitted_width = std::max(1UL, static_cast<unsigned long>(1920 * fit));
      const auto fitted_height = std::max(1UL, static_cast<unsigned long>(720 * fit));
      const auto x0 = (expected_width - fitted_width) / 2,
                 y0 = (expected_height - fitted_height) / 2;
      for (unsigned long y = 0; y < expected_height; ++y)
        for (unsigned long x = 0; x < expected_width; ++x)
          check(actual.pixels[y * expected_width + x] ==
                    (x < x0 || x >= x0 + fitted_width || y < y0 || y >= y0 + fitted_height
                         ? 0xff000000
                         : color),
                "GPU fit stretched, cropped or contaminated the letterbox");
    }
    std::cout << "Wayland output configure, GPU aspect fit and real presentation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
