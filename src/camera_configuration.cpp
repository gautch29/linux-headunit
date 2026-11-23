// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/camera.hpp>
#include "theme/vehicle_json.hpp"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <utility>

namespace headunit::camera {
namespace {
using V = theme::opack::Value;
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
const theme::opack::Dictionary& object(const V& value,
                                       std::initializer_list<std::string_view> keys) {
  const auto* fields = std::get_if<theme::opack::Dictionary>(&value.data);
  check(fields && fields->size() == keys.size(), "invalid camera configuration fields");
  for (const auto& [key, v] : *fields) {
    (void)v;
    const auto* name = std::get_if<std::string>(&key.data);
    check(name && std::find(keys.begin(), keys.end(), *name) != keys.end(),
          "unknown camera configuration field");
  }
  return *fields;
}
const V& field(const theme::opack::Dictionary& fields, std::string_view key) {
  for (const auto& [k, v] : fields)
    if (std::get<std::string>(k.data) == key) return v;
  throw std::invalid_argument("missing camera configuration field");
}
unsigned integer(const V& value, unsigned limit) {
  const auto* n = std::get_if<std::int64_t>(&value.data);
  check(n && *n >= 0 && static_cast<std::uint64_t>(*n) <= limit,
        "camera configuration value must be a bounded integer");
  return static_cast<unsigned>(*n);
}
bool flag(const V& value) {
  const auto* result = std::get_if<bool>(&value.data);
  check(result, "camera configuration flag must be boolean");
  return *result;
}
std::string string(const V& value) {
  const auto* result = std::get_if<std::string>(&value.data);
  check(result, "camera configuration value must be a string");
  return *result;
}
bool identifier(std::string_view name) {
  return !name.empty() && name.size() <= 64 &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
         });
}
bool video_device(std::string_view name) {
  constexpr std::string_view prefix = "/dev/video";
  if (!name.starts_with(prefix) || name.size() <= prefix.size() || name.size() > 32) return false;
  const auto index = name.substr(prefix.size());
  if ((index.size() > 1 && index.front() == '0') ||
      !std::all_of(index.begin(), index.end(),
                   [](unsigned char c) { return c >= '0' && c <= '9'; }))
    return false;
  std::uint32_t number = 0;
  const auto parsed = std::from_chars(index.data(), index.data() + index.size(), number);
  return parsed.ec == std::errc{} && parsed.ptr == index.data() + index.size();
}
}  // namespace
void validate_configuration(const Configuration& c) {
  check(identifier(c.stream_identifier), "invalid local camera stream identifier");
  check(c.source == SourceKind::synthetic_test_pattern || c.source == SourceKind::external ||
            c.source == SourceKind::v4l2,
        "unknown camera source kind");
  check((c.source == SourceKind::v4l2) == c.v4l2.has_value(),
        "V4L2 source requires its own paired capture configuration");
  if (c.v4l2) {
    const auto& source = *c.v4l2;
    check(video_device(source.device), "V4L2 device must be a canonical bounded /dev/videoN path");
    check(source.pixel_format == CapturePixelFormat::yuyv ||
              source.pixel_format == CapturePixelFormat::rgb24 ||
              source.pixel_format == CapturePixelFormat::bgr24,
          "unknown V4L2 capture pixel format");
    check(
        source.delivery == CaptureDelivery::dma_buf || source.delivery == CaptureDelivery::cpu_copy,
        "unknown V4L2 capture delivery");
    check(source.delivery != CaptureDelivery::dma_buf ||
              source.pixel_format == CapturePixelFormat::yuyv,
          "V4L2 DMA-buf capture currently requires YUYV");
    check(source.pixel_format != CapturePixelFormat::yuyv ||
              (source.adaptation ? source.adaptation->width : c.width) % 2 == 0,
          "V4L2 YUYV capture requires an even width");
    if (source.adaptation) {
      const auto& a = *source.adaptation;
      check(source.delivery == CaptureDelivery::cpu_copy,
            "capture adaptation requires explicit CPU-copy delivery");
      check(a.width >= 64 && a.width <= 1920 && a.height >= 64 && a.height <= 1080,
            "camera adaptation acquisition dimensions exceed bounds");
      check(a.fit == CaptureFit::contain || a.fit == CaptureFit::cover,
            "unknown camera adaptation fit");
    }
  }
  check(c.width >= 64 && c.width <= 1920 && c.height >= 64 && c.height <= 1080,
        "camera capture dimensions exceed bounds");
  check(c.frame_rate >= 1 && c.frame_rate <= 60, "camera frame rate exceeds bounds");
  check(c.maximum_age_ms >= 30 && c.maximum_age_ms <= 1000 &&
            std::uint64_t(c.maximum_age_ms) * c.frame_rate >= 1000,
        "invalid camera freshness limit");
  check(c.panel == Panel::main || c.panel == Panel::cluster, "unknown camera destination panel");
  check(c.canvas_width >= 64 && c.canvas_width <= 4096 && c.canvas_height >= 64 &&
            c.canvas_height <= 2160,
        "camera destination canvas exceeds bounds");
  check(c.destination.width >= 32 && c.destination.height >= 32 &&
            c.destination.x <= c.canvas_width && c.destination.y <= c.canvas_height &&
            c.destination.width <= c.canvas_width - c.destination.x &&
            c.destination.height <= c.canvas_height - c.destination.y,
        "camera destination exceeds canvas");
  check(c.rotation_degrees == 0 || c.rotation_degrees == 90 || c.rotation_degrees == 180 ||
            c.rotation_degrees == 270,
        "unsupported camera rotation");
}
Configuration decode_configuration(std::string_view text) {
  check(text.size() <= 8192, "camera configuration exceeds 8 KiB");
  const auto json = theme::vehicle::detail::Json(text).parse();
  const auto& root = object(json, {"schemaVersion", "enabled", "manualActivation",
                                   "streamIdentifier", "source", "capture", "destination"});
  check(integer(field(root, "schemaVersion"), 1) == 1, "unsupported camera configuration schema");
  Configuration c;
  c.enabled = flag(field(root, "enabled"));
  c.manual_activation = flag(field(root, "manualActivation"));
  c.stream_identifier = string(field(root, "streamIdentifier"));
  const auto& source_value = field(root, "source");
  const auto* source_fields = std::get_if<theme::opack::Dictionary>(&source_value.data);
  check(source_fields, "camera source must be an object");
  const auto kind = string(field(*source_fields, "kind"));
  const auto& source =
      kind == "v4l2"
          ? (source_fields->size() == 5
                 ? object(source_value, {"kind", "device", "pixelFormat", "delivery", "adaptation"})
                 : object(source_value, {"kind", "device", "pixelFormat", "delivery"}))
          : object(source_value, {"kind"});
  if (kind == "synthetic-test-pattern")
    c.source = SourceKind::synthetic_test_pattern;
  else if (kind == "external")
    c.source = SourceKind::external;
  else if (kind == "v4l2") {
    c.source = SourceKind::v4l2;
    V4l2Configuration selected;
    selected.device = string(field(source, "device"));
    const auto pixel_format = string(field(source, "pixelFormat"));
    if (pixel_format == "yuyv")
      selected.pixel_format = CapturePixelFormat::yuyv;
    else if (pixel_format == "rgb24")
      selected.pixel_format = CapturePixelFormat::rgb24;
    else if (pixel_format == "bgr24")
      selected.pixel_format = CapturePixelFormat::bgr24;
    else
      throw std::invalid_argument("unknown V4L2 capture pixel format");
    const auto delivery = string(field(source, "delivery"));
    if (delivery == "dma-buf")
      selected.delivery = CaptureDelivery::dma_buf;
    else if (delivery == "cpu-copy")
      selected.delivery = CaptureDelivery::cpu_copy;
    else
      throw std::invalid_argument("unknown V4L2 capture delivery");
    if (source_fields->size() == 5) {
      const auto& adaptation = object(field(source, "adaptation"), {"width", "height", "fit"});
      CaptureAdaptation a;
      a.width = integer(field(adaptation, "width"), 1920);
      a.height = integer(field(adaptation, "height"), 1080);
      const auto fit = string(field(adaptation, "fit"));
      if (fit == "contain")
        a.fit = CaptureFit::contain;
      else if (fit == "cover")
        a.fit = CaptureFit::cover;
      else
        throw std::invalid_argument("unknown camera adaptation fit");
      selected.adaptation = a;
    }
    c.v4l2 = std::move(selected);
  } else
    throw std::invalid_argument("unknown camera source kind");
  const auto& capture =
      object(field(root, "capture"), {"width", "height", "frameRate", "maximumAgeMs"});
  c.width = integer(field(capture, "width"), 1920);
  c.height = integer(field(capture, "height"), 1080);
  c.frame_rate = integer(field(capture, "frameRate"), 60);
  c.maximum_age_ms = integer(field(capture, "maximumAgeMs"), 1000);
  const auto& destination =
      object(field(root, "destination"),
             {"panel", "canvas", "frame", "rotationDegrees", "mirrorHorizontal"});
  const auto panel = string(field(destination, "panel"));
  if (panel == "main")
    c.panel = Panel::main;
  else if (panel == "cluster")
    c.panel = Panel::cluster;
  else
    throw std::invalid_argument("unknown camera destination panel");
  const auto& canvas = object(field(destination, "canvas"), {"width", "height"});
  c.canvas_width = integer(field(canvas, "width"), 4096);
  c.canvas_height = integer(field(canvas, "height"), 2160);
  const auto& frame = object(field(destination, "frame"), {"x", "y", "width", "height"});
  c.destination = {integer(field(frame, "x"), 4096), integer(field(frame, "y"), 2160),
                   integer(field(frame, "width"), 4096), integer(field(frame, "height"), 2160)};
  c.rotation_degrees = integer(field(destination, "rotationDegrees"), 270);
  c.mirror_horizontal = flag(field(destination, "mirrorHorizontal"));
  validate_configuration(c);
  return c;
}
}  // namespace headunit::camera
