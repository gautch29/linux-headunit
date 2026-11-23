// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace headunit {

// Converts the phone's AVC configuration and length-prefixed access units to
// the byte-stream format accepted by Linux H.264 decoders.
class H264AnnexB {
 public:
  void set_configuration(std::span<const std::uint8_t> avcc);
  std::vector<std::uint8_t> access_unit(std::span<const std::uint8_t> length_prefixed) const;
  bool configured() const { return !parameter_sets_.empty(); }

 private:
  unsigned nal_length_size_ = 0;
  std::vector<std::uint8_t> parameter_sets_;
};

}  // namespace headunit
