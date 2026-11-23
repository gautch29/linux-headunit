// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/camera_state.hpp>
#include <headunit/theme/vehicle_transport.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <new>

void fail_allocation_after(int count);

namespace v = headunit::theme::vehicle;
namespace o = headunit::theme::opack;
using o::Array;
using o::Dictionary;
using o::Value;
namespace {
void check(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
constexpr std::uint64_t ns(std::uint64_t ms) {
  return ms * 1000000;
}
Value& field(Value& object, std::string_view name) {
  for (auto& [key, value] : std::get<Dictionary>(object.data))
    if (std::get<std::string>(key.data) == name) return value;
  throw std::runtime_error("fixture field absent");
}
v::Configuration camera(bool writable, std::string_view settings = {}) {
  auto config = v::parse_configuration("", settings, R"({"version":1,"experimental":true,
    "stream_identifier":"camera","general":{"on":{"binding":"camera.visible",
    "max_age_ms":151,"writable":false,"initial":false}}})");
  if (!writable) return config;
  // Explicit typed fixture topology, not an admitted camera JSON capability.
  // No missing CameraButton/UserDismissible/default/focus fact is guessed here.
  const auto iid = config.camera->camera_general_on_iid;
  for (auto& signal : config.signals)
    if (signal.policy.iid == iid) signal.writable = true;
  for (auto& accessory : std::get<Array>(field(config.profile, "accessories").data))
    for (auto& service : std::get<Array>(field(accessory, "services").data))
      for (auto& characteristic : std::get<Array>(field(service, "characteristics").data))
        if (field(characteristic, "iid") == Value{iid})
          field(characteristic, "writable") = Value{true};
  return config;
}
headunit::DataMessage write(std::int64_t iid, Value value, std::int64_t tx = 7) {
  headunit::DataMessage message;
  message.kind = headunit::DataMessageKind::synchronous;
  message.type = 0x636d6e64;
  message.token = 42;
  message.payload = v::encode_outgoing(
      {"writeRequest", tx, Value{Dictionary{{Value{iid}, std::move(value)}}}, {}, {}}, 0);
  return message;
}
v::Command pop(v::Transport& transport) {
  check(transport.pending() && !transport.pending()->payload.empty(), "application reply absent");
  auto result = v::decode_incoming(transport.pending()->payload).command;
  transport.sent();
  return result;
}
void ack(v::Transport& transport) {
  check(transport.pending() && transport.pending()->kind == headunit::DataMessageKind::reply &&
            transport.pending()->payload.empty() && transport.pending()->token == 42,
        "transport receipt lost or contains application success");
  transport.sent();
}
struct Fixture {
  v::Configuration config = camera(true);
  std::shared_ptr<v::CameraStateSnapshot> state = std::make_shared<v::CameraStateSnapshot>();
  std::uint64_t clock = ns(13), generation = state->begin(ns(10));
  std::shared_ptr<v::Provider> provider = v::camera_state_provider(
      v::simulated_provider(config), *config.camera, state, [this] { return clock; });
  v::ConfiguredModel model{config, provider};
  std::shared_ptr<v::Coverage> coverage = std::make_shared<v::Coverage>(model.service().profile());
  unsigned observed_replies = 0;
  v::Transport transport{
      model.service(), [this](const auto&, const auto&) { ++observed_replies; }, {}, coverage};
  Fixture() {
    check(state->observe(generation, true, ns(12), ns(13)), "capture failed");
    model.refresh(13);
  }
  std::int64_t iid() const { return config.camera->camera_general_on_iid; }
  v::Service::Reading reading(std::uint64_t now) {
    const std::array ids{iid()};
    return model.service().snapshot(ids, now).at(iid());
  }
  v::CameraStateSnapshot::OffRequest start_at(std::uint64_t service_tick, std::uint64_t fine) {
    clock = fine;
    model.refresh(service_tick);
    check(transport.receive(write(iid(), Value{false}), service_tick), "request backpressured");
    ack(transport);
    check(!transport.pending() && observed_replies == 0,
          "eligible owner request did not remain pending");
    const auto request = state->pending_off(generation, clock);
    check(bool(request), "production provider never dispatched owner operation");
    return *request;
  }
  v::CameraStateSnapshot::OffRequest start() { return start_at(14, ns(14) + 100); }
  void receipt(const v::CameraStateSnapshot::OffRequest& request) {
    check(state->owner_off(generation, request.sequence, ns(15)), "withdrawal failed");
    check(state->presented_off(generation, request.sequence, 2, 2, 1, ns(16), ns(17), ns(18)),
          "accepted owner receipt failed");
  }
  void pump(std::uint64_t ms) {
    clock = ns(ms) + 100;
    transport.pump(ms);
  }
};
void unregistered_operation_allocation_failure() {
  auto config = camera(true, R"({"version":1,
    "sections":[{"id":"bench","label":"Bench"}],
    "settings":[{"id":"lamp","section":"bench","label":"Lamp",
                 "kind":"boolean","initial":false}]})");
  const auto c = *config.camera;
  auto base = v::simulated_provider(config);
  auto state = std::make_shared<v::CameraStateSnapshot>();
  const auto generation = state->begin(ns(10));
  check(state->observe(generation, true, ns(12), ns(13)), "allocation fixture capture failed");
  std::uint64_t fine = ns(14) + 100;
  auto provider = v::camera_state_provider(base, c, state, [&] { return fine; });
  auto policies = config.fixed_policies;
  std::int64_t setting_iid = 0;
  std::string setting_key;
  for (const auto& signal : config.signals) {
    policies.push_back(signal.policy);
    if (signal.writable && signal.key != c.binding) {
      setting_iid = signal.policy.iid;
      setting_key = signal.key;
    }
  }
  check(setting_iid != 0, "independent synchronous setting absent");
  std::optional<v::CameraStateSnapshot::OffRequest> started;
  v::Service service{config.profile,
                     policies,
                     config.fixed,
                     {{setting_iid,
                       [&](const Value& value) {
                         return base->apply(setting_key, value, 14);
                       }}},
                     {{c.camera_general_on_iid,
                       [&](const Value& value, std::uint64_t tick, std::uint64_t deadline) {
                         auto pending =
                             provider->start_confirmation(c.binding, value, tick, deadline);
                         started = state->pending_off(generation, fine);
                         check(pending && started, "production camera operation never started");
                         // The service has reserved its operation vector already. Inject the
                         // next allocation after successful start: registration of that batch,
                         // after the preceding independent synchronous write has completed.
                         fail_allocation_after(0);
                         return pending;
                       }}}};
  service.publish(c.camera_general_on_iid, Value{true}, 12, 13);
  service.publish(setting_iid, Value{false}, 12, 13);
  auto transport = std::make_unique<v::Transport>(service);
  auto message = write(c.camera_general_on_iid, Value{false});
  message.payload =
      v::encode_outgoing({"writeRequest",
                          7,
                          Value{Dictionary{{Value{setting_iid}, Value{true}},
                                           {Value{c.camera_general_on_iid}, Value{false}}}},
                          {},
                          {}},
                         0);
  bool failed = false;
  try {
    (void)transport->receive(message, 14);
  } catch (const std::bad_alloc&) {
    failed = true;
  }
  fail_allocation_after(-1);
  check(failed && started, "post-start allocation failure was not exercised");
  check(!transport->pending(), "exception returned receipt/applied response");
  check(state->off_result(generation, started->sequence, fine)->status ==
                v::CameraStateSnapshot::OffStatus::retired &&
            !state->pending_off(generation, fine),
        "unregistered camera operation survived allocation failure");
  const std::array ids{setting_iid, c.camera_general_on_iid};
  const auto current = service.snapshot(ids, 14);
  check(current.at(setting_iid).value == Value{true} &&
            current.at(c.camera_general_on_iid).value == Value{true},
        "exception lost synchronous result or published queued camera intent");
  const auto successor = state->request_off(ns(15), ns(100));
  check(bool(successor), "exceptional retirement left camera owner slot occupied");
  transport.reset();
  check(state->pending_off(generation, ns(16))->sequence == successor->sequence,
        "late retiring transport cancelled successor operation");
  state->cancel_off(generation, successor->sequence);
}
void camera_callback_storage_failure() {
  unsigned failures = 0, admitted = 0;
  for (int fail_at = 0; fail_at < 8; ++fail_at) {
    Fixture f;
    f.clock = ns(14) + 100;
    std::optional<v::PendingWrite> operation;
    fail_allocation_after(fail_at);
    try {
      operation = f.provider->start_confirmation(f.config.camera->binding, Value{false}, 14, 5014);
    } catch (const std::bad_alloc&) {
      ++failures;
    }
    fail_allocation_after(-1);
    if (operation) {
      ++admitted;
      check(bool(f.state->pending_off(f.generation, f.clock)), "admitted operation absent");
      operation.reset();  // Its cancellation belongs to the returned object.
    }
    check(!f.state->pending_off(f.generation, f.clock),
          "camera callback storage failure/destruction leaked actual owner intent");
  }
  check(failures != 0 && admitted != 0, "callback allocation failure and success not covered");
}
void model_wrapper_allocation_failure() {
  auto config = camera(true);
  const auto c = *config.camera;
  auto state = std::make_shared<v::CameraStateSnapshot>();
  const auto generation = state->begin(ns(10));
  check(state->observe(generation, true, ns(12), ns(13)), "wrapper fixture capture failed");
  std::uint64_t fine = ns(14) + 100;
  auto actual =
      v::camera_state_provider(v::simulated_provider(config), c, state, [&] { return fine; });
  // Delegate all production camera behavior; inject only after actual owner
  // admission and before ConfiguredModel builds its validation callback.
  class Provider final : public v::Provider {
   public:
    std::shared_ptr<v::Provider> actual;
    std::shared_ptr<v::CameraStateSnapshot> state;
    std::uint64_t generation, fine;
    std::optional<v::CameraStateSnapshot::OffRequest> started;
    Provider(std::shared_ptr<v::Provider> source, std::shared_ptr<v::CameraStateSnapshot> owner,
             std::uint64_t gen, std::uint64_t now)
        : actual(std::move(source)), state(std::move(owner)), generation(gen), fine(now) {}
    bool supports_confirmation(std::string_view key) const override {
      return actual->supports_confirmation(key);
    }
    std::optional<v::Sample> read(std::string_view key, std::uint64_t tick) override {
      return actual->read(key, tick);
    }
    Value apply(std::string_view key, const Value& value, std::uint64_t tick) override {
      return actual->apply(key, value, tick);
    }
    std::optional<v::PendingWrite> start_confirmation(std::string_view key, const Value& value,
                                                      std::uint64_t tick,
                                                      std::uint64_t deadline) override {
      auto operation = actual->start_confirmation(key, value, tick, deadline);
      started = state->pending_off(generation, fine);
      check(operation && started, "wrapper production operation never started");
      fail_allocation_after(0);
      return operation;
    }
  };
  auto provider = std::make_shared<Provider>(actual, state, generation, fine);
  v::ConfiguredModel model(config, provider);
  v::Transport transport(model.service());
  model.refresh(14);
  check(transport.receive(write(c.camera_general_on_iid, Value{false}), 14),
        "wrapper write not answered");
  fail_allocation_after(-1);
  ack(transport);
  check(pop(transport).errors.has_value() && provider->started,
        "wrapper allocation failure was not returned as per-IID error");
  check(state->off_result(generation, provider->started->sequence, fine)->status ==
                v::CameraStateSnapshot::OffStatus::retired &&
            !state->pending_off(generation, fine),
        "model wrapper exception leaked actual camera operation");
}
void request_crosses_service_tick() {
  for (const auto fine : {ns(15) + 100, ns(22) + 200}) {
    Fixture f;
    const auto request = f.start_at(14, fine);
    check(request.requested_ns == fine && request.deadline_ns == ns(5014),
          "dispatch reconstructed actual request time or extended original deadline");
    // Decode/dispatch can advance several milliseconds beyond the original
    // service tick. Pending still leaves the service reading unchanged.
    f.clock = fine + 100;
    f.transport.pump(14);
    check(!f.transport.pending() && f.reading(14).value == Value{true},
          "crossed-tick pending request published intent");
    check(f.state->owner_off(f.generation, request.sequence, fine + 200),
          "crossed-tick owner withdrawal rejected");
    const auto presented = fine + 600;
    check(f.state->presented_off(f.generation, request.sequence, 2, 2, 1, fine + 400, presented,
                                 fine + 800),
          "matching crossed-tick presentation rejected");
    const auto poll_ms = fine / 1000000 + 2;
    f.pump(poll_ms);
    const auto response = pop(f.transport);
    check(response.name == "writeResponse" && response.transaction == 7 && !response.errors &&
              response.values == Value{Dictionary{{Value{f.iid()}, Value{false}}}},
          "crossed-tick genuine completion did not confirm original transaction");
    const auto result = f.state->off_result(f.generation, request.sequence, f.clock);
    check(result && result->presented_ns == presented && f.observed_replies == 1,
          "actual presentation date or single completion lost");
    const auto sampled_ms = presented / 1000000;
    check(f.reading(sampled_ms + 150).value == Value{false} &&
              f.reading(sampled_ms + 151).error.has_value(),
          "completion substituted service/dispatch/poll time for presentation acquisition");
  }
}
void unrelated_off_does_not_confirm_crossed_request() {
  Fixture f;
  // A previous operation and independent Off observation postdate the service
  // tick but precede actual dispatch. The batch's coarse lower bound alone
  // cannot identify the new owner operation.
  const auto prior = *f.state->request_off(ns(14) + 100, ns(100));
  check(f.state->owner_off(f.generation, prior.sequence, ns(14) + 200), "prior Off failed");
  check(f.state->presented_off(f.generation, prior.sequence, 2, 2, 1, ns(14) + 300, ns(14) + 400,
                               ns(14) + 500),
        "prior receipt failed");
  const auto request = f.start_at(14, ns(16) + 700);
  check(request.sequence != prior.sequence, "new dispatch reused old owner identity");
  check(f.state->observe(f.generation, false, 0, ns(17)), "independent Off not observed");
  f.model.refresh(17);
  f.pump(17);
  check(f.reading(17).value == Value{false} && !f.transport.pending() && f.observed_replies == 0,
        "independent readback or preceding receipt confirmed new request");
  check(!f.state->presented_off(f.generation, prior.sequence, 2, 2, 1, ns(14) + 300, ns(14) + 400,
                                ns(17) + 100),
        "preceding operation identity completed successor");
  check(f.state->owner_off(f.generation, request.sequence, ns(17) + 200), "new withdrawal failed");
  check(!f.state->presented_off(f.generation, request.sequence, 2, 2, 2, ns(14) + 300, ns(14) + 400,
                                ns(17) + 300),
        "preceding presentation date confirmed successor");
  check(f.state->presented_off(f.generation, request.sequence, 2, 2, 2, ns(17) + 400, ns(17) + 600,
                               ns(17) + 800),
        "new receipt failed");
  f.pump(18);
  check(pop(f.transport).values.has_value(), "new exact receipt was not confirmed");
}
void camera_confirmation_time_failures() {
  for (unsigned failure = 0; failure < 4; ++failure) {
    Fixture f;
    f.model.refresh(14);
    if (failure == 0) f.clock = ns(14) - 1;  // Future service tick.
    if (failure == 1) {
      check(f.state->observe(f.generation, true, ns(14) + 400, ns(14) + 900),
            "new owner observation failed");
      f.clock = ns(14) + 800;  // Before the camera owner's actual observation.
    }
    if (failure == 2) f.clock = ns(5014);  // At original deadline, not dispatch+5s.
    if (failure == 3) f.clock = ns(5014) + 1;
    check(f.transport.receive(write(f.iid(), Value{false}), 14), "bad time request not answered");
    ack(f.transport);
    check(pop(f.transport).errors.has_value() && !f.state->pending_off(f.generation, ns(5014)),
          "future/reversed/expired request dispatched or confirmed");
    check(f.reading(14).value == Value{true}, "rejected timing admission changed service reading");
  }
  {
    Fixture f;
    const auto request = f.start_at(14, ns(15) + 100);
    f.clock = ns(15) + 900;
    f.transport.pump(15);
    check(!f.transport.pending(), "normal pending poll produced response");
    f.clock = ns(15) + 800;  // Same service ms, actual clock reversed after poll.
    f.transport.pump(15);
    check(pop(f.transport).errors.has_value() && f.reading(15).value == Value{true},
          "reversed fine poll retained operation or changed current state");
    check(f.state->off_result(f.generation, request.sequence, ns(16))->status ==
              v::CameraStateSnapshot::OffStatus::retired,
          "fine reversal did not cancel exact request");
    // A new operation cannot reset the provider's genuine clock frontier.
    check(f.transport.receive(write(f.iid(), Value{false}), 15), "reversed restart not answered");
    ack(f.transport);
    check(pop(f.transport).errors.has_value() && !f.state->pending_off(f.generation, ns(16)),
          "replacement operation reset actual clock watermark");
  }
  for (const bool already_presented : {false, true}) {
    Fixture f;
    const auto request = f.start_at(14, ns(15) + 100);
    if (already_presented) {
      check(f.state->owner_off(f.generation, request.sequence, ns(16)),
            "late fixture withdrawal failed");
      check(f.state->presented_off(f.generation, request.sequence, 2, 2, 1, ns(17), ns(18), ns(19)),
            "late fixture receipt failed");
    }
    f.model.service().publish(f.iid(), Value{true}, 5013, 5013);
    // The earlier service tick remains below deadline while actual dispatch
    // has reached it. Neither pending nor cached applied completion may pass.
    f.clock = request.deadline_ns;
    f.transport.pump(5013);
    check(pop(f.transport).errors.has_value() && f.reading(5013).value == Value{true},
          "actual expired deadline passed via earlier coarse poll tick");
  }
  {
    Fixture f;
    const auto request = f.start_at(14, ns(16));
    check(f.state->owner_off(f.generation, request.sequence, ns(17)),
          "retired fixture withdrawal failed");
    check(f.state->presented_off(f.generation, request.sequence, 2, 2, 1, ns(18), ns(19), ns(20)),
          "retired fixture receipt failed");
    f.state->begin(ns(21));
    f.model.service().publish(f.iid(), Value{true}, 21, 21);
    f.pump(22);
    check(pop(f.transport).errors.has_value() && f.reading(22).value == Value{true},
          "retired crossed-tick request changed successor reading");
  }
}
void accepted_pending_completion() {
  Fixture f;
  const auto request = f.start();
  f.pump(14);
  check(f.reading(14).value == Value{true}, "pending intent changed current state");
  // A separate origin cannot dispatch the same IID while the first owns it.
  v::Transport high(f.model.service(), {}, {}, f.coverage, 1);
  check(high.receive(write(f.iid(), Value{false}, 8), 14), "busy request not answered");
  ack(high);
  check(pop(high).errors.has_value(), "second channel bypassed IID ownership");
  f.receipt(request);
  f.pump(19);
  const auto response = pop(f.transport);
  check(response.name == "writeResponse" && response.transaction == 7 && !response.error &&
            !response.errors &&
            response.values == Value{Dictionary{{Value{f.iid()}, Value{false}}}},
        "matching completion lost original transaction/value");
  check(f.observed_replies == 1 && f.reading(19).value == Value{false},
        "completion not applied once");
  const auto rows = f.coverage->resources();
  check(rows.size() == 1 && rows[0].channels[0].confirmed_writes == 1 &&
            rows[0].channels[1].confirmed_writes == 0,
        "pending/rejected write counted as confirmation");
  // The confirmation date is the real presentation at17ms, not polling at19ms.
  check(!f.reading(168).value || f.reading(168).value == Value{},
        "polling renewed confirmation age");
}
void failure_and_retirement() {
  {
    Fixture f;
    const auto request = f.start();
    check(f.state->owner_off(f.generation, request.sequence, ns(15)), "withdrawal failed");
    check(!f.state->presented_off(f.generation, request.sequence, 1, 2, 1, ns(16), ns(17), ns(18)),
          "old epoch receipt accepted");
    f.pump(19);
    check(!f.transport.pending() && f.reading(19).value == Value{true},
          "rejected receipt changed service current state");
    f.pump(5014);
    check(pop(f.transport).errors.has_value(), "timeout produced applied value");
    check(!f.state->presented_off(f.generation, request.sequence, 2, 2, 1, ns(5015), ns(5016),
                                  ns(5017)),
          "late receipt revived timeout");
    check(!f.reading(5017).value || f.reading(5017).value == Value{},
          "timeout fabricated fresh reading");
  }
  {
    Fixture f;
    const auto request = f.start();
    f.receipt(request);
    // Even an already produced receipt cannot be delivered through an old owner
    // after the actual camera generation retires before the receiver consumes it.
    f.state->begin(ns(19));
    f.model.service().publish(f.iid(), Value{true}, 19, 19);
    f.pump(20);
    check(pop(f.transport).errors.has_value() && f.reading(20).value == Value{true},
          "retired camera completion mutated successor reading");
  }
  {
    Fixture f;
    const auto request = f.start();
    f.model.disconnect();
    check(f.state->off_result(f.generation, request.sequence, ns(15))->status ==
              v::CameraStateSnapshot::OffStatus::retired,
          "disconnect left owner operation pending");
    f.pump(15);
    check(!f.transport.pending(), "disconnect replayed application response");
  }
  {
    Fixture f;
    auto temporary = std::make_unique<v::Transport>(
        f.model.service(), v::Transport::WriteObserver{}, nullptr, f.coverage, 1);
    f.clock = ns(14);
    check(temporary->receive(write(f.iid(), Value{false}), 14), "temporary request failed");
    const auto old = *f.state->pending_off(f.generation, ns(14));
    temporary.reset();
    check(f.state->off_result(f.generation, old.sequence, ns(15))->status ==
              v::CameraStateSnapshot::OffStatus::retired,
          "channel destructor did not cancel owner");
    const auto newer = *f.state->request_off(ns(16), ns(100));
    f.state->cancel_off(f.generation, old.sequence);
    check(f.state->pending_off(f.generation, ns(17))->sequence == newer.sequence,
          "old cancellation retired replacement operation");
  }
}
struct Controlled {
  std::array<v::WriteCompletion, 2> outcomes;
  unsigned starts = 0, cancellations = 0, synchronous = 0;
  static Value topology() {
    Array characteristics;
    for (std::int64_t id = 3; id <= 5; ++id)
      characteristics.push_back(Value{Dictionary{{Value{std::string("iid")}, Value{id}},
                                                 {Value{std::string("type")}, Value{id}},
                                                 {Value{std::string("writable")}, Value{true}}}});
    auto service = Value{
        Dictionary{{Value{std::string("iid")}, Value{std::int64_t(2)}},
                   {Value{std::string("characteristics")}, Value{std::move(characteristics)}}}};
    auto accessory =
        Value{Dictionary{{Value{std::string("iid")}, Value{std::int64_t(1)}},
                         {Value{std::string("services")}, Value{Array{std::move(service)}}}}};
    return Value{
        Dictionary{{Value{std::string("accessories")}, Value{Array{std::move(accessory)}}}}};
  }
  v::Service service{topology(),
                     {{3, v::ValueKind::boolean, 10000, {}, {}},
                      {4, v::ValueKind::boolean, 10000, {}, {}},
                      {5, v::ValueKind::boolean, 10000, {}, {}}},
                     {},
                     {{3,
                       [this](const Value& value) {
                         ++synchronous;
                         return value;
                       }}},
                     {{4,
                       [this](const Value&, std::uint64_t, std::uint64_t) {
                         return start(0);
                       }},
                      {5, [this](const Value&, std::uint64_t, std::uint64_t) {
                         return start(1);
                       }}}};
  v::Transport transport{service};
  std::optional<v::PendingWrite> start(std::size_t index) {
    ++starts;
    return v::PendingWrite{[this, index](std::uint64_t) { return outcomes[index]; },
                           [this] {
                             ++cancellations;
                           }};
  }
  Controlled() {
    for (std::int64_t id = 3; id <= 5; ++id)
      service.publish(id, Value{false}, 90, 100);
  }
  Value current(std::int64_t id, std::uint64_t now) {
    const std::array ids{id};
    return *service.snapshot(ids, now).at(id).value;
  }
};
void batch_and_backpressure() {
  {
    Controlled c;
    auto message = write(4, Value{true});
    message.payload =
        v::encode_outgoing({"writeRequest",
                            7,
                            Value{Dictionary{{Value{std::int64_t(3)}, Value{true}},
                                             {Value{std::int64_t(4)}, Value{true}},
                                             {Value{std::int64_t(5)}, Value{true}},
                                             {Value{std::int64_t(99)}, Value{false}}}},
                            {},
                            {}},
                           0);
    check(c.transport.receive(message, 100), "mixed batch backpressured");
    ack(c.transport);
    check(c.starts == 2 && c.synchronous == 1 && !c.transport.pending() &&
              c.current(3, 100) == Value{true} && c.current(4, 100) == Value{false},
          "mixed batch treated pending as applied or lost synchronous outcome");
    c.outcomes[1].status = v::WriteCompletion::Status::rejected;
    c.transport.pump(101);
    check(!c.transport.pending() && c.current(5, 101) == Value{false},
          "rejection fabricated state");
    c.outcomes[0] = {v::WriteCompletion::Status::applied, Value{false}, 102};
    c.transport.pump(103);
    const auto result = pop(c.transport);
    check(result.values == Value{Dictionary{{Value{std::int64_t(3)}, Value{true}},
                                            {Value{std::int64_t(4)}, Value{false}}}} &&
              std::get<Dictionary>(result.errors->data).size() == 2 && c.cancellations == 2,
          "mixed per-IID completion/error or actual different value lost");
  }
  {
    Controlled c;
    auto malformed = write(4, Value{true});
    malformed.payload =
        v::encode_outgoing({"writeRequest",
                            7,
                            Value{Dictionary{{Value{std::int64_t(4)}, Value{true}},
                                             {Value{std::string("bad-later-key")}, Value{false}}}},
                            {},
                            {}},
                           0);
    check(c.transport.receive(malformed, 100), "malformed request backpressured");
    ack(c.transport);
    check(pop(c.transport).error.has_value() && c.starts == 0, "malformed batch dispatched owner");
    Dictionary oversized;
    for (std::int64_t id = 0; id < 33; ++id)
      oversized.emplace_back(Value{id}, Value{true});
    malformed.payload = v::encode_outgoing({"writeRequest", 8, Value{oversized}, {}, {}}, 0);
    check(c.transport.receive(malformed, 100), "oversized batch backpressured");
    ack(c.transport);
    check(pop(c.transport).error.has_value() && c.starts == 0, "33-IID batch dispatched owner");
  }
  {
    Controlled c;
    const auto notification = v::encode_outgoing({"updateNotify", {}, {}, {}, {}}, 0);
    // Capacity refusal happens before dispatch; retry is intact and runs once.
    unsigned filled = 0;
    while (c.transport.notify(notification))
      ++filled;
    check(filled == 32 && !c.transport.receive(write(4, Value{true}), 100) && c.starts == 0,
          "backpressure executed writer before reservation");
    while (c.transport.pending())
      c.transport.sent();
    check(c.transport.receive(write(4, Value{true}), 100) && c.starts == 1,
          "retry did not dispatch once");
    ack(c.transport);
    check(!c.transport.receive(write(5, Value{true}), 100) && c.starts == 1,
          "deferred response byte reservation underflow admitted another writer");
    filled = 0;
    while (c.transport.notify(notification))
      ++filled;
    check(filled == 31, "deferred reply lost output capacity reservation");
    c.outcomes[0] = {v::WriteCompletion::Status::applied, Value{true}, 101};
    c.transport.pump(102);
    unsigned notifications = 0, responses = 0;
    while (c.transport.pending()) {
      const auto command = pop(c.transport);
      if (command.name == "updateNotify") ++notifications;
      if (command.name == "writeResponse") ++responses;
    }
    check(notifications == 31 && responses == 1 && c.starts == 1,
          "completion overflowed or redispatched");
  }
  for (unsigned failure = 0; failure < 6; ++failure) {
    Controlled c;
    check(c.transport.receive(write(4, Value{true}), 100), "failure fixture not admitted");
    ack(c.transport);
    c.outcomes[0] = {v::WriteCompletion::Status::applied, Value{true}, 101};
    if (failure == 0) c.outcomes[0].sampled_ms = 99;
    if (failure == 1) c.outcomes[0].sampled_ms = 103;
    if (failure == 2) c.outcomes[0].sampled_ms.reset();
    if (failure == 3) c.outcomes[0].value = Value{std::int64_t(1)};
    if (failure == 4) c.outcomes[0].status = v::WriteCompletion::Status::retired;
    const auto now = failure == 5 ? 5100 : 102;
    c.transport.pump(now);
    check(pop(c.transport).errors.has_value() && c.current(4, now) == Value{false},
          "late/rejected/malformed completion changed current state");
  }
}

void independent_observations_and_default() {
  {
    Fixture f;
    const auto request = f.start();
    f.receipt(request);
    f.model.service().publish(f.iid(), Value{true}, 18, 18);
    f.pump(19);
    check(pop(f.transport).values.has_value() && f.reading(19).value == Value{true},
          "older completion overwrote independent newer observation");
  }
  {
    Fixture f;
    const auto request = f.start();
    f.receipt(request);
    f.model.service().publish(f.iid(), Value{true}, 17, 18);
    f.pump(19);
    check(pop(f.transport).errors.has_value() && f.reading(19).value == Value{true},
          "equal-date conflicting confirmation overwrote observation");
  }
  {
    auto config = camera(false);
    auto state = std::make_shared<v::CameraStateSnapshot>();
    const auto generation = state->begin(ns(10));
    std::uint64_t clock = ns(14);
    auto provider = v::camera_state_provider(v::simulated_provider(config), *config.camera, state,
                                             [&] { return clock; });
    v::ConfiguredModel model(config, provider);
    v::Transport transport(model.service());
    model.refresh(14);
    check(transport.receive(write(config.camera->camera_general_on_iid, Value{false}), 14),
          "default request not answered");
    ack(transport);
    check(pop(transport).error.has_value() && !state->pending_off(generation, ns(14)),
          "read-only default advertised/dispatched camera writes");
  }
  {
    Fixture f;
    check(f.transport.receive(write(f.iid(), Value{true}), 13), "unsupported On not answered");
    ack(f.transport);
    check(pop(f.transport).errors.has_value() && f.reading(13).value == Value{true},
          "unsupported camera activation changed current state");
  }
}
}  // namespace
int main() try {
  unregistered_operation_allocation_failure();
  camera_callback_storage_failure();
  model_wrapper_allocation_failure();
  request_crosses_service_tick();
  unrelated_off_does_not_confirm_crossed_request();
  camera_confirmation_time_failures();
  accepted_pending_completion();
  failure_and_retirement();
  independent_observations_and_default();
  batch_and_backpressure();
  std::cout << "Vehicle confirmation: production camera provider/model/transport pending, receipt, "
               "late/epoch/retirement, observations and read-only default passed\n";
} catch (const std::exception& e) {
  fail_allocation_after(-1);
  std::cerr << e.what() << '\n';
  return 1;
}
