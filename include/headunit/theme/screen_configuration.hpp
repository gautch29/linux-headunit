// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/theme/protocol.hpp>

namespace headunit::theme::video {
enum class Codec { h264, h265 };
struct Configuration {
  Codec codec;
  Bytes codec_data;
  bool operator==(const Configuration&) const = default;
};
// Bare avcC/hvcC, direct boxes (including the reserved-zero-size screen form),
// or standard VisualSampleEntry containers. Lengths/parameter-set framing are
// checked here; codec syntax and negotiated dimensions are checked by Decoder.
// Empty repeat/keepalive configuration is handled by the owning session.
Configuration screen_configuration(std::span<const std::uint8_t>);
}  // namespace headunit::theme::video
