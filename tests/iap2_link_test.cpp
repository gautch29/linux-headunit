// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/iap2_link.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() try {
  using headunit::encode_link_frame;
  using headunit::LinkFrame;
  using headunit::LinkFrameReader;

  const LinkFrame ack{0x40, 7, 6, 0, {}};
  const LinkFrame data{0x80, 8, 7, 1, {0x01, 0xff, 0x5a, 0x02}};
  const auto ack_bytes = encode_link_frame(ack);
  const auto data_bytes = encode_link_frame(data);
  check(ack_bytes.size() == 9, "empty acknowledgement must have a nine-byte header");
  check(data_bytes.size() == 14, "payload frame length is incorrect");
  check(ack_bytes[0] == 0xff && ack_bytes[1] == 0x5a, "wire marker is incorrect");
  check((std::accumulate(ack_bytes.begin(), ack_bytes.end(), 0u) & 0xffu) == 0,
        "acknowledgement checksum is invalid");
  check((std::accumulate(data_bytes.begin(), data_bytes.begin() + 9, 0u) & 0xffu) == 0,
        "data header checksum is invalid");
  check((std::accumulate(data_bytes.begin() + 9, data_bytes.end(), 0u) & 0xffu) == 0,
        "data payload checksum is invalid");

  // A header-only acknowledgement observed on the Pi's RFCOMM link. It has
  // no application payload, identity, or authentication material.
  constexpr std::uint8_t observed_ack[]{0xff, 0x5a, 0x00, 0x09, 0x40, 0x63, 0x87, 0x00, 0x74};
  LinkFrameReader observed;
  check(observed.feed(observed_ack) == std::vector<LinkFrame>({LinkFrame{0x40, 0x63, 0x87, 0, {}}}),
        "observed acknowledgement did not decode");
  check(encode_link_frame(LinkFrame{0x40, 0x63, 0x87, 0, {}}) ==
            std::vector<std::uint8_t>(std::begin(observed_ack), std::end(observed_ack)),
        "encoder did not match observed acknowledgement");

  for (std::size_t split = 0; split <= data_bytes.size(); ++split) {
    LinkFrameReader reader;
    const auto first = reader.feed(std::span(data_bytes).first(split));
    const auto second = reader.feed(std::span(data_bytes).subspan(split));
    check(first.size() + second.size() == 1, "split frame was lost");
    check((first.empty() ? second[0] : first[0]) == data, "split frame was decoded incorrectly");
    check(reader.buffered_bytes() == 0, "reader retained a complete frame");
  }

  LinkFrameReader joined;
  check(joined.feed({}).empty(), "empty link input produced a frame");
  std::vector<std::uint8_t> stream{0x12, 0x34, 0xff, 0x00};
  stream.insert(stream.end(), ack_bytes.begin(), ack_bytes.end());
  stream.insert(stream.end(), data_bytes.begin(), data_bytes.end());
  check(joined.feed(stream) == std::vector<LinkFrame>({ack, data}),
        "joined frames or noise were mishandled");
  LinkFrameReader quiet;
  check(quiet.feed(std::span(data_bytes).first(7)).empty(), "partial link header produced data");
  check(quiet.feed({}).empty() && quiet.buffered_bytes() == 7,
        "empty input changed a retained link header");
  check(quiet.feed(std::span(data_bytes).subspan(7)) == std::vector<LinkFrame>{data},
        "empty input interrupted later link completion");

  auto bad_header = data_bytes;
  bad_header[8] ^= 1;
  bad_header.insert(bad_header.end(), ack_bytes.begin(), ack_bytes.end());
  LinkFrameReader after_bad_header;
  check(after_bad_header.feed(bad_header) == std::vector<LinkFrame>({ack}),
        "bad header was accepted or prevented resynchronization");

  auto bad_payload = data_bytes;
  bad_payload.back() ^= 1;
  bad_payload.insert(bad_payload.end(), ack_bytes.begin(), ack_bytes.end());
  LinkFrameReader after_bad_payload;
  check(after_bad_payload.feed(bad_payload) == std::vector<LinkFrame>({ack}),
        "bad payload was accepted or prevented resynchronization");

  LinkFrameReader bounded(14);
  std::vector<std::uint8_t> overlong = data_bytes;
  overlong[3] = 15;
  overlong[8] = 0;
  for (std::size_t index = 0; index < 8; ++index)
    overlong[8] -= overlong[index];
  overlong.insert(overlong.end(), ack_bytes.begin(), ack_bytes.end());
  check(bounded.feed(overlong) == std::vector<LinkFrame>({ack}),
        "overlong header stalled the reader");

  LinkFrame maximum{0x40, 9, 8, 1, std::vector<std::uint8_t>(65525, 0x42)};
  for (std::size_t at = 0; at + 1 < maximum.payload.size(); at += 257) {
    maximum.payload[at] = 0xff;
    maximum.payload[at + 1] = 0x5a;
  }
  const auto maximum_wire = encode_link_frame(maximum);
  check(maximum_wire.size() == 65535, "maximum link frame changed size");
  for (const std::size_t chunk : {1u, 9u, 4096u, 65535u}) {
    LinkFrameReader reader;
    std::vector<LinkFrame> received;
    for (std::size_t at = 0; at < maximum_wire.size(); at += chunk) {
      auto next = reader.feed(
          std::span(maximum_wire).subspan(at, std::min(chunk, maximum_wire.size() - at)));
      received.insert(received.end(), next.begin(), next.end());
      check(reader.buffered_bytes() <= 65535, "fragmented link frame exceeded storage bound");
    }
    check(received == std::vector<LinkFrame>{maximum} && reader.buffered_bytes() == 0,
          "maximum frame with embedded markers was changed or retained");
  }
  auto damaged = maximum_wire;
  damaged.back() ^= 1;
  damaged.insert(damaged.end(), ack_bytes.begin(), ack_bytes.end());
  LinkFrameReader resynchronized;
  check(resynchronized.feed(damaged) == std::vector<LinkFrame>{ack},
        "large damaged payload prevented following acknowledgement");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
