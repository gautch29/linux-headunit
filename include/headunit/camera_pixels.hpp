// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/camera.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

namespace headunit::camera {
inline constexpr std::size_t maximum_packed_capture_storage = 32U * 1024U * 1024U;

// A single progressive, uncropped plane negotiated with the capture driver.
// Stride includes row padding. Matrix/range describe the encoded YUYV samples;
// RGB24/BGR24 channels are already encoded RGB and are copied unchanged.
struct PackedCaptureFormat {
  unsigned width = 0, height = 0, stride = 0;
  CapturePixelFormat pixel_format = CapturePixelFormat::yuyv;
  theme::video::YuvMatrix matrix = theme::video::YuvMatrix::bt601;
  bool full_range = false;
};
void validate_packed_capture_format(const PackedCaptureFormat&);

// Explicit CPU-copy delivery only; this never substitutes for DMA-buf import.
// Source storage must cover the last visible row (its trailing padding is
// optional), and may not exceed stride*height or the global storage bound.
// No row padding is read. The returned opaque ARGB image owns its pixels and
// retains no capture-buffer address; a driver may reuse the source afterward.
// YUYV supports BT.601/709 full/limited quantization, with final RGB saturation.
// Transfer functions/primaries are not remapped; BT.2020 YUYV rejects.
theme::graphics::Image copy_capture_image(const PackedCaptureFormat&,
                                          std::span<const std::uint8_t>);
// Same bounded geometry used by the CPU adapter. This reports a software fit,
// not lens calibration, signed-slot placement or a presentation receipt.
CaptureMapping capture_mapping(unsigned source_width, unsigned source_height,
                               unsigned delivery_width, unsigned delivery_height, CaptureFit);
// Aspect-preserving CPU adaptation, executed by the capture worker. Contain
// uses opaque black bars; cover removes a centred crop. Bilinear sampling never
// changes the acquisition timestamp or turns copied pixels into a fresh frame.
// Optional output is the SAME calculation used by these pixels, written only
// on successful return. It does not adapt twice or infer applied geometry later.
theme::graphics::Image adapt_capture_image(const theme::graphics::Image&, unsigned width,
                                           unsigned height, CaptureFit,
                                           CaptureMapping* consumed_mapping = nullptr);
}  // namespace headunit::camera
