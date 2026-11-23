// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/camera_visibility.hpp>
#include <array>
#include <iostream>
#include <stdexcept>

namespace v = headunit::theme::vehicle;
namespace o = headunit::theme::opack;
namespace {
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
void rejects(auto action) {
  try {
    action();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid camera visibility binding or timestamp accepted");
}
constexpr std::uint64_t ms(std::uint64_t value) {
  return value * 1000000;
}
o::Value text(const char* value) {
  return o::Value{std::string(value)};
}
o::Value field(std::int64_t iid) {
  return o::Value{
      o::Dictionary{{text("iid"), o::Value{iid}}, {text("supportsInvalid"), o::Value{true}}}};
}
v::LocalProfile profile(v::ValueKind on_kind = v::ValueKind::boolean, bool fixed_uid = true,
                        bool fixed_on = false, std::uint64_t ttl = 500, std::string uid = "camera",
                        bool second = false) {
  o::Array characteristics{field(10), field(11)};
  std::vector<v::SignalPolicy> policies{{10, v::ValueKind::string, 500, {}, {}},
                                        {11, on_kind, ttl, {}, {}}};
  std::map<std::int64_t, o::Value> fixed;
  if (fixed_uid) fixed.emplace(10, o::Value{std::move(uid)});
  if (fixed_on) fixed.emplace(11, o::Value{false});
  std::vector<v::ScriptBinding> bindings{
      {10, {"automakerInputStreams", "criticalInputStream", "inputStreamUID"}, {}, "rear"},
      {11, {"automakerInputStreams", "criticalInputStream", "on"}, {}, "rear"}};
  if (second) {
    characteristics.push_back(field(12));
    characteristics.push_back(field(13));
    policies.push_back({12, v::ValueKind::string, 500, {}, {}});
    policies.push_back({13, v::ValueKind::boolean, ttl, {}, {}});
    fixed.emplace(12, o::Value{std::string("camera")});
    bindings.push_back(
        {12, {"automakerInputStreams", "criticalInputStream", "inputStreamUID"}, {}, "front"});
    bindings.push_back({13, {"automakerInputStreams", "criticalInputStream", "on"}, {}, "front"});
  }
  o::Value service{o::Dictionary{{text("iid"), o::Value{std::int64_t(2)}},
                                 {text("characteristics"), o::Value{std::move(characteristics)}}}};
  o::Value accessory{o::Dictionary{{text("iid"), o::Value{std::int64_t(1)}},
                                   {text("services"), o::Value{o::Array{std::move(service)}}}}};
  auto source = std::make_shared<v::ReadOnlyService>(
      o::Value{o::Dictionary{{text("accessories"), o::Value{o::Array{std::move(accessory)}}}}},
      std::move(policies), std::move(fixed));
  return {std::move(source), std::move(bindings), {}};
}
o::Value read(v::LocalProfile& p, std::uint64_t now_ms) {
  const std::array<std::int64_t, 1> ids{11};
  const auto reading = p.source->snapshot(ids, now_ms).at(11);
  check(reading.value.has_value(), "visibility missing from source");
  return *reading.value;
}
}  // namespace
int main() {
  try {
    {
      auto p = profile();
      v::CameraVisibility visibility(p, "camera", 150, ms(10));
      for (unsigned at = 11; at <= 80; ++at) {
        visibility.withdraw(ms(at));
        check(read(p, at) == o::Value{false}, "waiting input became visible");
      }
      check(visibility.refresh(true, ms(30), ms(82)) && read(p, 82) == o::Value{true},
            "negative visibility polls starved delayed USB capture");
      visibility.withdraw(ms(90));
      for (unsigned at = 91; at <= 140; ++at)
        visibility.withdraw(ms(at));
      check(!visibility.refresh(true, ms(89), ms(141)),
            "pre-Off capture bypassed transition cutoff");
      check(visibility.refresh(true, ms(110), ms(145)), "delayed post-Off input did not recover");
    }
    {
      auto p = profile();
      v::CameraVisibility visibility(p, "camera", 150, ms(10));
      check(visibility.on_iid() == 11 && !visibility.visible() && read(p, 10) == o::Value{false},
            "startup camera is not explicitly off");
      check(visibility.refresh(true, ms(20) + 500000, ms(30)) && visibility.visible(),
            "fresh capture did not become visible");
      check(!visibility.refresh(true, ms(20) + 500000, ms(100)),
            "polling renewed an old true capture");
      check(read(p, 100) == o::Value{true}, "fresh visible source changed on repeated poll");
      check(!visibility.refresh(true, ms(20) + 500000, ms(170) + 500000) && visibility.visible(),
            "inclusive camera age limit changed");
      check(!visibility.refresh(true, ms(20) + 500000, ms(170) + 500001) && !visibility.visible(),
            "stale frame was not withdrawn");
      check(read(p, 171) == o::Value{false}, "stale source became a truthy invalid sentinel");
      check(visibility.refresh(true, ms(181), ms(190)),
            "new source did not recover after stale withdrawal");
      visibility.withdraw(ms(190));
      check(!visibility.refresh(true, ms(181), ms(191)) && read(p, 191) == o::Value{false},
            "retired frame was reactivated");
      check(visibility.refresh(true, ms(192), ms(193)),
            "new capture did not recover after manual exit");
    }
    {
      auto p = profile();
      v::CameraVisibility visibility(p, "camera", 150, ms(10));
      check(visibility.refresh(true, ms(20) + 750000, ms(20) + 900000),
            "same-ms test capture not visible");
      visibility.withdraw(ms(20) + 950000);
      check(!visibility.visible() && read(p, 20) == o::Value{false},
            "same-ms OFF was delayed or invalidated");
      check(!visibility.refresh(true, ms(20) + 750000, ms(21)), "OFF revived a repeated capture");
    }
    {
      auto p = profile();
      v::CameraVisibility visibility(p, "camera", 150, ms(10) + 100000);
      check(!visibility.refresh(true, ms(10) + 900000, ms(11)) && read(p, 11) == o::Value{false},
            "startup capture timestamp was fabricated");
      check(visibility.refresh(true, ms(12), ms(12)), "genuinely newer capture did not publish");
      check(!visibility.refresh(true, ms(14), ms(13)) && read(p, 13) == o::Value{false},
            "future capture remained visible");
      check(visibility.refresh(true, ms(15), ms(15)),
            "future rejection prevented later valid recovery");
      check(!visibility.refresh(true, ms(14), ms(16)) && read(p, 16) == o::Value{false},
            "reversed capture remained visible");
      rejects([&] { visibility.withdraw(ms(15)); });
      rejects([&] { visibility.refresh(true, ms(16), ms(15)); });
    }
    {
      auto p = profile(v::ValueKind::boolean, true, false, 151);
      v::CameraVisibility visibility(p, "camera", 150, ms(10));
      check(visibility.refresh(true, ms(20) + 500000, ms(21)),
            "fresh capture missing with minimum safe TTL");
      check(!visibility.refresh(true, ms(20) + 500000, ms(170) + 500000) &&
                read(p, 170) == o::Value{true},
            "visibility expired at the inclusive freshness edge");
      visibility.refresh(true, ms(20) + 500000, ms(171));
      check(read(p, 171) == o::Value{false}, "source expiry exposed invalid visibility");
    }
    {
      auto p = profile(v::ValueKind::boolean, true, false, 151);
      v::CameraVisibility visibility(p, "camera", 150, ms(10));
      visibility.refresh(true, ms(20), ms(21));
      visibility.refresh(true, ms(20), ms(150));
      // Deliberately bypass refresh after this read: the underlying service must
      // still age from capture 20ms, not the repeated owner's 150ms poll.
      check(read(p, 171) == o::Value{}, "polling relabeled a true capture as a new observation");
    }
    for (const auto kind : {v::ValueKind::integer, v::ValueKind::real, v::ValueKind::string}) {
      auto p = profile(kind);
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile(v::ValueKind::boolean, false);
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile(v::ValueKind::boolean, true, true);
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile(v::ValueKind::boolean, true, false, 150);
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile(v::ValueKind::boolean, true, false, 500, "other-camera");
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile(v::ValueKind::boolean, true, false, 500, "camera", true);
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    for (unsigned field = 0; field < 2; ++field) {
      auto p = profile();
      p.bindings[field].instance.reset();
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile();
      p.bindings[1].instance = "other";
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    for (unsigned field = 0; field < 2; ++field) {
      auto p = profile();
      p.bindings.push_back(p.bindings[field]);
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    for (unsigned field = 0; field < 2; ++field) {
      auto p = profile();
      p.bindings[field].path[2] = "unrelated";
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile();
      auto duplicate = p.bindings[1];
      duplicate.instance = "front";
      p.bindings.push_back(std::move(duplicate));
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile();
      p.vehicle_inputs.push_back({11, "headlights", v::ValueKind::boolean, 1, 0, {}, {}, {}});
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    {
      auto p = profile();
      p.bindings.clear();
      rejects([&] { v::CameraVisibility(p, "camera", 150, ms(10)); });
    }
    for (const auto age : {0ULL, 1001ULL}) {
      auto p = profile();
      rejects([&] { v::CameraVisibility(p, "camera", age, ms(10)); });
    }
    std::cout << "Camera visibility: exact repeated bindings, real capture freshness, immediate "
                 "OFF and recovery passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
