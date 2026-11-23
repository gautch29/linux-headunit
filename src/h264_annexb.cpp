// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/h264_annexb.hpp"

#include <iterator>
#include <stdexcept>
#include <utility>

namespace headunit {
namespace {
constexpr std::size_t max_access_unit = 8 * 1024 * 1024;
constexpr std::uint8_t start_code[]{0, 0, 0, 1};

void append_nal(std::vector<std::uint8_t>& output, std::span<const std::uint8_t> nal) {
  if (nal.empty() || output.size() > max_access_unit - 4 ||
      nal.size() > max_access_unit - output.size() - 4) {
    throw std::invalid_argument("invalid H.264 NAL size");
  }
  output.insert(output.end(), std::begin(start_code), std::end(start_code));
  output.insert(output.end(), nal.begin(), nal.end());
}
}  // namespace

void H264AnnexB::set_configuration(std::span<const std::uint8_t> avcc) {
  if (avcc.size() < 7 || avcc[0] != 1) {
    throw std::invalid_argument("invalid AVC configuration");
  }
  const unsigned length_size = (avcc[4] & 3) + 1;
  std::vector<std::uint8_t> parameters;
  std::size_t offset = 6;
  const auto read_group = [&](unsigned count) {
    for (unsigned index = 0; index < count; ++index) {
      if (avcc.size() - offset < 2) {
        throw std::invalid_argument("truncated AVC parameter length");
      }
      const auto length = (static_cast<unsigned>(avcc[offset]) << 8) | avcc[offset + 1];
      offset += 2;
      if (!length || avcc.size() - offset < length) {
        throw std::invalid_argument("invalid AVC parameter length");
      }
      append_nal(parameters, avcc.subspan(offset, length));
      offset += length;
    }
  };
  const unsigned sps_count = avcc[5] & 31;
  if (!sps_count) throw std::invalid_argument("AVC configuration lacks SPS");
  read_group(sps_count);
  if (offset >= avcc.size()) {
    throw std::invalid_argument("AVC configuration lacks PPS count");
  }
  const unsigned pps_count = avcc[offset++];
  if (!pps_count) throw std::invalid_argument("AVC configuration lacks PPS");
  read_group(pps_count);
  nal_length_size_ = length_size;
  parameter_sets_ = std::move(parameters);
}

std::vector<std::uint8_t> H264AnnexB::access_unit(std::span<const std::uint8_t> input) const {
  if (!configured() || input.empty() || input.size() > max_access_unit) {
    throw std::invalid_argument("invalid H.264 access unit");
  }
  std::vector<std::uint8_t> converted;
  converted.reserve(input.size() + 64);
  std::size_t offset = 0;
  bool keyframe = false;
  while (offset < input.size()) {
    if (input.size() - offset < nal_length_size_) {
      throw std::invalid_argument("truncated H.264 NAL length");
    }
    std::size_t length = 0;
    for (unsigned i = 0; i < nal_length_size_; ++i)
      length = (length << 8) | input[offset++];
    if (!length || input.size() - offset < length) {
      throw std::invalid_argument("invalid H.264 NAL length");
    }
    const auto nal = input.subspan(offset, length);
    if ((nal[0] & 31) == 5) keyframe = true;
    append_nal(converted, nal);
    offset += length;
  }
  if (keyframe) {
    if (parameter_sets_.size() > max_access_unit - converted.size())
      throw std::invalid_argument("H.264 access unit plus parameters exceeds limit");
    std::vector<std::uint8_t> with_parameters;
    with_parameters.reserve(parameter_sets_.size() + converted.size());
    with_parameters.insert(with_parameters.end(), parameter_sets_.begin(), parameter_sets_.end());
    with_parameters.insert(with_parameters.end(), converted.begin(), converted.end());
    return with_parameters;
  }
  return converted;
}

}  // namespace headunit
