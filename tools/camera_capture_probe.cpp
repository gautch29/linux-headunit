// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Explicit physical-device qualification. No image pixels are saved and no
// display, phone, radio or camera control owner is started by this probe.
#include <headunit/camera_capture.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <thread>

std::uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void write_timing(const std::optional<headunit::camera::CaptureTiming>& timing) {
  if (!timing) {
    std::cout << "null";
    return;
  }
  std::cout << "{\"driverSequence\":" << timing->driver_sequence
            << ",\"captureMonotonicNs\":" << timing->capture_monotonic_ns
            << ",\"observedMonotonicNs\":" << timing->observed_monotonic_ns << ",\"reference\":\""
            << (timing->reference == headunit::camera::CaptureReference::end_of_frame
                    ? "end-of-frame"
                    : "start-of-exposure")
            << "\",\"ageMs\":"
            << double(timing->observed_monotonic_ns - timing->capture_monotonic_ns) / 1000000
            << '}';
}
void write_rect(const headunit::theme::graphics::Rect& rect) {
  std::cout << "{\"x\":" << rect.origin.x << ",\"y\":" << rect.origin.y
            << ",\"width\":" << rect.size.width << ",\"height\":" << rect.size.height << '}';
}
int main(int argc, char** argv) try {
  if (argc != 2) throw std::invalid_argument("usage: headunit-camera-capture-probe CAMERA.json");
  std::ifstream file(argv[1]);
  if (!file) throw std::invalid_argument("cannot open capture profile");
  std::string text;
  for (char ch; file.get(ch);) {
    if (text.size() == 8192) throw std::invalid_argument("capture profile exceeds bound");
    text.push_back(ch);
  }
  const auto config = headunit::camera::decode_configuration(text);
  if (!config.enabled || config.source != headunit::camera::SourceKind::v4l2)
    throw std::invalid_argument("physical capture probe requires enabled V4L2");
  headunit::camera::Queue queue(config);
  headunit::camera::CaptureProvider provider(
      config, queue, [config] { return headunit::camera::make_v4l2_capture_device(config); });
  const auto end = now_ns() + 5000000000ULL;
  std::uint64_t frames = 0, maximum_age = 0, sum_age = 0;
  while (now_ns() < end) {
    if (provider.failure()) throw std::runtime_error(*provider.failure());
    const auto at = now_ns();
    if (auto frame = queue.stage(at)) {
      const auto age = at - frame->capture_monotonic_ns;
      maximum_age = std::max(maximum_age, age);
      sum_age += age;
      ++frames;
      queue.complete(frame->sequence, true);  // Queue consumption only; no display receipt.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  provider.request_stop();
  while (!provider.stopped())
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  if (const auto failure = provider.failure()) throw std::runtime_error(*failure);
  const auto stats = provider.statistics();
  if (frames < 30 || !stats.format) throw std::runtime_error("insufficient physical captures");
  const auto& pixels = stats.format->pixels;
  const auto fit = config.v4l2->adaptation ? config.v4l2->adaptation->fit
                                           : headunit::camera::CaptureFit::contain;
  const auto mapping = headunit::camera::capture_mapping(pixels.width, pixels.height, config.width,
                                                         config.height, fit);
  // Negotiated acquisition-to-delivery only. This probe has no signed scene,
  // orientation, compositor, display mode or optical observation to report.
  std::cout << "{\"physicalCapture\":true,\"displayQualified\":false,\"frames\":" << frames
            << ",\"driverWidth\":" << stats.format->pixels.width
            << ",\"driverHeight\":" << stats.format->pixels.height
            << ",\"deliveryWidth\":" << config.width << ",\"deliveryHeight\":" << config.height
            << ",\"meanAgeMs\":" << double(sum_age) / frames / 1000000
            << ",\"maximumAgeMs\":" << double(maximum_age) / 1000000
            << ",\"dequeued\":" << stats.dequeued << ",\"published\":" << stats.published
            << ",\"superseded\":" << stats.superseded << ",\"corrupted\":" << stats.corrupted
            << ",\"stale\":" << stats.stale << ",\"backpressure\":" << stats.backpressure
            << ",\"validSequenceGaps\":" << stats.sequence_gaps
            << ",\"maximumDequeueAgeMs\":" << double(stats.maximum_dequeue_age_ns) / 1000000
            << ",\"maximumPublicationAgeMs\":" << double(stats.maximum_publication_age_ns) / 1000000
            << ",\"lastDequeue\":";
  write_timing(stats.last_dequeued);
  std::cout << ",\"lastPublication\":";
  write_timing(stats.last_published);
  std::cout << std::setprecision(17) << ",\"captureGeometry\":{\"policy\":\""
            << (fit == headunit::camera::CaptureFit::contain ? "contain" : "cover")
            << "\",\"adaptationEnabled\":" << (config.v4l2->adaptation ? "true" : "false")
            << ",\"negotiatedStrideBytes\":" << pixels.stride
            << ",\"sourceAspect\":" << double(pixels.width) / pixels.height
            << ",\"deliveryAspect\":" << double(config.width) / config.height
            << ",\"scale\":" << mapping.scale << ",\"imageRect\":";
  write_rect(mapping.image);
  std::cout << ",\"sourceRect\":";
  write_rect(mapping.source);
  std::cout << ",\"deliveryRect\":";
  write_rect(mapping.destination);
  std::cout << ",\"signedSceneFit\":null,\"opticalFit\":null}";
  std::cout << "}\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
