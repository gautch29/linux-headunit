// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_service.hpp>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace headunit::theme;
namespace v = headunit::theme::vehicle;
namespace op = headunit::theme::opack;
using op::Value;
Value str(std::string s) {
  return Value{std::move(s)};
}
Value num(std::int64_t i) {
  return Value{i};
}
Value dict(std::initializer_list<op::DictionaryEntry> pairs) {
  return Value{op::Dictionary(pairs)};
}
Value arr(std::initializer_list<Value> values) {
  return Value{op::Array(values)};
}
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
void rejects(auto fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid service input accepted");
}
Value profile() {
  return dict({{str("accessories"),
                arr({dict({{str("iid"), num(1)},
                           {str("type"), num(1)},
                           {str("services"),
                            arr({dict({{str("iid"), num(2)},
                                       {str("type"), num(2)},
                                       {str("characteristics"),
                                        arr({dict({{str("iid"), num(3)},
                                                   {str("type"), num(3)},
                                                   {str("format"), str("float")},
                                                   {str("mutable"), Value{true}},
                                                   {str("writable"), Value{false}},
                                                   {str("supportsInvalid"), Value{true}}}),
                                             dict({{str("iid"), num(4)},
                                                   {str("type"), num(4)},
                                                   {str("format"), str("int")},
                                                   {str("mutable"), Value{true}},
                                                   {str("writable"), Value{false}}})})}})})}})})}});
}
std::vector<v::SignalPolicy> policies() {
  return {{3, v::ValueKind::real, 100, 0.0, 200.0}, {4, v::ValueKind::integer, 200, 0.0, 10000.0}};
}
v::Command request(v::ReadOnlyService& service, std::string name, std::optional<Value> values,
                   std::uint64_t now, std::optional<std::int64_t> tx = 1) {
  const auto bytes = service.handle(v::encode_outgoing({name, tx, values, {}, {}}, 0), now);
  check(bytes.has_value(), "request has no response");
  return v::decode_incoming(*bytes).command;
}
Value readings(std::initializer_list<op::DictionaryEntry> values) {
  return dict(values);
}
Value& field(Value& object, std::string_view name) {
  for (auto& [key, value] : std::get<op::Dictionary>(object.data))
    if (std::get<std::string>(key.data) == name) return value;
  throw std::runtime_error("fixture field missing");
}
Value writable_profile() {
  auto result = profile();
  auto& accessories = std::get<op::Array>(field(result, "accessories").data);
  auto& services = std::get<op::Array>(field(accessories[0], "services").data);
  auto& characteristics = std::get<op::Array>(field(services[0], "characteristics").data);
  field(characteristics[0], "writable") = Value{true};
  return result;
}
v::Command fixture(std::string_view name) {
  v::ReadOnlyService s(profile(), policies());
  s.publish(3, Value{12.5}, 1000, 1000);
  s.publish(4, num(100), 1000, 1000);
  if (name == "config") return request(s, "configRequest", {}, 1000);
  if (name == "read") return request(s, "readRequest", arr({num(3), num(4), num(99)}), 1000);
  if (name == "register") return request(s, "registerRequest", str("*"), 1000);
  (void)request(s, "registerRequest", str("*"), 1000);
  if (name == "expired") return v::decode_incoming(*s.updates(1200)).command;
  if (name == "update") {
    s.publish(3, Value{20.0}, 1001, 1001);
    return v::decode_incoming(*s.updates(1001)).command;
  }
  if (name == "unregister") return request(s, "unregisterRequest", str("*"), 1000);
  throw std::invalid_argument("unknown fixture");
}

void sibling_update_groups() {
  // Without a group only the changed signal is notified.
  v::ReadOnlyService plain(profile(), policies());
  plain.publish_batch({{3, Value{12.5}, 1000}, {4, num(100), 1000}}, 1000);
  (void)request(plain, "registerRequest", str("*"), 1000);
  plain.publish_batch({{3, Value{20.0}, 1001}, {4, num(100), 1001}}, 1001);
  check(
      v::decode_incoming(*plain.updates(1001)).command.values == readings({{num(3), Value{20.0}}}),
      "ungrouped update resent an unchanged signal");
  // A composite script updater reads both members from one batch: a change
  // to either must resend the unchanged sibling too.
  v::ReadOnlyService grouped(profile(), policies());
  grouped.set_update_groups({{3, 4}});
  grouped.publish_batch({{3, Value{12.5}, 1000}, {4, num(100), 1000}}, 1000);
  (void)request(grouped, "registerRequest", str("*"), 1000);
  grouped.publish_batch({{3, Value{12.5}, 1001}, {4, num(200), 1001}}, 1001);
  check(v::decode_incoming(*grouped.updates(1001)).command.values ==
            readings({{num(3), Value{12.5}}, {num(4), num(200)}}),
        "grouped update omitted its unchanged sibling");
  check(!grouped.updates(1002), "unchanged group was resent");
  rejects([&] { grouped.set_update_groups({{3}}); });
  rejects([&] { grouped.set_update_groups({{3, 999}}); });
  rejects([&] { grouped.set_update_groups({{3, 4}, {4, 3}}); });
}
void sibling_group_freshness_and_retirement() {
  v::ReadOnlyService service(profile(), policies());
  service.set_update_groups({{3, 4}});
  service.publish_batch({{3, Value{12.5}, 1000}, {4, num(100), 1000}}, 1000);
  (void)request(service, "registerRequest", str("*"), 1000);
  service.publish(4, num(200), 1010, 1010);
  check(v::decode_incoming(*service.updates(1010)).command.values ==
            readings({{num(3), Value{12.5}}, {num(4), num(200)}}),
        "group update omitted the still-fresh sibling");
  // Resending does not refresh acquisition: signal 3 still expires at 1100.
  check(!service.updates(1099), "group sibling expired before its acquisition boundary");
  auto update = v::decode_incoming(*service.updates(1100)).command;
  check(update.values == readings({{num(3), Value{}}, {num(4), num(200)}}),
        "sibling delivery refreshed an expired acquisition");
  service.publish(4, num(300), 1101, 1101);
  update = v::decode_incoming(*service.updates(1101)).command;
  check(update.values == readings({{num(3), Value{}}, {num(4), num(300)}}),
        "changed group member revived its stale sibling");
  service.withdraw(4, 1102);
  update = v::decode_incoming(*service.updates(1102)).command;
  check(update.values == readings({{num(3), Value{}}}) &&
            update.errors == readings({{num(4), num(-6727)}}),
        "group provider withdrawal retained a reading or hid unavailability");
  check(!service.updates(1103), "group withdrawal repeated without a change");
  service.disconnect();
  check(service.subscription_count() == 0 && !service.updates(0),
        "group disconnect retained a subscriber or timed reading");
  service.publish_batch({{3, Value{20.0}, 1}, {4, num(100), 1}}, 1);
  (void)request(service, "registerRequest", arr({num(3)}), 1);
  service.publish(4, num(200), 2, 2);
  check(!service.updates(2), "unsubscribed sibling triggered a group notification");
  service.publish(3, Value{30.0}, 3, 3);
  update = v::decode_incoming(*service.updates(3)).command;
  check(update.values == readings({{num(3), Value{30.0}}}) && !update.errors,
        "group notification included an unsubscribed sibling");
  (void)request(service, "registerRequest", arr({num(4)}), 3);
  rejects([&] { service.set_update_groups({{3, 4}, {3, 4}}); });
  service.publish(4, num(300), 4, 4);
  update = v::decode_incoming(*service.updates(4)).command;
  check(update.values == readings({{num(3), Value{30.0}}, {num(4), num(300)}}),
        "failed group reconfiguration or disconnect lost the configured group");
}

void batch_atomicity() {
  v::ReadOnlyService service(profile(), policies());
  service.publish_batch({{3, Value{12.5}, 1000}, {4, num(100), 1000}}, 1000);
  (void)request(service, "registerRequest", str("*"), 1000);
  const auto original_profile = service.profile();
  service.publish_batch({{3, Value{20.0}, 1001}, {4, num(200), 1001}}, 1001);
  const auto update = service.updates(1001);
  check(bool(update), "coherent batch omitted its update");
  check(v::decode_incoming(*update).command.values ==
            readings({{num(3), Value{20.0}}, {num(4), num(200)}}),
        "coherent batch did not notify both related values");
  const std::vector<std::vector<v::Publication>> bad_batches{
      {{3, Value{30.0}, 1002}, {4, Value{true}, 1002}},
      {{3, {}, {}}, {4, Value{true}, 1002}},
      {{3, {}, 1002}, {4, Value{true}, 1002}},
      {{3, Value{30.0}, 1002}, {4, num(10001), 1002}},
      {{4, num(300), 1002}, {3, Value{std::numeric_limits<double>::quiet_NaN()}, 1002}},
      {{3, Value{30.0}, 1002}, {99, num(300), 1002}},
      {{3, Value{30.0}, 1002}, {4, num(300), 1003}},
      {{3, Value{30.0}, 1002}, {4, num(300), 1001}},
      {{3, Value{30.0}, 1002}, {4, num(300), {}}},
      {{3, Value{30.0}, 1002}, {4, {}, 1000}},
      {{3, Value{30.0}, 1002}, {3, {}, {}}}};
  for (const auto& batch : bad_batches) {
    rejects([&] { service.publish_batch(batch, 1002); });
    check(!service.updates(1001), "failed batch changed readings or advanced the service clock");
    const auto state = service.snapshot(std::array<std::int64_t, 2>{3, 4}, 1001);
    check(state.at(3).value == Value{20.0} && state.at(4).value == num(200),
          "invalid later entry partially committed");
  }
  rejects([&] { service.publish_batch({{3, Value{30.0}, 1002}, {4, num(300), 1401}}, 1400); });
  check(!service.updates(1001), "future rejected batch poisoned the monotonic clock");
  rejects([&] { service.publish_batch({{3, {}, {}}}, 1000); });
  check(!service.updates(1001) && service.subscription_count() == 2 &&
            service.profile() == original_profile,
        "backwards-clock rejection changed readings, subscriptions or configuration");
  // The failed first entry's acquisition time must also remain unused.
  service.publish_batch({{3, Value{30.0}, 1002}, {4, num(300), 1002}}, 1002);
  check(service.snapshot(std::array<std::int64_t, 2>{3, 4}, 1002).at(3).value == Value{30.0},
        "failed batch advanced a source ordering watermark");
}

void batch_withdrawal_and_recovery() {
  v::ReadOnlyService service(profile(), policies());
  service.publish_batch({{3, Value{18.0}, 1000}, {4, num(100), 1000}}, 1000);
  (void)request(service, "registerRequest", str("*"), 1000);
  service.publish_batch({{3, {}, {}}, {4, {}, 1000}}, 1020);
  const auto cleared = service.updates(1020);
  check(bool(cleared), "batch withdrawal omitted its update");
  const auto reply = v::decode_incoming(*cleared).command;
  check(reply.values == readings({{num(3), Value{}}}) &&
            reply.errors == readings({{num(4), num(-6727)}}),
        "withdrawal and explicit invalidation did not clear both consumers");
  service.publish_batch({{3, {}, {}}, {4, {}, 1000}}, 1030);
  check(!service.updates(1030),
        "repeated absence or same-time invalidation generated a new update");
  rejects([&] { service.publish_batch({{3, Value{18.0}, 1000}}, 1030); });
  rejects([&] { service.publish_batch({{4, num(100), 1000}}, 1030); });
  rejects([&] { service.publish_batch({{3, {}, 999}}, 1030); });
  service.publish_batch({{3, Value{19.0}, 1010}, {4, num(101), 1001}}, 1040);
  const auto recovered = service.updates(1040);
  check(bool(recovered), "delayed batch recovery omitted its update");
  check(v::decode_incoming(*recovered).command.values ==
            readings({{num(3), Value{19.0}}, {num(4), num(101)}}),
        "withdrawal replaced source timestamps with absence polling time");
  service.publish_batch({{3, {}, {}}, {4, num(102), 1002}}, 1050);
  rejects([&] { service.publish_batch({{3, Value{19.0}, 1010}}, 1050); });
  service.publish_batch({{3, Value{20.0}, 1020}}, 1060);
  (void)service.updates(1060);
  check(!service.updates(1119), "delayed source expired before its original-time boundary");
  const auto expired = service.updates(1120);
  check(bool(expired), "delayed source did not expire at its original-time boundary");
  check(v::decode_incoming(*expired).command.values == readings({{num(3), Value{}}}),
        "batch recovery renewed the acquisition time");
  service.publish_batch({{4, {}, 1125}}, 1130);
  rejects([&] { service.publish_batch({{4, num(103), 1125}}, 1130); });
  rejects([&] { service.publish_batch({{4, num(103), 1100}}, 1130); });
  service.publish_batch({{4, {}, 1125}}, 1130);  // Explicit invalidation may repeat its watermark.
  service.publish_batch({{4, num(103), 1126}}, 1130);
  check(service.snapshot(std::array<std::int64_t, 1>{4}, 1130).at(4).value == num(103),
        "new source observation failed after explicit invalidation");
}

void batch_size_and_fixed_configuration() {
  auto text_profile = profile();
  auto& accessories = std::get<op::Array>(field(text_profile, "accessories").data);
  auto& services = std::get<op::Array>(field(accessories[0], "services").data);
  auto& characteristics = std::get<op::Array>(field(services[0], "characteristics").data);
  field(characteristics[1], "format") = str("string");
  auto text_policies = policies();
  text_policies[1] = {4, v::ValueKind::string, 200, {}, {}};
  v::ReadOnlyService text_service(text_profile, text_policies);
  text_service.publish_batch({{3, Value{12.5}, 1000}, {4, str("observed"), 1000}}, 1000);
  rejects([&] {
    text_service.publish_batch({{3, Value{20.0}, 1001}, {4, str(std::string(2048, 'x')), 1001}},
                               1001);
  });
  const auto untouched = text_service.snapshot(std::array<std::int64_t, 2>{3, 4}, 1000);
  check(untouched.at(3).value == Value{12.5} && untouched.at(4).value == str("observed"),
        "oversized later value partially committed or advanced time");

  v::ReadOnlyService configured(profile(), policies(), {{4, num(8000)}});
  configured.publish_batch({{3, Value{12.5}, 1000}}, 1000);
  (void)request(configured, "registerRequest", str("*"), 1000);
  const std::vector<v::Publication> fixed_operations{
      {4, num(9000), 1001}, {4, {}, 1001}, {4, {}, {}}};
  for (const auto& fixed : fixed_operations) {
    rejects([&] { configured.publish_batch({{3, Value{20.0}, 1001}, fixed}, 1001); });
    check(!configured.updates(1000), "fixed-value rejection partially committed or advanced time");
  }
  configured.publish_batch({}, 1001);
  rejects([&] { configured.publish_batch({}, 1000); });
  check(configured.subscription_count() == 2 &&
            configured.snapshot(std::array<std::int64_t, 1>{4}, 1001).at(4).value == num(8000),
        "batch changed fixed configuration or subscriptions");

  op::Array large_characteristics;
  std::vector<v::SignalPolicy> large_policies;
  std::vector<v::Publication> publications;
  std::vector<std::int64_t> ids;
  for (std::int64_t index = 0; index < 512; ++index) {
    const auto id = index + 3;
    ids.push_back(id);
    large_characteristics.push_back(
        dict({{str("iid"), num(id)}, {str("supportsInvalid"), Value{true}}}));
    large_policies.push_back({id, v::ValueKind::integer, 100, 0.0, 511.0});
    publications.push_back({id, num(index), 1});
  }
  const auto large_profile =
      dict({{str("accessories"),
             arr({dict({{str("iid"), num(1)},
                        {str("services"),
                         arr({dict({{str("iid"), num(2)},
                                    {str("characteristics"),
                                     Value{std::move(large_characteristics)}}})})}})})}});
  v::ReadOnlyService large(large_profile, std::move(large_policies));
  large.publish_batch(publications, 1);
  const auto snapshot = large.snapshot(ids, 1);
  for (std::size_t index = 0; index < ids.size(); ++index)
    check(snapshot.at(ids[index]).value == num(index),
          "maximum-size batch did not commit every entry");
  for (auto& publication : publications)
    publication.sampled_ms = 2;
  publications.push_back({515, num(1), 2});
  rejects([&] { large.publish_batch(publications, 2); });
  check(large.snapshot(ids, 1) == snapshot, "oversized batch changed readings or time");
}

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--fixture") {
      const auto wire = op::encode(v::command_value(fixture(argv[2])));
      std::cout.write(reinterpret_cast<const char*>(wire.data()), wire.size());
      return 0;
    }
    check(argc == 1, "unexpected arguments");
    batch_atomicity();
    sibling_update_groups();
    sibling_group_freshness_and_retirement();
    batch_withdrawal_and_recovery();
    batch_size_and_fixed_configuration();
    v::ReadOnlyService s(profile(), policies());
    auto r = request(s, "readRequest", arr({num(3), num(4), num(99)}), 0);
    check(r.values == readings({{num(3), Value{}}}), "missing invalid-capable sample must be null");
    check(r.errors == readings({{num(4), num(-6727)}, {num(99), num(-6727)}}),
          "missing IDs need numeric-key errors");
    s.publish(3, Value{0.0}, 1, 1);
    s.publish(4, num(100), 1, 1);
    r = request(s, "registerRequest", arr({num(3), num(3), num(99)}), 1, 0);
    check(r.transaction == 0 && s.subscription_count() == 2, "zero transaction/duplicate IDs");
    check(r.values == readings({{num(3), Value{0.0}}}), "zero must remain a reading");
    check(!s.updates(1), "register snapshot must not generate duplicate update");
    s.publish(4, num(101), 2, 2);
    check(!s.updates(2), "unsubscribed signal emitted");
    s.publish(3, Value{10.5}, 3, 3);
    r = v::decode_incoming(*s.updates(3)).command;
    check(r.name == "updateNotify" && !r.transaction && !r.errors &&
              r.values == readings({{num(3), Value{10.5}}}),
          "update must contain only changed subscribed values");
    s.publish(3, Value{10.5}, 4, 4);
    check(!s.updates(103), "unchanged fresh sample emitted");
    r = v::decode_incoming(*s.updates(104)).command;
    check(r.values == readings({{num(3), Value{}}}), "expiry must clear old value at boundary");
    check(!s.updates(105), "expiry repeated every timer tick");
    s.publish(3, Value{11.0}, 106, 106);
    check(s.updates(106).has_value(), "recovery must notify");
    s.invalidate(3, 107, 107);
    check(s.updates(107).has_value(), "explicit invalidation must notify");
    rejects([&] { s.publish(3, Value{11.0}, 106, 107); });
    rejects([&] { s.publish(3, Value{11.0}, 108, 107); });
    rejects([&] { s.publish(3, Value{true}, 108, 108); });
    rejects([&] { s.publish(3, Value{201.0}, 108, 108); });
    rejects([&] { s.publish(3, Value{std::numeric_limits<double>::quiet_NaN()}, 108, 108); });
    rejects([&] { s.publish(99, Value{11.0}, 108, 108); });
    rejects([&] { s.updates(106); });
    r = request(s, "registerRequest", arr({num(4), Value{true}}), 108);
    check(r.error == -6756 && s.subscription_count() == 2,
          "malformed registration mutated subscriptions");
    r = request(s, "registerRequest", str("*"), 109);
    check(!r.error && s.subscription_count() == 2,
          "wildcard registration must replace explicit list");
    r = request(s, "readRequest", arr({}), 109);
    check(r.error == -6737, "empty read should fail");
    r = request(s, "readRequest", arr({num(4)}), 109, {});
    check(r.error == -6705, "read requires transaction ID");
    op::Array excessive(513, num(4));
    r = request(s, "registerRequest", Value{excessive}, 109);
    check(r.error == -6737 && s.subscription_count() == 2, "request count must be bounded");
    r = request(s, "writeRequest", readings({{num(4), num(500)}}), 110);
    check(r.name == "writeResponse" && r.error == -6710, "write must fail explicitly");
    r = request(s, "readRequest", arr({num(4)}), 110);
    check(r.values == readings({{num(4), num(101)}}), "write changed receiver-owned value");
    r = request(s, "unregisterRequest", arr({num(3)}), 111);
    check(!r.values && !r.errors && !r.error && s.subscription_count() == 1,
          "unsubscribe response");
    check(!s.updates(201), "integer sample expired early");
    r = v::decode_incoming(*s.updates(202)).command;
    check(r.errors == readings({{num(4), num(-6727)}}),
          "expired non-null-capable sample needs error");
    (void)request(s, "unregisterRequest", str("*"), 203);
    check(s.subscription_count() == 0 && !s.updates(204), "unsubscribe all");
    (void)request(s, "registerRequest", str("*"), 205);
    s.disconnect();
    check(s.subscription_count() == 0 && !s.updates(0), "disconnect must clear connection state");
    r = request(s, "readRequest", arr({num(4)}), 0);
    check(!r.values && r.errors, "disconnect must invalidate cached readings");
    rejects([] { v::ReadOnlyService bad(profile()); });
    rejects([] {
      auto p = policies();
      p[0].max_age_ms = 0;
      v::ReadOnlyService bad(profile(), p);
    });
    rejects([] {
      auto p = policies();
      p.push_back(p[0]);
      v::ReadOnlyService bad(profile(), p);
    });
    {
      v::ReadOnlyService configured(profile(), policies(), {{4, num(8000)}});
      configured.publish(3, Value{50.0}, 1, 1);
      r = request(configured, "registerRequest", str("*"), 1);
      check(r.values == readings({{num(3), Value{50.0}}, {num(4), num(8000)}}),
            "static and sampled initial values");
      r = v::decode_incoming(*configured.updates(1000000)).command;
      check(r.values == readings({{num(3), Value{}}}), "only the live sample expires");
      check(!configured.updates(1000001), "fixed configuration must not generate timer updates");
      rejects([&] { configured.publish(4, num(9000), 1000002, 1000002); });
      rejects([&] { configured.invalidate(4, 1000002, 1000002); });
      rejects([&] { configured.withdraw(4, 1000002); });
      configured.disconnect();
      r = request(configured, "readRequest", arr({num(3), num(4)}), 0);
      check(r.values == readings({{num(3), Value{}}, {num(4), num(8000)}}),
            "disconnect preserves only fixed configuration");
      rejects([] { v::ReadOnlyService bad(profile(), policies(), {{4, Value{true}}}); });
      rejects([] { v::ReadOnlyService bad(profile(), policies(), {{4, num(10001)}}); });
      rejects([] { v::ReadOnlyService bad(profile(), policies(), {{99, num(1)}}); });
    }
    {
      v::ReadOnlyService recovering(profile(), policies());
      recovering.publish(3, Value{18.0}, 1000, 1000);
      (void)request(recovering, "registerRequest", arr({num(3)}), 1000);
      recovering.withdraw(3, 1020);
      r = v::decode_incoming(*recovering.updates(1020)).command;
      check(r.values == readings({{num(3), Value{}}}),
            "provider loss must notify without becoming a measurement");
      recovering.withdraw(3, 1030);
      check(!recovering.updates(1030), "continued provider loss must not repeat notifications");
      rejects([&] { recovering.publish(3, Value{18.0}, 1000, 1030); });
      rejects([&] { recovering.withdraw(3, 1029); });
      rejects([&] { recovering.withdraw(99, 1030); });
      recovering.publish(3, Value{19.0}, 1010, 1040);
      r = v::decode_incoming(*recovering.updates(1040)).command;
      check(r.values == readings({{num(3), Value{19.0}}}),
            "fresh delayed source must recover after a later absence poll");
      recovering.withdraw(3, 1050);
      check(recovering.snapshot(std::array<std::int64_t, 1>{3}, 1050).at(3).value == Value{},
            "withdrawn reading must be unknown immediately");
      recovering.publish(3, Value{20.0}, 1020, 1060);
      rejects([&] { recovering.publish(3, Value{20.0}, 1015, 1060); });
    }
    {
      double simulated_target = 21.0;
      v::Service writable(writable_profile(), policies(), {},
                          {{3, [&](const Value& requested) {
                              simulated_target = std::get<double>(requested.data);
                              return Value{simulated_target};
                            }}});
      writable.publish(3, Value{simulated_target}, 1, 1);
      writable.publish(4, num(2), 1, 1);
      r = request(writable, "registerRequest", arr({num(3)}), 1);
      check(r.values == readings({{num(3), Value{21.0}}}), "initial simulated climate reading");
      r = request(writable, "writeRequest", readings({{num(3), Value{22.5}}}), 2);
      check(!r.error && r.values == readings({{num(3), Value{22.5}}}) && simulated_target == 22.5,
            "validated write must update the simulated model");
      r = v::decode_incoming(*writable.updates(2)).command;
      check(r.values == readings({{num(3), Value{22.5}}}),
            "subscribers need applied climate state");
      r = request(writable, "writeRequest", readings({{num(3), Value{201.0}}}), 3);
      check(r.errors == readings({{num(3), num(-6705)}}) && simulated_target == 22.5,
            "out-of-range write must not mutate model");
      r = request(writable, "writeRequest", readings({{num(4), num(3)}}), 4);
      check(r.errors == readings({{num(4), num(-6710)}}), "unbound write must fail");
      r = request(writable, "writeRequest", readings({{num(99), num(3)}}), 5);
      check(r.errors == readings({{num(99), num(-6727)}}), "unknown write IID must fail");
      r = request(writable, "writeRequest", readings({{num(3), Value{23.0}}, {num(4), num(4)}}), 6);
      check(!r.error && r.values == readings({{num(3), Value{23.0}}}) &&
                r.errors == readings({{num(4), num(-6710)}}) && simulated_target == 23.0,
            "batch must report applied values and independent unbound errors");
      rejects([&] {
        request(writable, "writeRequest", readings({{num(3), Value{24.0}}, {num(3), Value{25.0}}}),
                7);
      });
      check(simulated_target == 23.0, "duplicate batch must fail before any provider callback");
      r = request(writable, "writeRequest", readings({{num(3), Value{24.0}}, {str("bad"), num(1)}}),
                  8);
      check(r.error == -6756 && simulated_target == 23.0,
            "malformed later IID must not apply earlier writes");
      op::Dictionary excessive_writes;
      for (std::int64_t id = 3; id < 36; ++id)
        excessive_writes.emplace_back(num(id), Value{24.0});
      r = request(writable, "writeRequest", Value{excessive_writes}, 9);
      check(r.error == -6737 && simulated_target == 23.0, "oversized batch invoked a writer");
      rejects([] { v::Service bad(writable_profile(), policies()); });
      rejects([] {
        v::Service bad(profile(), policies(), {}, {{3, [](const Value& v) {
                                                      return v;
                                                    }}});
      });
      rejects([] {
        v::Service bad(writable_profile(), policies(), {{3, Value{21.0}}}, {{3, [](const Value& v) {
                                                                               return v;
                                                                             }}});
      });
    }
    std::cout << "Vehicle service: subscriptions, reads, updates, expiry, recovery, bounds and "
                 "disconnect passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
