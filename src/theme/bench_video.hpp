// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
// Phone-free diagnostics only. Synthetic encrypted fixtures are parsed by the
// real framer, then replayed at the native decoder queue boundary. This does
// not simulate wireless negotiation or receiver socket/backpressure behavior.
#include <headunit/theme/native_input.hpp>
#include <headunit/theme/screen_configuration.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace headunit::theme::bench {
struct VideoFixture {
  AirplayVideoPacket configuration;
  std::vector<AirplayVideoPacket> frames;
  static VideoFixture load(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 32 || file.tellg() > 16 * 1024 * 1024)
      throw std::invalid_argument("invalid synthetic video fixture size");
    std::vector<std::uint8_t> bytes(std::size_t(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
      throw std::runtime_error("cannot read synthetic video fixture");
    HapKey32 key;
    std::copy_n(bytes.begin(), key.size(), key.begin());
    // This key is an explicit fixture marker, never a captured phone secret.
    if (!std::all_of(key.begin(), key.end(), [](auto b) { return b == 0x31; }))
      throw std::invalid_argument("not a generated synthetic video fixture");
    AirplayVideoStream parser(key);
    std::vector<AirplayVideoPacket> packets;
    for (std::size_t at = 32; at < bytes.size();) {
      const auto size = std::min<std::size_t>(65536, bytes.size() - at);
      auto chunk = parser.feed(std::span(bytes).subspan(at, size));
      at += size;
      if (packets.size() + chunk.size() > 121)
        throw std::invalid_argument("too many synthetic video frames");
      for (auto& packet : chunk)
        packets.push_back(std::move(packet));
    }
    if (parser.partial() || packets.size() < 3 || packets.size() > 121 ||
        packets.front().kind != AirplayVideoPacket::Kind::config || packets.front().data.empty())
      throw std::invalid_argument("incomplete synthetic video fixture");
    if (video::screen_configuration(packets.front().data).codec != video::Codec::h265)
      throw std::invalid_argument("synthetic native-video benchmark requires HEVC");
    VideoFixture result;
    result.configuration = packets.front();
    for (std::size_t i = 1; i < packets.size(); ++i) {
      const auto& packet = packets[i];
      if (packet.kind != AirplayVideoPacket::Kind::frame || packet.data.empty() ||
          !packet.sender_ntp || packet.data.size() > video::NativeInputQueue::max_packet_bytes)
        throw std::invalid_argument("invalid synthetic video fixture frame");
      result.frames.push_back(packet);
    }
    return result;
  }
};
class VideoReplay {
 public:
  static constexpr std::uint64_t frame_period_ns = 16666667, lead_ns = 150000000;
  explicit VideoReplay(const std::array<const char*, 2>& paths) {
    for (unsigned i = 0; i < 2; ++i) {
      fixtures_[i] = VideoFixture::load(paths[i]);
      queues[i] = std::make_shared<video::NativeInputQueue>();
      auto configuration = fixtures_[i].configuration;
      if (!queues[i]->enqueue(configuration))
        throw std::runtime_error("cannot enqueue fixture configuration");
    }
  }
  void pump(std::uint64_t now, std::uint64_t clock_origin) {
    for (unsigned i = 0; i < 2; ++i) {
      if (!pending_[i]) {
        if (!next_[i]) next_[i] = now;
        if (now < next_[i]) continue;
        auto packet = fixtures_[i].frames[sent[i] % fixtures_[i].frames.size()];
        const auto elapsed = now + lead_ns - clock_origin;
        packet.timestamp =
            ((900 + elapsed / 1000000000) << 32) + ((elapsed % 1000000000) << 32) / 1000000000;
        packet.sender_ntp = packet.timestamp;
        pending_[i] = std::move(packet);
      }
      // Retry intact compressed packets when bounded ingress applies pressure.
      // Do not change their timestamps, fabricate receipts or burst to catch up.
      if (queues[i]->enqueue(*pending_[i])) {
        pending_[i].reset();
        ++sent[i];
        next_[i] = std::max(next_[i] + frame_period_ns, now + frame_period_ns / 2);
      } else
        ++backpressure[i];
    }
  }
  std::array<std::shared_ptr<video::NativeInputQueue>, 2> queues;
  std::array<std::uint64_t, 2> sent{}, backpressure{};

 private:
  std::array<VideoFixture, 2> fixtures_;
  std::array<std::optional<AirplayVideoPacket>, 2> pending_;
  std::array<std::uint64_t, 2> next_{};
};
}  // namespace headunit::theme::bench
