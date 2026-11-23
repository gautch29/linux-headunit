// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/iap2_parameter.hpp"

#include <stdexcept>

namespace headunit {

std::vector<Iap2Parameter> decode_iap2_parameters(std::span<const std::uint8_t> bytes) {
  std::vector<Iap2Parameter> parameters;
  std::size_t cursor = 0;
  while (cursor < bytes.size()) {
    if (bytes.size() - cursor < 4) throw std::invalid_argument("truncated iAP2 parameter");
    const auto length = (static_cast<std::size_t>(bytes[cursor]) << 8) | bytes[cursor + 1];
    if (length < 4 || length > bytes.size() - cursor) {
      throw std::invalid_argument("invalid iAP2 parameter length");
    }
    const auto identifier = static_cast<std::uint16_t>(
        (static_cast<unsigned>(bytes[cursor + 2]) << 8) | bytes[cursor + 3]);
    parameters.push_back({identifier, std::vector<std::uint8_t>(bytes.begin() + cursor + 4,
                                                                bytes.begin() + cursor + length)});
    cursor += length;
  }
  return parameters;
}

std::vector<std::uint8_t> encode_iap2_parameters(std::span<const Iap2Parameter> parameters) {
  std::vector<std::uint8_t> bytes;
  for (const auto& parameter : parameters) {
    const auto length = parameter.value.size() + 4;
    if (length > 65535 || bytes.size() + length > 65535) {
      throw std::invalid_argument("iAP2 parameters exceed 16-bit length");
    }
    bytes.push_back(static_cast<std::uint8_t>(length >> 8));
    bytes.push_back(static_cast<std::uint8_t>(length));
    bytes.push_back(static_cast<std::uint8_t>(parameter.identifier >> 8));
    bytes.push_back(static_cast<std::uint8_t>(parameter.identifier));
    bytes.insert(bytes.end(), parameter.value.begin(), parameter.value.end());
  }
  return bytes;
}

}  // namespace headunit
