// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/route_guidance.hpp>
#include <headunit/iap2_identity.hpp>
#include <iostream>
#include <stdexcept>
using namespace headunit;
using namespace std::chrono_literals;
namespace {
void check(bool v, const char* why) {
  if (!v) throw std::runtime_error(why);
}
std::vector<std::uint8_t> str(std::string s) {
  s.push_back(0);
  return {s.begin(), s.end()};
}
ControlMessage msg(std::uint16_t id, std::initializer_list<Iap2Parameter> p) {
  return {id, encode_iap2_parameters(p)};
}
ControlMessage route(std::uint8_t state = 1) {
  return msg(0x5201, {{0, {0, 42}},
                      {1, {state}},
                      {10, {0, 0, 1, 44}},
                      {13, {0, 7}},
                      {19, str("Apple Maps")},
                      {20, {1}}});
}
ControlMessage maneuver(unsigned index = 7) {
  return msg(0x5202, {{0, {0, 42}},
                      {1, {0, std::uint8_t(index)}},
                      {2, str("Turn right onto Rue des Lilas")},
                      {3, {2}},
                      {4, str("Rue des Lilas")}});
}
}  // namespace
int main() try {
  const auto now = Clock::time_point(10s);
  RouteGuidance nav({});
  check(!nav.snapshot(now).available, "empty must be unavailable");
  auto component = route_guidance_component({});
  check(component.identifier == 30, "component ID");
  const auto fields = decode_iap2_parameters(component.value);
  check(fields.size() == 9 && fields[8].value == std::vector<std::uint8_t>({0, 0}),
        "no unsupported lane capacity");
  check(start_route_guidance({}).identifier == 0x5200, "subscription ID");
  Iap2Identity identity;
  identity.bluetooth_address = {1, 2, 3, 4, 5, 6};
  identity.route_guidance = RouteGuidanceConfig{};
  bool identified = false, rx = false, tx = false;
  for (const auto& p : decode_iap2_parameters(make_identification_reply(identity).parameters)) {
    identified |= p.identifier == 30;
    if (p.identifier == 6) tx = p.value == std::vector<std::uint8_t>({0x52, 0, 0x52, 3});
    if (p.identifier == 7) rx = p.value == std::vector<std::uint8_t>({0x52, 1, 0x52, 2});
  }
  check(identified && rx && tx, "capability and message advertisement");
  check(nav.receive(maneuver(), now), "plan before route state");
  check(nav.receive(route(), now), "route update");
  auto snapshot = nav.snapshot(now);
  check(snapshot.available && snapshot.maneuver && snapshot.distance_m == 300,
        "correlated guidance");
  check(navigation_prompt(snapshot) == "Turn right · 300 m", "human prompt");
  check(nav.receive(msg(0x5201, {{0, {0, 42}}, {15, {0}}, {10, {0, 0, 0, 90}}}), now + 1s),
        "delta");
  check(nav.snapshot(now + 1s).distance_m == 90 && nav.snapshot(now + 1s).maneuver,
        "not visible in app does not stop route");
  auto broken = route();
  broken.parameters.push_back(0);
  check(!nav.receive(broken, now + 2s) && nav.snapshot(now + 2s).distance_m == 90,
        "truncated TLV must be atomic");
  check(!nav.receive(msg(0x5201, {{0, {0, 42}}, {10, {0, 0, 0, 3}}, {1, {1, 2}}}), now + 2s) &&
            nav.snapshot(now + 2s).distance_m == 90,
        "invalid width must not partially commit");
  check(!nav.receive(msg(0x5201, {{0, {0, 42}}, {1, {1}}, {1, {0}}}), now + 2s),
        "reject duplicate known fields");
  check(!nav.receive(msg(0x5201, {{0, {0, 43}}, {1, {0}}}), now + 2s), "wrong component ignored");
  check(!nav.receive(msg(0x5202, {{0, {0, 42}}, {1, {0, 7}}, {2, {0xc0, 0xaf}}}), now + 2s),
        "invalid UTF-8 rejected");
  check(nav.receive(msg(0x5201, {{0, {0, 42}}, {13, {0, 8}}}), now + 2s), "cursor change");
  check(!nav.snapshot(now + 2s).maneuver && !nav.snapshot(now + 2s).distance_m,
        "new cursor must not inherit old maneuver/distance");
  check(nav.receive(maneuver(8), now + 2s) && nav.snapshot(now + 2s).maneuver->index == 8,
        "late maneuver matches cursor");
  check(nav.receive(msg(0x5201, {{0, {0, 42}}, {1, {5}}}), now + 3s), "reroute");
  check(!nav.snapshot(now + 3s).maneuver &&
            navigation_prompt(nav.snapshot(now + 3s)) == "Updating route",
        "reroute clears directions");
  check(nav.receive(route(), now + 4s) && !nav.snapshot(now + 4s).maneuver,
        "reused index cannot revive old route");
  check(nav.receive(maneuver(), now + 4s), "fresh plan");
  check(nav.receive(msg(0x5201, {{0, {0, 42}}, {0x7777, {1, 2, 3}}}), now + 4s),
        "unknown extension accepted");
  check(nav.snapshot(now + 1h).available && nav.snapshot(now + 1h).distance_m == 300,
        "unchanged route survives silent connected session");
  check(nav.receive(maneuver(9), now + 10s) && nav.snapshot(now + 10s).maneuver->index == 7 &&
            nav.snapshot(now + 10s).distance_m == 300,
        "future plan does not change current position");
  check(nav.receive(route(0), now + 11s) && !nav.snapshot(now + 11s).available, "route stop");
  check(nav.receive(route(), now + 12s) && !nav.snapshot(now + 12s).maneuver,
        "new trip no old maneuver");
  check(nav.receive(maneuver(), now + 12s), "new trip plan");
  check(nav.receive(msg(0x5201, {{0, {0, 42}}, {20, {0}}}), now + 13s) &&
            !nav.snapshot(now + 13s).available,
        "unsupported source clears");
  nav.clear();
  check(!nav.snapshot(now + 14s).available, "disconnect clear");
  check(nav.receive(route(), now + 14s) && nav.receive(maneuver(), now + 14s),
        "restore test route");
  for (unsigned i = 0; i < 300; ++i) {
    check(nav.receive(
              msg(0x5202, {{0, {0, 42}}, {1, {std::uint8_t(i >> 8), std::uint8_t(i)}}, {3, {2}}}),
              now + 14s),
          "bounded plan cache");
  }
  check(nav.snapshot(now + 14s).maneuver->index == 7, "cache pressure retains current maneuver");
  check(nav.receive(msg(0x5201, {{0, {0, 42}}, {19, str("Other Maps")}}), now + 15s) &&
            !nav.snapshot(now + 15s).available,
        "new source must not inherit previous route state");
  check(nav.receive(route(), now + 16s) && nav.receive(maneuver(), now + 16s),
        "restore after source switch");
  check(nav.receive(msg(0x5201, {{0, {0, 42}}, {14, {0, 0}}}), now + 16s), "empty plan");
  check(!nav.snapshot(now + 16s).maneuver, "empty plan clears old maneuver");
  std::cout << "Navigation capability, delta correlation, validation, reroute, stop and "
               "silent-session persistence passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
