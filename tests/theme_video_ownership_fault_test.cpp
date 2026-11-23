// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/theme/video_stream.hpp"

#include <iostream>
#include <new>
#include <stdexcept>

void fail_allocation_after(int count);

namespace {
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
}  // namespace

int main() try {
  using namespace headunit::theme;
  unsigned faults = 0;
  for (int fail_at = 0; fail_at < 8; ++fail_at) {
    auto memory = std::make_shared<video::Memory>();
    video::Stream stream({"cpu", 4, 4}, memory);
    graphics::Image image{4, 4, std::vector<std::uint32_t>(16, 0xff123456)};
    const auto pixels = image.pixels;
    fail_allocation_after(fail_at);
    bool injected = false;
    try {
      check(stream.offer(1, std::move(image)), "offer refused an empty queue");
    } catch (const std::bad_alloc&) {
      injected = true;
    }
    fail_allocation_after(-1);
    if (injected) {
      ++faults;
      check(image.pixels == pixels, "allocation failure consumed the caller's frame");
      check(memory->bytes == 0 && stream.unused() && !stream.next_pts(),
            "allocation failure committed stream state");
      check(stream.offer(1, std::move(image)), "same frame was not retryable");
    }
    auto frame = stream.prepare().value();
    check(frame.token == 1 && frame.image && frame.image->pixels == pixels,
          "retry changed the token or pixels");
    check(!stream.current_pts() && memory->bytes == 64,
          "offer manufactured a presentation or lost byte accounting");
    stream.close();
    check(memory->bytes == 64, "close released an externally held frame");
    frame.image.reset();
    check(memory->bytes == 0, "last lease did not release memory accounting");
  }
  check(faults > 0, "no frame ownership allocation failure was injected");
  std::cout << "CPU video ownership allocation faults passed: " << faults << '\n';
} catch (const std::exception& error) {
  fail_allocation_after(-1);
  std::cerr << error.what() << '\n';
  return 1;
}
