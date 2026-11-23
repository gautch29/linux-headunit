// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/iap2_message.hpp>

#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() try {
  using headunit::ControlMessage;
  using headunit::ControlMessageReader;
  using headunit::encode_control_message;

  // Header-only control message observed in the iPhone's RFCOMM session.
  constexpr std::uint8_t observed[]{0x40, 0x40, 0x00, 0x06, 0x1d, 0x00};
  ControlMessageReader wire;
  check(wire.feed(observed) == std::vector<ControlMessage>({{0x1d00, {}}}),
        "observed control message was not decoded");
  check(encode_control_message({0x1d00, {}}) ==
            std::vector<std::uint8_t>(std::begin(observed), std::end(observed)),
        "observed control header was not reproduced");

  const ControlMessage data{0x4301, {0x01, 0x02, 0x40, 0x40, 0xff}};
  const auto bytes = encode_control_message(data);
  for (std::size_t split = 0; split <= bytes.size(); ++split) {
    ControlMessageReader reader;
    const auto first = reader.feed(std::span(bytes).first(split));
    const auto second = reader.feed(std::span(bytes).subspan(split));
    check(first.size() + second.size() == 1, "split message was lost");
    check((first.empty() ? second[0] : first[0]) == data, "split message was decoded incorrectly");
  }

  std::vector<std::uint8_t> joined{0x01, 0x40, 0x00};
  joined.insert(joined.end(), observed, observed + sizeof(observed));
  joined.insert(joined.end(), bytes.begin(), bytes.end());
  ControlMessageReader reader;
  check(reader.feed(joined) == std::vector<ControlMessage>({{0x1d00, {}}, data}),
        "joined messages or garbage were mishandled");

  auto oversized = bytes;
  oversized[2] = 0x01;
  oversized[3] = 0x00;
  oversized.insert(oversized.end(), observed, observed + sizeof(observed));
  ControlMessageReader bounded(64);
  check(bounded.feed(oversized) == std::vector<ControlMessage>({{0x1d00, {}}}),
        "oversized message blocked a later valid message");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
