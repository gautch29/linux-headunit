// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/airplay_identity.hpp>
#include <headunit/iap2_parameter.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace headunit;
void check(bool ok, const char* text) {
  if (!ok) throw std::runtime_error(text);
}
int main() try {
  char path[] = "/tmp/headunit-device-identity-XXXXXX";
  const int fd = mkstemp(path);
  check(fd >= 0, "identity fixture creation");
  close(fd);
  struct Cleanup {
    const char* path;
    ~Cleanup() { unlink(path); }
  } cleanup{path};
  const std::string base =
      "name=My Headunit\nmodel=Prototype\ndeviceid=02:11:22:33:44:55\n"
      "features=0x44540380,0x61\nflags=0x4\nprotovers=1.1\nsrcvers=950.7.1\n"
      "pi=AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA\n"
      "pk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
      "main_display_id=BBBBBBBB-BBBB-4BBB-8BBB-BBBBBBBBBBBB\n"
      "cluster_display_id=CCCCCCCC-CCCC-4CCC-8CCC-CCCCCCCCCCCC\n";
  auto write = [&](const std::string& data) {
    std::ofstream out(path);
    out << data;
  };
  write(base);
  const auto legacy = load_airplay_identity(path);
  check(legacy.manufacturer == "Independent", "legacy identity defaults");
  write(base +
        "manufacturer=My Workshop\nserial=My "
        "Serial\nfirmware=2.3\nhardware=CM5\ntouchscreen_name=My Touch\n");
  const auto loaded = load_airplay_identity(path);
  check(loaded.pairing_id == legacy.pairing_id && loaded.public_key_hex == legacy.public_key_hex &&
            loaded.main_display_id == legacy.main_display_id,
        "pairing identity changed with labels");
  check(loaded.touchscreen_name == "My Touch", "touchscreen label not loaded");
  auto accessory = make_iap2_identity(loaded);
  accessory.bluetooth_address = {2, 0, 0, 0, 0, 1};
  const auto wire = decode_iap2_parameters(make_identification_reply(accessory).parameters);
  const std::string expected[]{"My Headunit", "Prototype", "My Workshop",
                               "My Serial",   "2.3",       "CM5"};
  for (unsigned i = 0; i < 6; ++i) {
    auto value = std::vector<std::uint8_t>(expected[i].begin(), expected[i].end());
    value.push_back(0);
    check(wire[i].identifier == i && wire[i].value == value,
          "configured iAP2 field missing on wire");
  }
  for (const auto& invalid :
       {std::string("typo=value\n"), std::string("manufacturer=\n"),
        std::string("name=Duplicate\n"), std::string("hardware=") + std::string(128, 'a') + "\n"}) {
    write(base + invalid);
    bool rejected = false;
    try {
      (void)load_airplay_identity(path);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    check(rejected, "malformed identity accepted");
  }
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
