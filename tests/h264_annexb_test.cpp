// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/h264_annexb.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

int main() {
  headunit::H264AnnexB converter;
  const std::vector<std::uint8_t> config{1,    100,  0, 31, 0xff, 0xe1, 0,   2,
                                         0x67, 0x64, 1, 0,  2,    0x68, 0xee};
  converter.set_configuration(config);
  if (!converter.configured()) throw std::runtime_error("configuration missing");
  const std::vector<std::uint8_t> keyframe{0, 0, 0, 2, 0x65, 0x88};
  const std::vector<std::uint8_t> expected{0, 0,    0,    1, 0x67, 0x64, 0, 0,    0,
                                           1, 0x68, 0xee, 0, 0,    0,    1, 0x65, 0x88};
  if (converter.access_unit(keyframe) != expected)
    throw std::runtime_error("keyframe did not include parameters");
  const std::vector<std::uint8_t> delta{0, 0, 0, 2, 0x41, 0x9a};
  const std::vector<std::uint8_t> delta_expected{0, 0, 0, 1, 0x41, 0x9a};
  if (converter.access_unit(delta) != delta_expected)
    throw std::runtime_error("delta NAL conversion failed");
  auto invalid = delta;
  invalid[3] = 10;
  bool rejected = false;
  try {
    (void)converter.access_unit(invalid);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  if (!rejected) throw std::runtime_error("invalid length was accepted");
}
