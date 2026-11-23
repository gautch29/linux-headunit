// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/video_decoder.hpp>
#include <chrono>
#include <iostream>
#include <thread>

using namespace headunit::theme;
namespace v = video;
int main(int argc, char** argv) {
  try {
    std::array<std::uint8_t, 32> key{};
    std::cin.read(reinterpret_cast<char*>(key.data()), key.size());
    if (!std::cin) throw std::runtime_error("missing synthetic key");
    headunit::AirplayVideoStream screen(key);
    std::unique_ptr<v::Decoder> decoder;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    auto drain = [&] {
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("decode probe deadline exceeded");
      while (auto frame = decoder->pull()) {
        std::uint64_t red = 0, green = 0, blue = 0;
        for (const auto pixel : frame->image.pixels) {
          red += (pixel >> 16) & 255;
          green += (pixel >> 8) & 255;
          blue += pixel & 255;
        }
        const auto count = frame->image.pixels.size();
        std::cout << "{\"ntp\":\"" << frame->sender_ntp << "\",\"ns\":\"" << frame->timestamp_ns
                  << "\",\"width\":" << frame->image.width << ",\"height\":" << frame->image.height
                  << ",\"rgb\":[" << red / count << ',' << green / count << ',' << blue / count
                  << "]}\n";
      }
    };
    std::array<std::uint8_t, 4096> input{};
    while (std::cin.read(reinterpret_cast<char*>(input.data()), input.size()) ||
           std::cin.gcount()) {
      auto bytes = std::span(input).first(std::cin.gcount());
      for (const auto& packet : screen.feed(bytes)) {
        const auto* message = &packet;
        {
          if (message->kind == headunit::AirplayVideoPacket::Kind::config) {
            if (decoder) throw std::runtime_error("probe requires one configuration");
            auto configuration = v::screen_configuration(message->data);
            v::DecoderOptions options;
            options.codec = configuration.codec;
            options.codec_data = std::move(configuration.codec_data);
            options.max_width = 1920;
            options.max_height = 1080;
            options.decoder = argc == 2                               ? argv[1]
                              : configuration.codec == v::Codec::h264 ? "avdec_h264"
                                                                      : "avdec_h265";
            decoder = std::make_unique<v::Decoder>(std::move(options));
            std::cerr << "decoder=" << decoder->decoder_name() << '\n';
          } else if (message->kind == headunit::AirplayVideoPacket::Kind::frame) {
            if (!decoder) throw std::runtime_error("no decoder configuration");
            while (!decoder->push(*message)) {
              drain();
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
          }
        }
      }
    }
    if (screen.partial() || !decoder) throw std::runtime_error("incomplete stream");
    decoder->finish();
    do {
      drain();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!decoder->drained());
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
