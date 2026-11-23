// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/screen_configuration.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace headunit::theme;
using namespace headunit::theme::video;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
}
Bytes box(const char* name, Bytes body, bool extended = false) {
  auto n = std::uint32_t(body.size() + (extended ? 16 : 8));
  Bytes out;
  auto put = [&](std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
      out.push_back(value >> shift);
  };
  put(extended ? 1 : n);
  out.insert(out.end(), name, name + 4);
  if (extended) {
    put(0);
    put(n);
  }
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
int main() {
  try {
    Bytes avc{1, 66, 0, 30, 0xff, 0xe1, 0, 4, 0x67, 66, 0, 30, 1, 0, 2, 0x68, 0x80};
    Bytes hevc(23);
    hevc[0] = 1;
    hevc[13] = 0xf0;
    hevc[15] = hevc[16] = 0xfc;
    hevc[17] = hevc[18] = 0xf8;
    hevc[21] = 3;
    hevc[22] = 3;
    for (auto type : {32, 33, 34})
      hevc.insert(hevc.end(), {std::uint8_t(0x80 | type), 0, 1, 0, 2, std::uint8_t(type << 1), 1});
    for (auto codec : {Codec::h264, Codec::h265}) {
      const auto& record = codec == Codec::h264 ? avc : hevc;
      const auto name = codec == Codec::h264 ? "avcC" : "hvcC";
      CHECK(screen_configuration(record) == (Configuration{codec, record}));
      for (bool extended : {false, true})
        CHECK(screen_configuration(box(name, record, extended)) == (Configuration{codec, record}));
      auto zero = box(name, record);
      std::fill_n(zero.begin(), 4, 0);
      CHECK(screen_configuration(zero).codec_data == record);
      auto entry = Bytes(78);
      auto config = box(name, record);
      entry.insert(entry.end(), config.begin(), config.end());
      auto extra = box("pasp", Bytes(8));
      entry.insert(entry.end(), extra.begin(), extra.end());
      CHECK(screen_configuration(box(codec == Codec::h264 ? "avc1" : "hvc1", entry)).codec_data ==
            record);
      const auto entry_name = codec == Codec::h264 ? "avc1" : "hvc1";
      auto terminated = entry;
      terminated.insert(terminated.end(), 4, 0);
      CHECK(screen_configuration(box(entry_name, terminated)) == (Configuration{codec, record}));
      terminated.back() = 1;
      rejects([&] { screen_configuration(box(entry_name, terminated)); });
      for (auto padding : {1, 2, 3, 5, 6, 7, 8}) {
        auto malformed = entry;
        malformed.insert(malformed.end(), padding, 0);
        rejects([&] { screen_configuration(box(entry_name, malformed)); });
      }
      auto outside = config;
      outside.insert(outside.end(), 4, 0);
      rejects([&] { screen_configuration(outside); });
      auto empty_entry = Bytes(82);
      rejects([&] { screen_configuration(box(entry_name, empty_entry)); });
      rejects([&] { screen_configuration(box(codec == Codec::h264 ? "hvc1" : "avc1", entry)); });
      auto duplicate = config;
      duplicate.insert(duplicate.end(), config.begin(), config.end());
      rejects([&] { screen_configuration(duplicate); });
      for (std::size_t n = 0; n < record.size(); ++n)
        rejects([&] { screen_configuration(std::span(record).first(n)); });
      config[3] += 1;
      rejects([&] { screen_configuration(config); });
    }
    auto extended = avc;
    extended[1] = 100;
    extended.insert(extended.end(), {0xfc, 0xf8, 0xf8, 0});
    CHECK(screen_configuration(extended).codec == Codec::h264);
    auto bad = avc;
    bad[14] = 255;
    rejects([&] { screen_configuration(bad); });
    bad = hevc;
    bad[28] = 0;
    rejects([&] { screen_configuration(bad); });
    bad = hevc;
    bad[21] = 2;
    rejects([&] { screen_configuration(bad); });
    rejects([&] { screen_configuration(Bytes(131073)); });
    rejects([&] { screen_configuration(box("junk", avc)); });
    std::cout << "screen configuration: bare AVC/HEVC, boxes, sample entries, lengths and "
                 "parameter framing passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
