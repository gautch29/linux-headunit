// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/screen_configuration.hpp>
#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace headunit::theme::video {
namespace {
using View = std::span<const std::uint8_t>;
void check(bool ok, const char* why) {
  if (!ok) throw std::invalid_argument(why);
}
std::uint32_t be(View data) {
  std::uint32_t n = 0;
  for (auto b : data)
    n = (n << 8) | b;
  return n;
}
struct Reader {
  View data;
  std::size_t at = 0;
  View take(std::size_t n) {
    if (n > data.size() - at)
      throw std::invalid_argument("truncated video configuration at " + std::to_string(at) +
                                  " need " + std::to_string(n) + " have " +
                                  std::to_string(data.size() - at));
    auto v = data.subspan(at, n);
    at += n;
    return v;
  }
  unsigned byte() { return take(1)[0]; }
  void nal(unsigned type, bool hevc) {
    const auto n = be(take(2));
    auto bytes = take(n);
    check(bytes.size() >= (hevc ? 2U : 1U) && !(bytes[0] & 0x80), "invalid video parameter set");
    check((hevc ? ((bytes[0] >> 1) & 63) : (bytes[0] & 31)) == type,
          "mismatched video parameter type");
    if (hevc) check((bytes[1] & 7) != 0, "invalid HEVC temporal ID");
  }
};
void validate(View data, Codec codec) {
  check(!data.empty() && data.size() <= 65536 && data[0] == 1,
        "invalid video configuration version or size");
  Reader r{data};
  if (codec == Codec::h264) {
    auto head = r.take(6);
    check((head[4] & 0xfc) == 0xfc && (head[4] & 3) != 2 && (head[5] & 0xe0) == 0xe0,
          "invalid AVC configuration flags");
    const auto count = head[5] & 31;
    check(count > 0, "missing AVC sequence parameters");
    for (unsigned i = 0; i < unsigned(count); ++i)
      r.nal(7, false);
    const auto pps = r.byte();
    check(pps > 0, "missing AVC picture parameters");
    for (unsigned i = 0; i < pps; ++i)
      r.nal(8, false);
    if (r.at < data.size()) {
      // Optional high-profile chroma/bit-depth and SPS-extension section.
      const std::array<unsigned, 13> extended{100, 110, 122, 144, 44,  83, 86,
                                              118, 128, 138, 139, 134, 135};
      bool allowed = false;
      for (auto profile : extended)
        allowed |= head[1] == profile;
      check(allowed, "unexpected AVC configuration suffix");
      check((r.byte() & 0xfc) == 0xfc && (r.byte() & 0xf8) == 0xf8 && (r.byte() & 0xf8) == 0xf8,
            "invalid AVC extension flags");
      const auto extensions = r.byte();
      for (unsigned i = 0; i < extensions; ++i)
        r.nal(13, false);
    }
  } else {
    const auto head = r.take(23);
    check((head[13] & 0xf0) == 0xf0 && (head[15] & 0xfc) == 0xfc && (head[16] & 0xfc) == 0xfc &&
              (head[17] & 0xf8) == 0xf8 && (head[18] & 0xf8) == 0xf8 && (head[21] & 3) != 2,
          "invalid HEVC configuration flags");
    std::array<bool, 64> found{};
    for (unsigned i = 0; i < head[22]; ++i) {
      const auto flags = r.byte(), type = flags & 63;
      check(!(flags & 0x40) && !found[type], "invalid HEVC parameter array");
      check(type == 32 || type == 33 || type == 34 || type == 39 || type == 40,
            "unsupported HEVC configuration NAL type");
      const auto count = be(r.take(2));
      check(count > 0 && count <= 512, "invalid HEVC parameter count");
      for (unsigned j = 0; j < count; ++j)
        r.nal(type, true);
      found[type] = true;
    }
    check(found[32] && found[33] && found[34], "incomplete HEVC parameter sets");
  }
  check(r.at == data.size(), "unexpected video configuration suffix");
}
void boxes(View input, unsigned depth, std::optional<Configuration>& output, unsigned& count) {
  check(depth <= 2, "nested video configuration limit exceeded");
  Reader r{input};
  while (r.at < input.size()) {
    // QuickTime sample descriptions may end in exactly four zero bytes.
    // This is not an incomplete box header and applies only inside the entry,
    // never to arbitrary padding after a top-level codec record.
    if (depth > 0 && input.size() - r.at == 4 && be(input.subspan(r.at)) == 0) {
      r.take(4);
      break;
    }
    check(++count <= 32, "too many video configuration boxes");
    const auto head = r.take(8);
    auto size = std::size_t(be(head.first(4)));
    const std::string_view type(reinterpret_cast<const char*>(head.data() + 4), 4);
    std::size_t header = 8;
    if (size == 1) {
      auto extended = r.take(8);
      check(be(extended.first(4)) == 0, "oversized extended video box");
      size = be(extended.last(4));
      header = 16;
    } else if (size == 0)
      size = input.size() - (r.at - header);
    check(size >= header, "invalid video box length");
    auto body = r.take(size - header);
    if (type == "avcC" || type == "hvcC") {
      check(!output, "multiple video codec configurations");
      const auto codec = type == "avcC" ? Codec::h264 : Codec::h265;
      validate(body, codec);
      output = Configuration{codec, Bytes(body.begin(), body.end())};
    } else if (type == "avc1" || type == "avc3" || type == "hvc1" || type == "hev1") {
      check(body.size() >= 78, "truncated video sample entry");
      boxes(body.subspan(78), depth + 1, output, count);
      check(output && ((type == "avc1" || type == "avc3") == (output->codec == Codec::h264)),
            "sample entry codec mismatch");
    } else
      check(type == "pasp" || type == "colr" || type == "btrt" || type == "clap" || type == "fiel",
            "unsupported video configuration box");
  }
}
}  // namespace
Configuration screen_configuration(View input) {
  check(!input.empty() && input.size() <= 131072, "invalid screen configuration size");
  if (input[0] == 1) {
    std::optional<Configuration> output;
    for (auto codec : {Codec::h264, Codec::h265}) {
      bool valid = false;
      try {
        validate(input, codec);
        valid = true;
      } catch (const std::invalid_argument&) {
      }
      if (valid) {
        check(!output, "ambiguous bare video configuration");
        output = Configuration{codec, Bytes(input.begin(), input.end())};
      }
    }
    check(bool(output), "invalid bare video configuration");
    return std::move(*output);
  }
  std::optional<Configuration> output;
  unsigned count = 0;
  boxes(input, 0, output, count);
  check(bool(output), "screen configuration contains no codec record");
  return std::move(*output);
}
}  // namespace headunit::theme::video
