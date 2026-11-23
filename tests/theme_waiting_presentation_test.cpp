// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Explicit GPU buffer/real receipt qualification. Does not prove that a
// fullscreen compositor exposes the separately running native backup.
#include <headunit/theme/gpu_compositor.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace headunit::theme::graphics;
using Clock = std::chrono::steady_clock;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
PresentationReceipt collect(GpuCompositor& output, std::uint64_t token) {
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  for (;;) {
    if (auto receipt = output.poll_presentation()) {
      check(receipt->token == token && (!receipt->presented || receipt->correlation),
            "real waiting receipt was lost or reordered");
      return *receipt;
    }
    check(Clock::now() < deadline, "waiting presentation test deadline");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
void transparent(const Image& image) {
  check(!image.pixels.empty() &&
            std::all_of(image.pixels.begin(), image.pixels.end(), [](auto p) { return p == 0; }),
        "waiting buffer retained scene RGB, opacity or letterbox bars");
}
void fitted(const Image& image, std::uint32_t color, bool preview) {
  check(
      image.width && image.height && image.pixels.size() == std::size_t(image.width) * image.height,
      "invalid final image");
  if (preview) {
    check(image.width == 192 && image.height == 72, "SHM preview dimensions changed");
  } else
    check(image.width == 1280 && image.height == 720, "fullscreen configure was not applied");
  const double fit = std::min(double(image.width) / 192, double(image.height) / 72);
  const auto width = std::max(1u, std::uint32_t(192 * fit)),
             height = std::max(1u, std::uint32_t(72 * fit));
  const auto x0 = (image.width - width) / 2, y0 = (image.height - height) / 2;
  for (unsigned y = 0; y < image.height; ++y)
    for (unsigned x = 0; x < image.width; ++x)
      check(image.pixels[std::size_t(y) * image.width + x] ==
                (x < x0 || x >= x0 + width || y < y0 || y >= y0 + height ? 0xff000000 : color),
            "restored opaque output or fit is incorrect");
}
}  // namespace
int main(int argc, char** argv) try {
  check(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--preview"),
        "invalid waiting test arguments");
  const bool preview = argc == 2;
  WaylandOptions options{"Headunit waiting buffer regression"};
  options.opaque_background = true;
  options.max_pending_presentations = 2;
  if (preview)
    options.readback_render_node = "/dev/dri/renderD128";
  else
    options.output_name = "headless";  // Real configure fits the authored canvas into 1280x720.
  GpuCompositor output(192, 72, options);
  const Image image{192, 72, std::vector<std::uint32_t>(192 * 72, 0x80402010)};
  Image actual;
  output.compose_reference(image);
  output.submit(1, &actual);
  fitted(actual, 0xff402010, preview);
  check(collect(output, 1).presented, "normal initial image not presented");
  output.compose_reference(image);
  output.submit(2, &actual, WindowContent::reveal_underlay);
  transparent(actual);
  (void)collect(output, 2);  // Discard is honest; it does not qualify backup or PTS.
  output.compose_reference(image);
  output.submit(3, &actual);
  fitted(actual, 0xff402010, preview);
  check(collect(output, 3).presented, "normal image was not restored after transparent output");
  output.compose_reference(image);
  output.submit(4, &actual);
  fitted(actual, 0xff402010, preview);
  output.compose_reference(image);
  output.submit(5, &actual, WindowContent::reveal_underlay);
  transparent(actual);
  output.compose_reference(image);
  bool rejected = false;
  try {
    output.submit(6, nullptr, WindowContent::rendered_scene);
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected && !output.presentation_slot_available(),
        "overfull queue consumed another opacity transition");
  (void)collect(output, 4);
  (void)collect(output, 5);
  // Rejected submission preserved the composed frame and token capacity.
  output.submit(6, &actual);
  fitted(actual, 0xff402010, preview);
  check(collect(output, 6).presented && !output.presentation_pending(),
        "queue rejection broke normal restoration");
  std::cout << "Opaque/transparent/opaque final buffers, full letterbox clearing, ordered actual "
               "receipts and two-frame rejection passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
