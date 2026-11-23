// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "bench_video.hpp"
#include <headunit/hap_crypto.hpp>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace headunit;
using namespace headunit::theme;
using Bytes = std::vector<std::uint8_t>;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
template <class F>
void rejects(F fn) {
  bool bad = false;
  try {
    fn();
  } catch (const std::exception&) {
    bad = true;
  }
  CHECK(bad);
}
Bytes config() {
  Bytes hevc(23);
  hevc[0] = 1;
  hevc[13] = 0xf0;
  hevc[15] = hevc[16] = 0xfc;
  hevc[17] = hevc[18] = 0xf8;
  hevc[21] = 3;
  hevc[22] = 3;
  for (auto type : {32, 33, 34})
    hevc.insert(hevc.end(), {std::uint8_t(0x80 | type), 0, 1, 0, 2, std::uint8_t(type << 1), 1});
  return hevc;  // Framing fixture, not decodable video; actual decoding has CM5 tests.
}
void packet(Bytes& out, unsigned kind, const Bytes& body, unsigned sequence = 0,
            unsigned flags = 2) {
  Bytes header(128);
  const auto size = body.size() + (kind == 0 ? 16 : 0);
  for (unsigned i = 0; i < 4; ++i)
    header[i] = size >> (i * 8);
  header[4] = kind;
  header[6] = flags;
  const std::uint64_t stamp = (std::uint64_t{900} << 32) + sequence;
  for (unsigned i = 0; i < 8; ++i)
    header[8 + i] = stamp >> (i * 8);
  Bytes payload = body;
  if (kind == 0) {
    HapKey32 key;
    key.fill(0x31);
    std::string nonce(8, '\0');
    for (unsigned i = 0; i < 8; ++i)
      nonce[i] = std::uint64_t(sequence) >> (i * 8);
    payload = hap_encrypt(key, nonce, body, header);
  }
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), payload.begin(), payload.end());
}
Bytes fixture(unsigned frames = 2, unsigned flags = 2) {
  Bytes bytes(32, 0x31);
  packet(bytes, 1, config(), 0, flags);
  for (unsigned i = 0; i < frames; ++i)
    packet(bytes, 0, Bytes{1, 2, 3}, i);
  return bytes;
}
int main() try {
  const auto directory =
      std::filesystem::temp_directory_path() / ("headunit-video-bench-" + std::to_string(getpid()));
  CHECK(std::filesystem::create_directory(directory));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  const auto path = (directory / "synthetic.fixture").string();
  auto save = [&](const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    CHECK(bool(file));
  };
  save(fixture());
  auto loaded = bench::VideoFixture::load(path.c_str());
  CHECK(loaded.frames.size() == 2 && loaded.frames[0].data == (Bytes{1, 2, 3}));
  auto bad = fixture();
  bad.back() ^= 1;
  save(bad);
  rejects([&] { bench::VideoFixture::load(path.c_str()); });
  bad = fixture();
  bad.pop_back();
  save(bad);
  rejects([&] { bench::VideoFixture::load(path.c_str()); });
  bad = fixture();
  bad[0] = 0;
  save(bad);
  rejects([&] { bench::VideoFixture::load(path.c_str()); });
  for (auto bytes : {fixture(1), fixture(121), fixture(2, 0), Bytes(31, 0x31)}) {
    save(bytes);
    rejects([&] { bench::VideoFixture::load(path.c_str()); });
  }
  bad = Bytes(32, 0x31);
  packet(bad, 1, config());
  packet(bad, 1, config());
  packet(bad, 0, Bytes{1}, 0);
  save(bad);
  rejects([&] { bench::VideoFixture::load(path.c_str()); });
  // A large valid packet split across the reader's 64 KiB chunks is accepted.
  bad = Bytes(32, 0x31);
  packet(bad, 1, config());
  packet(bad, 0, Bytes(65480, 0x41), 0);
  packet(bad, 0, Bytes{2}, 1);
  save(bad);
  CHECK(bench::VideoFixture::load(path.c_str()).frames[0].data.size() == 65480);
  save(fixture());
  bench::VideoReplay replay({path.c_str(), path.c_str()});
  const std::uint64_t origin = 1000000000;
  for (unsigned i = 0; i < video::NativeInputQueue::max_packets + 2; ++i)
    replay.pump(origin + i * bench::VideoReplay::frame_period_ns, origin);
  for (unsigned i = 0; i < 2; ++i) {
    CHECK(replay.sent[i] ==
          video::NativeInputQueue::max_packets - 1);  // One configuration also occupies ingress.
    CHECK(replay.backpressure[i] >= 2);
  }
  const auto sent = replay.sent;
  replay.pump(origin + 3000000000, origin);
  CHECK(replay.sent == sent);
  replay.queues[0]->close();
  rejects([&] { replay.pump(origin + 4000000000, origin); });
  std::cout << "video bench: authenticated framing, truncation, marker/timestamps, chunk "
               "boundaries and bounded backpressure passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
