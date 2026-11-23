// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/ultra_ui_envelope.hpp"

#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using V = headunit::OpackValue;
void check(bool value) {
  if (!value) throw std::runtime_error("UI envelope test failed");
}
template <class F>
void rejects(F&& action) {
  bool caught = false;
  try {
    action();
  } catch (const std::invalid_argument&) {
    caught = true;
  }
  check(caught);
}
}  // namespace
int main() try {
  const std::array<std::string, 2> ids{"abcd0000-0000-4000-8000-000000000001",
                                       "abcd0000-0000-4000-8000-000000000002"};
  const V::Bytes data{'b', 'p', 'l', 'i', 's', 't', '0', '0', 0x80, 0, 0xff};
  for (const auto& id : ids) {
    const auto wire = headunit::encode_ultra_ui_envelope({id, data});
    const auto message = headunit::decode_ultra_ui_envelope(wire, ids);
    check(message.display_uuid == id && message.binary_plist == data);
  }
  auto uppercase = ids[0];
  uppercase.replace(0, 4, "ABCD");
  check(
      headunit::decode_ultra_ui_envelope(headunit::encode_ultra_ui_envelope({uppercase, data}), ids)
          .display_uuid == ids[0]);
  const auto unknown =
      headunit::encode_ultra_ui_envelope({"abcd0000-0000-4000-8000-000000000003", data});
  rejects([&] { (void)headunit::decode_ultra_ui_envelope(unknown, ids); });
  const std::array<std::string, 2> panels{"Center_Display", "Cluster_Display"};
  for (const auto& panel : panels)
    check(headunit::decode_ultra_ui_envelope(headunit::encode_ultra_ui_envelope({panel, data}),
                                             panels)
              .display_uuid == panel);
  rejects([&] {
    (void)headunit::decode_ultra_ui_envelope(
        headunit::encode_ultra_ui_envelope({"center_Display", data}), panels);
  });
  rejects([&] {
    (void)headunit::decode_ultra_ui_envelope(headunit::encode_ultra_ui_envelope({"unknown", data}),
                                             panels);
  });
  rejects([&] { (void)headunit::encode_ultra_ui_envelope({"bad/id", data}); });
  rejects([&] { (void)headunit::encode_ultra_ui_envelope({std::string(129, 'a'), data}); });
  rejects([&] { (void)headunit::encode_ultra_ui_envelope({ids[0], V::Bytes{'a'}}); });
  V::Dictionary root{{"displayUUID", V{ids[0]}}, {"data", V{data}}};
  root.emplace("unexpected", V{true});
  rejects([&] { (void)headunit::decode_ultra_ui_envelope(headunit::encode_opack(V{root}), ids); });
  root.erase("unexpected");
  root["data"] = V{std::string("bplist00")};
  rejects([&] { (void)headunit::decode_ultra_ui_envelope(headunit::encode_opack(V{root}), ids); });
  V::Bytes excessive(512 * 1024 + 1);
  std::copy(data.begin(), data.end(), excessive.begin());
  rejects([&] { (void)headunit::encode_ultra_ui_envelope({ids[0], excessive}); });
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
