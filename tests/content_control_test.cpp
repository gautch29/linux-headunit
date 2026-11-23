// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/content_control.hpp>
#include <headunit/theme/vehicle_transport.hpp>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace v = headunit::theme::vehicle;
namespace op = headunit::theme::opack;
using op::Value;
#define CHECK(x)                                             \
  do {                                                       \
    if (!(x)) throw std::runtime_error("check failed: " #x); \
  } while (false)
void rejects(auto f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid operation accepted");
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    char name[] = "/tmp/headunit-content-XXXXXX";
    const auto p = ::mkdtemp(name);
    CHECK(p);
    path = p;
  }
  ~Directory() { std::filesystem::remove_all(path); }
};
std::string configuration(const std::string& path) {
  return R"({"version":1,"experimental":true,"sections":[],"settings":[],"cluster_content":{
    "pages":[{"id":"body","widget":"body","panel":"Cluster_Display","zone":"zone1"},
              {"id":"tyres","widget":"tyres","panel":"Cluster_Display","zone":"zone1"}],
    "request_file":")" +
         path + R"(",
    "seats":[{"id":"driver","layout":"seat_front_left","occupancy":{},"buckle":{}}],
    "trips":[{"id":"current","label":"Trip","average_speed":{"units":1032}}]}})";
}
std::int64_t iid(const v::Configuration& c, std::string_view key) {
  for (const auto& s : c.signals)
    if (s.key == key) return s.policy.iid;
  throw std::runtime_error("binding missing");
}
Value read(v::ConfiguredModel& m, std::int64_t id, std::uint64_t now) {
  return *m.service().snapshot(std::array<std::int64_t, 1>{id}, now).at(id).value;
}
v::Command write(v::ConfiguredModel& m, std::int64_t id, Value value, std::uint64_t now) {
  const auto bytes = m.service().handle(
      v::encode_outgoing(
          {"writeRequest", 1, Value{op::Dictionary{{Value{id}, std::move(value)}}}, {}, {}}, 0),
      now);
  CHECK(bytes);
  return v::decode_incoming(*bytes).command;
}
v::LocalProfile local_profile() {
  using P = headunit::PlistValue;
  P::Array signals;
  const auto signal = [&](std::int64_t id, const char* field, const char* kind,
                          std::optional<P> fixed = {}) {
    P::Dictionary d{{"iid", P{id}},
                    {"kind", P{std::string(kind)}},
                    {"maxAgeMs", P{std::int64_t(3000)}},
                    {"instance", P{std::string("zone1")}},
                    {"path", P{P::Array{P{std::string("automakerRequestContent")},
                                        P{std::string("requestContent")}, P{std::string(field)}}}}};
    if (fixed) d.emplace("fixedValue", std::move(*fixed));
    signals.push_back(P{std::move(d)});
  };
  signal(3, "on", "boolean");
  signal(4, "contentURL", "string");
  signal(5, "displayPanelIdentifier", "string", P{std::string("Cluster_Display")});
  signal(6, "displayZoneIdentifier", "string", P{std::string("zone1")});
  signal(7, "userDismissible", "boolean", P{true});
  return v::decode_local_profile(
      headunit::encode_binary_plist(P{P::Dictionary{{"schemaVersion", P{std::int64_t(1)}},
                                                    {"mode", P{std::string("local-ui-bindings")}},
                                                    {"signals", P{std::move(signals)}}}}));
}
void publish(const std::filesystem::path& path, std::string bytes, unsigned mode = 0600) {
  auto temporary = path.parent_path() / "replacement";
  {
    std::ofstream output(temporary);
    output << bytes;
  }
  CHECK(!::chmod(temporary.c_str(), mode));
  std::filesystem::rename(temporary, path);
}
std::string request(unsigned sequence, unsigned renewed, std::string id = "body",
                    std::string nonce = std::string(32, 'a'), unsigned generation = 7) {
  return "HUC1 " + nonce + " " + std::to_string(generation) + " " + std::to_string(sequence) + " " +
         std::to_string(renewed) + " " + std::to_string(renewed + 2000) + "\n" + id + "\n";
}
int main() try {
  Directory directory;
  const auto path = directory.path / "cluster-content.request";
  const auto c = v::parse_configuration("", configuration(path.string()));
  CHECK(c.content_request && c.content_pages.size() == 2);
  auto state = std::make_shared<v::ContentStateSnapshot>(*c.content_request);
  auto provider = v::content_request_provider(v::simulated_provider(c), *c.content_request,
                                              std::string(32, 'a'), 7, state);
  v::ConfiguredModel m(c, provider);
  m.refresh(1000);
  const auto on = iid(c, "content.request.on"), url = iid(c, "content.request.url");
  CHECK(read(m, on, 1000) == Value{false});
  CHECK(read(m, iid(c, "content.seat.driver.occupancy"), 1000) == Value{});
  // Missing numeric/bool/enum readings serialize without fake initial values.
  const auto info = v::add_server_info(
      headunit::encode_binary_plist(headunit::PlistValue{headunit::PlistValue::Dictionary{}}),
      m.service(), 1000);
  CHECK(!info.empty());
  const auto decoded = headunit::decode_binary_plist(info);
  bool unknown_seen = false;
  const auto visit = [&](const auto& self, const headunit::PlistValue& p) -> void {
    if (const auto* d = std::get_if<headunit::PlistValue::Dictionary>(&p.value)) {
      if (d->contains("iid") &&
          d->at("iid") == headunit::PlistValue{iid(c, "content.seat.driver.occupancy")}) {
        CHECK(!d->contains("initialValue"));
        unknown_seen = true;
      }
      for (const auto& [key, value] : *d) {
        (void)key;
        self(self, value);
      }
    } else if (const auto* a = std::get_if<headunit::PlistValue::Array>(&p.value))
      for (const auto& value : *a)
        self(self, value);
  };
  visit(visit, decoded);
  CHECK(unknown_seen);
  auto local = local_profile();
  v::ContentLocalInput input(local, state);
  input.pump(1000);
  v::ScriptView idle_view(local.source, local.bindings, input.dismissals());
  CHECK(idle_view.dismiss({{3, Value{false}}}, 1001).values.at(3) == Value{false});
  m.refresh(1001);
  CHECK(read(m, on, 1001) == Value{false});
  publish(path, request(1, 1100));
  m.refresh(1100);
  CHECK(read(m, on, 1100) == Value{false});  // Await the clean scene's acceptance.
  CHECK(read(m, url, 1100) == Value{c.content_pages[0].url});
  input.pump(1100);
  m.refresh(1100);
  CHECK(read(m, on, 1100) == Value{true});
  input.pump(1120, false);  // Startup/co-brand scene suppresses vehicle content.
  CHECK(local.source->snapshot(std::array<std::int64_t, 1>{3}, 1120).at(3).value == Value{false});
  m.refresh(1120);
  CHECK(read(m, on, 1120) == Value{false});
  input.pump(1150);  // An older intent cannot cross that OFF frontier.
  CHECK(local.source->snapshot(std::array<std::int64_t, 1>{3}, 1150).at(3).value == Value{false});
  m.refresh(1150);
  CHECK(read(m, on, 1150) == Value{false});
  publish(path, request(1, 1160));
  m.refresh(1200);
  input.pump(1200);
  CHECK(local.source->snapshot(std::array<std::int64_t, 1>{3}, 1200).at(3).value == Value{true});
  CHECK(write(m, on, Value{true}, 1200).errors);
  CHECK(write(m, url, Value{c.content_pages[1].url}, 1200).errors);
  CHECK(!write(m, on, Value{false}, 1250).errors);
  input.pump(1250);
  CHECK(local.source->snapshot(std::array<std::int64_t, 1>{3}, 1250).at(3).value == Value{false});
  publish(path,
          request(1, 1300));  // Phone dismissal retires the command, including later renewals.
  m.refresh(1300);
  CHECK(read(m, on, 1300) == Value{false});
  publish(path, request(2, 1400, "tyres"));
  m.refresh(1400);
  input.pump(1400);
  m.refresh(1400);
  CHECK(read(m, on, 1400) == Value{true});
  CHECK(read(m, url, 1400) == Value{c.content_pages[1].url});
  v::ScriptView view(local.source, local.bindings, input.dismissals());
  CHECK(
      view.dismiss({{3, Value{true}}, {4, Value{false}}, {99, Value{false}}}, 1450).values.empty());
  CHECK(view.dismiss({{3, Value{false}}}, 1450).values.at(3) == Value{false});
  CHECK(!state->read(1450).on);
  // A control-thread renewal racing the scene's dismissal cannot reopen it.
  state->update({true, c.content_pages[1].url, 1460, 3460, 2});
  CHECK(!state->read(1460, true).on);
  publish(path, request(2, 1500, "tyres"));
  m.refresh(1500);
  CHECK(read(m, on, 1500) == Value{false});
  publish(path, request(3, 1600, "tyres"));
  m.refresh(1600);
  input.pump(1600);
  m.refresh(1600);
  CHECK(read(m, on, 1600) == Value{true});
  input.pump(3600);  // Scene withdraws even if control owner stops polling.
  CHECK(local.source->snapshot(std::array<std::int64_t, 1>{3}, 3600).at(3).value == Value{false});
  m.refresh(3600);
  CHECK(read(m, on, 3600) == Value{false});
  publish(path, request(3, 3700, "tyres"));
  m.refresh(3700);
  CHECK(read(m, on, 3700) == Value{false});
  // Unsafe/missing files, malformed framing, bad leases and old publishers withdraw.
  unsigned sequence = 4, now = 3800;
  for (const auto& invalid :
       {request(3, 3600, "body", std::string(32, 'b')),
        request(3, 3600, "body", std::string(32, 'a'), 8), request(3, 3600, "undeclared"),
        request(3, 3600) + "extra\n", std::string(385, 'x'), request(3, 6000)}) {
    publish(path, invalid);
    m.refresh(now);
    CHECK(read(m, on, now) == Value{false});
    now += 100;
  }
  sequence = 5;
  publish(path, request(sequence, now), 0644);
  m.refresh(now);
  CHECK(read(m, on, now) == Value{false});
  now += 100;
  publish(path, request(++sequence, now));
  m.refresh(now);
  input.pump(now);
  m.refresh(now);
  CHECK(read(m, on, now) == Value{true});
  std::filesystem::remove(path);
  std::filesystem::create_symlink(directory.path / "replacement", path);
  now += 100;
  m.refresh(now);
  CHECK(read(m, on, now) == Value{false});
  std::filesystem::remove(path);
  publish(path, request(++sequence, now + 100, "-"));
  now += 100;
  m.refresh(now);
  CHECK(read(m, on, now) == Value{false});
  state->close(now);
  state->update({true, c.content_pages.front().url, now + 100, now + 2100});
  CHECK(!state->read(now + 100).on);
  for (const auto& bad : {"maps", "media", "trip", "camera"}) {
    auto raw = configuration(path.string());
    raw.replace(raw.find("\"widget\":\"body\""), 15, "\"widget\":\"" + std::string(bad) + "\"");
    rejects([&] { (void)v::parse_configuration("", raw); });
  }
  rejects([&] {
    (void)v::content_request_provider(v::simulated_provider(c), *c.content_request, "bad", 7);
  });
  rejects([&] { (void)v::ContentLocalInput(v::LocalProfile{}, state); });
  std::cout << "Content configuration, unavailable /info, lease, dismissal and scene withdrawal "
               "checks passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
