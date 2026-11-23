// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

// Pure configuration parsing/validation. No provider, device, DMA buffer,
// ioctl, display or real capture is constructed by this test.
#include <headunit/camera.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace c = headunit::camera;
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
template <class F>
void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "unsafe or malformed camera configuration was accepted");
}
std::string document(const std::string& source, unsigned width = 1280) {
  return "{\"schemaVersion\":1,\"enabled\":true,\"manualActivation\":true,\"streamIdentifier\":"
         "\"rear-camera\",\"source\":" +
         source + ",\"capture\":{\"width\":" + std::to_string(width) +
         ",\"height\":720,\"frameRate\":30,\"maximumAgeMs\":150},"
         "\"destination\":{\"panel\":\"main\",\"canvas\":{\"width\":1920,\"height\":720},"
         "\"frame\":{\"x\":320,\"y\":0,\"width\":1280,\"height\":720},\"rotationDegrees\":0,"
         "\"mirrorHorizontal\":false}}";
}
std::string source(const std::string& device = "/dev/video0", const std::string& format = "yuyv",
                   const std::string& delivery = "dma-buf") {
  return "{\"kind\":\"v4l2\",\"device\":\"" + device + "\",\"pixelFormat\":\"" + format +
         "\",\"delivery\":\"" + delivery + "\"}";
}
void configuration_matrix() {
  for (const auto& device :
       {"/dev/video0", "/dev/video1", "/dev/video42", "/dev/video4294967295"}) {
    const auto result = c::decode_configuration(document(source(device)));
    check(result.source == c::SourceKind::v4l2 && result.v4l2 && result.v4l2->device == device &&
              result.v4l2->pixel_format == c::CapturePixelFormat::yuyv &&
              result.v4l2->delivery == c::CaptureDelivery::dma_buf,
          "V4L2 DMA source identity was changed");
  }
  for (const auto& format : {"yuyv", "rgb24", "bgr24"}) {
    const auto result =
        c::decode_configuration(document(source("/dev/video5", format, "cpu-copy")));
    check(result.v4l2->delivery == c::CaptureDelivery::cpu_copy,
          "explicit CPU copy was treated as DMA-buf");
  }
  for (const auto& kind : {"synthetic-test-pattern", "external"}) {
    const auto result =
        c::decode_configuration(document("{\"kind\":\"" + std::string(kind) + "\"}"));
    check(!result.v4l2, "legacy source unexpectedly carries a device selection");
  }
  rejects([] { c::decode_configuration(document(source("/dev/video0", "rgb24"))); });
  rejects([] { c::decode_configuration(document(source("/dev/video0", "bgr24"))); });
  rejects([] { c::decode_configuration(document(source(), 1279)); });
  rejects(
      [] { c::decode_configuration(document(source("/dev/video0", "yuyv", "cpu-copy"), 1279)); });
  const auto odd_rgb =
      c::decode_configuration(document(source("/dev/video0", "rgb24", "cpu-copy"), 1279));
  check(odd_rgb.width == 1279, "RGB CPU source inherited YUYV's paired-pixel width rule");
  rejects([] { c::decode_configuration(document(source("/dev/video0", "nv12"))); });
  rejects([] { c::decode_configuration(document(source("/dev/video0", "yuyv", "automatic"))); });
}
void adaptation_configuration() {
  auto text = source("/dev/video0", "yuyv", "cpu-copy");
  text.pop_back();
  text += R"(,"adaptation":{"width":1280,"height":720,"fit":"contain"}})";
  const auto decoded = c::decode_configuration(document(text, 1650));
  check(decoded.width == 1650 && decoded.v4l2->adaptation->width == 1280 &&
            decoded.v4l2->adaptation->fit == c::CaptureFit::contain,
        "capture geometry confused with signed-slot delivery geometry");
  auto bad = decoded;
  bad.v4l2->delivery = c::CaptureDelivery::dma_buf;
  rejects([&] { c::validate_configuration(bad); });
  bad = decoded;
  bad.v4l2->adaptation->width = 1279;
  rejects([&] { c::validate_configuration(bad); });
  bad = decoded;
  bad.v4l2->adaptation->height = 1081;
  rejects([&] { c::validate_configuration(bad); });
  auto unknown = text;
  unknown.replace(unknown.find("contain"), 7, "stretch");
  rejects([&] { c::decode_configuration(document(unknown)); });
}
void source_fields() {
  for (const auto& source_value : std::vector<std::string>{
           "{\"kind\":\"v4l2\"}",
           "{\"kind\":\"v4l2\",\"device\":\"/dev/video0\",\"pixelFormat\":\"yuyv\"}",
           "{\"kind\":\"v4l2\",\"device\":\"/dev/video0\",\"delivery\":\"dma-buf\"}",
           "{\"kind\":\"v4l2\",\"pixelFormat\":\"yuyv\",\"delivery\":\"dma-buf\"}",
           "{\"kind\":\"v4l2\",\"device\":false,\"pixelFormat\":\"yuyv\",\"delivery\":\"dma-buf\"}",
           "{\"kind\":\"v4l2\",\"device\":\"/dev/"
           "video0\",\"pixelFormat\":null,\"delivery\":\"dma-buf\"}",
           "{\"kind\":\"v4l2\",\"device\":\"/dev/video0\",\"pixelFormat\":\"yuyv\",\"delivery\":3}",
           "{\"kind\":\"v4l2\",\"device\":\"/dev/"
           "video0\",\"pixelFormat\":\"yuyv\",\"delivery\":\"dma-buf\",\"unknown\":true}",
           "{\"kind\":\"external\",\"device\":\"/dev/"
           "video0\",\"pixelFormat\":\"yuyv\",\"delivery\":\"dma-buf\"}",
           "{\"kind\":\"synthetic-test-pattern\",\"device\":\"/dev/video0\"}",
           "{\"kind\":\"external\",\"delivery\":\"cpu-copy\"}",
           "{\"kind\":\"v4l2\",\"kind\":\"external\",\"device\":\"/dev/"
           "video0\",\"pixelFormat\":\"yuyv\",\"delivery\":\"dma-buf\"}",
           "{\"kind\":\"camera\"}", "{\"kind\":false}", "[]", "null"})
    rejects([&] { c::decode_configuration(document(source_value)); });
}
void lexical_devices() {
  for (const auto& device : std::vector<std::string>{
           "", "video0", "/dev/video", "/dev/video01", "/dev/video-1", "/dev/video+1",
           "/dev/video4294967296", "/dev/video0/", "/dev//video0", "/dev/./video0",
           "/dev/../dev/video0", "/dev/v4l/by-id/camera", "/dev/video0;echo bad",
           "/dev/video0$(echo bad)", "/dev/video0\\u0000ignored", "/dev/video0\\n",
           "/dev/video" + std::string(100, '1')})
    rejects([&] { c::decode_configuration(document(source(device))); });
}
void programmatic_configuration() {
  auto result = c::decode_configuration(document(source()));
  auto bad = result;
  bad.v4l2.reset();
  rejects([&] { c::validate_configuration(bad); });
  for (const auto kind : {c::SourceKind::external, c::SourceKind::synthetic_test_pattern}) {
    bad = result;
    bad.source = kind;
    rejects([&] { c::validate_configuration(bad); });
  }
  bad = result;
  bad.v4l2->pixel_format = static_cast<c::CapturePixelFormat>(99);
  rejects([&] { c::validate_configuration(bad); });
  bad = result;
  bad.v4l2->delivery = static_cast<c::CaptureDelivery>(99);
  rejects([&] { c::validate_configuration(bad); });
  bad = result;
  bad.v4l2->device = std::string("/dev/video0\0ignored", 19);
  rejects([&] { c::validate_configuration(bad); });
  // Appending metadata preserves old aggregate users; absence never invents an
  // exposure boundary or labels a test pattern as a hardware acquisition.
  const c::Frame legacy{1, 2, true, {}, {}};
  check(!legacy.capture_reference, "legacy aggregate acquired fabricated hardware metadata");
  const c::Frame acquired{3, 4, false, {}, {}, c::CaptureReference::end_of_frame};
  check(acquired.capture_reference == c::CaptureReference::end_of_frame,
        "hardware timestamp reference was lost");
}
void disabled_template() {
  std::ifstream file(std::string(HEADUNIT_SOURCE_DIR) + "/config/camera-v4l2.example.json");
  check(bool(file), "V4L2 configuration example is missing");
  std::stringstream text;
  text << file.rdbuf();
  const auto result = c::decode_configuration(text.str());
  check(!result.enabled && result.manual_activation && result.source == c::SourceKind::v4l2 &&
            result.v4l2 && result.v4l2->device == "/dev/video0" &&
            result.v4l2->delivery == c::CaptureDelivery::dma_buf &&
            result.v4l2->pixel_format == c::CapturePixelFormat::yuyv,
        "V4L2 example activates a device or changes its explicit route");
}
}  // namespace
int main() try {
  configuration_matrix();
  adaptation_configuration();
  source_fields();
  lexical_devices();
  programmatic_configuration();
  disabled_template();
  std::ifstream usb(std::string(HEADUNIT_SOURCE_DIR) + "/config/camera-ultra-usb.example.json");
  check(bool(usb), "Ultra USB example is missing");
  std::stringstream usb_text;
  usb_text << usb.rdbuf();
  const auto adapted = c::decode_configuration(usb_text.str());
  check(!adapted.enabled && adapted.width == 1650 && adapted.v4l2->adaptation->width == 640,
        "USB template activates hardware or confuses acquisition with delivery");
  std::cout << "Strict V4L2 configuration, source isolation, acquisition metadata and disabled "
               "template passed (no device I/O)\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
