// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include "headunit/opack.hpp"

#include <span>
#include <string>

namespace headunit {
struct UltraUiEnvelope {
  // Wire key displayUUID also carries case-sensitive signed-asset panel names.
  std::string display_uuid;
  OpackValue::Bytes binary_plist;
};

OpackValue::Bytes encode_ultra_ui_envelope(const UltraUiEnvelope& message);
// Only routes to an explicitly configured display. The inner binary plist is
// opaque here; the UI session must validate it before executing any command.
UltraUiEnvelope decode_ultra_ui_envelope(std::span<const std::uint8_t> message,
                                         std::span<const std::string> configured_displays);
}  // namespace headunit
