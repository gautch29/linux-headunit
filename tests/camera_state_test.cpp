// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/camera_state.hpp>
#include <headunit/theme/vehicle_transport.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace v = headunit::theme::vehicle;
namespace o = headunit::theme::opack;
namespace {
void check(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}
void rejects(auto operation) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid camera state accepted");
}
constexpr std::uint64_t ms(std::uint64_t n) {
  return n * 1000000;
}
const char* profile = R"({"version":1,"experimental":true,"stream_identifier":"camera",
  "general":{"on":{"binding":"camera.visible","max_age_ms":151,"writable":false,"initial":false}}})";
class Base final : public v::Provider {
 public:
  unsigned reads = 0, writes = 0;
  std::optional<v::Sample> read(std::string_view key, std::uint64_t now) override {
    check(key == "other", "camera state leaked to base provider");
    ++reads;
    return v::Sample{o::Value{std::int64_t(9)}, now};
  }
  o::Value apply(std::string_view key, const o::Value& value, std::uint64_t) override {
    check(key == "other", "camera write leaked to base provider");
    ++writes;
    return value;
  }
};
void paired(v::ConfiguredModel& model, const v::CameraCapability& c, std::uint64_t now,
            bool visible) {
  const std::array<std::int64_t, 2> ids{c.input_stream_on_iid, c.camera_general_on_iid};
  for (const auto& [iid, reading] : model.service().snapshot(ids, now)) {
    (void)iid;
    if (reading.error || reading.value != o::Value{visible})
      throw std::runtime_error("camera aliases differ at ms=" + std::to_string(now) +
                               " expected=" + std::to_string(visible));
  }
}
}  // namespace
int main() try {
  const auto config = v::parse_configuration("", "", profile);
  const auto c = *config.camera;
  {
    // A real USB acquisition can predate several OFF reads while in flight.
    // Neither those reads nor repeated unavailable owner turns are activation
    // events. Preserve the original acquisition instead of requiring recapture
    // after every read (which starves a delayed physical producer forever).
    v::CameraStateSnapshot state;
    const auto g = state.begin(ms(10));
    for (unsigned at = 11; at <= 80; ++at) {
      state.observe(g, false, 0, ms(at));
      check(state.read(at, 151).value == o::Value{false}, "waiting camera became On");
    }
    check(!state.observe(g, true, ms(9), ms(81)), "pre-generation capture bypassed cutoff");
    check(state.observe(g, true, ms(30), ms(82)) && state.read(82, 151).value == o::Value{true} &&
              state.read(82, 151).sampled_ms == 30,
          "negative polls starved delayed physical acquisition");
    state.observe(g, false, 0, ms(90));  // Actual visible-to-Off transition is dated.
    check(!state.observe(g, true, ms(89), ms(100)), "old frame revived real Off transition");
    for (unsigned at = 101; at <= 140; ++at)
      check(state.read(at, 151).value == o::Value{false}, "withdrawn camera returned On");
    check(state.observe(g, true, ms(110), ms(145)),
          "delayed post-Off acquisition could not recover");
    check(state.read(261, 151).value == o::Value{false}, "expired USB capture remained On");
    for (unsigned at = 262; at <= 280; ++at)
      state.read(at, 151);
    check(!state.observe(g, true, ms(260), ms(281)), "pre-expiry capture revived after withdrawal");
    check(state.observe(g, true, ms(270), ms(290)), "polling starved post-expiry recovery");
    auto owner = std::make_shared<v::CameraStateSnapshot>();
    const auto generation = owner->begin(ms(300));
    auto source = v::camera_state_provider(std::make_shared<Base>(), c, owner);
    v::ConfiguredModel model(config, source);
    for (unsigned at = 301; at <= 380; ++at)
      model.refresh(at);
    check(owner->observe(generation, true, ms(330), ms(382)),
          "delayed model capture rejected by owner");
    model.refresh(382);
    paired(model, c, 382, true);
    model.refresh(400);  // Same acquisition is immutable, not a heartbeat.
    paired(model, c, 400, true);
    model.refresh(481);
    paired(model, c, 481, false);
    for (unsigned at = 482; at <= 510; ++at)
      model.refresh(at);
    check(owner->observe(generation, true, ms(490), ms(515)), "delayed model recovery rejected");
    model.refresh(515);
    paired(model, c, 515, true);
    owner->observe(generation, false, 0, ms(520));
    model.refresh(520);
    paired(model, c, 520, false);
    check(!owner->observe(generation, true, ms(490), ms(525)),
          "old acquisition bypassed model withdrawal");
    model.refresh(525);
    paired(model, c, 525, false);
  }
  {
    auto state = std::make_shared<v::CameraStateSnapshot>();
    auto provider = v::camera_state_provider(std::make_shared<Base>(), c, state);
    v::ConfiguredModel model(config, provider);
    check(!state->request_off(ms(1), ms(10)), "missing owner accepted OFF");
    const auto g = state->begin(ms(10));
    state->observe(g, true, ms(12), ms(13));
    model.refresh(13);
    paired(model, c, 13, true);
    const auto request = state->request_off(ms(14), ms(100));
    check(bool(request), "OFF request rejected");
    check(!state->request_off(ms(13), ms(99)), "reversed request clock accepted");
    check(!state->request_off(ms(15), ms(101)), "second request bypassed bounded slot");
    check(!state->pending_off(g + 1, ms(15)), "wrong owner acquired OFF");
    state->observe(g, false, 0, ms(16));
    model.refresh(16);
    paired(model, c, 16, false);
    check(state->off_result(g, request->sequence, ms(16))->status ==
              v::CameraStateSnapshot::OffStatus::pending,
          "eligibility false claimed applied OFF");
    check(state->off_for_composition(g, ms(16)) == 0, "unapplied request stamped a composition");
    check(!state->presented_off(g, request->sequence, 0, 0, 1, ms(16), ms(17), ms(18)),
          "unapplied OFF receipt accepted");
    check(state->owner_off(g, request->sequence, ms(19)), "owner OFF not accepted");
    check(!state->pending_off(g, ms(20)), "owner action could execute twice");
    check(state->off_for_composition(g, ms(20)) == request->sequence,
          "withdrawn composition not stamped");
    check(!state->presented_off(g + 1, request->sequence, 0, 0, 1, ms(20), ms(21), ms(22)),
          "wrong generation receipt accepted");
    check(!state->presented_off(g, request->sequence + 1, 0, 0, 1, ms(20), ms(21), ms(22)),
          "wrong request receipt accepted");
    check(!state->presented_off(g, request->sequence, 0, 1, 1, ms(20), ms(21), ms(22)),
          "old epoch receipt accepted");
    check(!state->presented_off(g, request->sequence, 0, 0, 1, ms(18), ms(21), ms(22)),
          "pre-OFF queued scene accepted");
    check(!state->presented_off(g, request->sequence, 0, 0, 0, ms(20), ms(21), ms(22)),
          "bootstrap receipt accepted");
    check(!state->presented_off(g, request->sequence, 0, 0, 1, ms(20), ms(19), ms(22)),
          "pre-submit receipt accepted");
    check(!state->presented_off(g, request->sequence, 0, 0, 1, ms(20), ms(21), ms(20)),
          "future presentation accepted");
    check(state->presented_off(g, request->sequence, 0, 0, 1, ms(20), ms(21), ms(22)),
          "genuine OFF receipt not accepted");
    const auto result = state->off_result(g, request->sequence, ms(23));
    check(result && result->status == v::CameraStateSnapshot::OffStatus::applied &&
              result->withdrawn_ns == ms(19) && result->presented_ns == ms(21),
          "real OFF dates lost");
    check(!state->presented_off(g, request->sequence, 0, 0, 1, ms(20), ms(21), ms(23)),
          "receipt completed twice");
    model.refresh(23);
    paired(model, c, 23, false);
    rejects([&] { provider->apply(c.binding, o::Value{false}, 23); });
  }
  {
    v::CameraStateSnapshot state;
    const auto g = state.begin(ms(10));
    check(!state.request_off(ms(11), ms(11)) &&
              !state.request_off(ms(11), ms(11) + v::CameraStateSnapshot::maximum_off_wait_ns + 1),
          "invalid deadline accepted");
    const auto a = *state.request_off(ms(12), ms(200));
    check(state.read(199, 151).value == o::Value{false}, "stale fixture not false");
    check(state.off_result(g, a.sequence, ms(199))->status ==
              v::CameraStateSnapshot::OffStatus::pending,
          "missing/stale false completed OFF");
    check(!state.pending_off(g, ms(200)) && state.off_result(g, a.sequence, ms(200))->status ==
                                                v::CameraStateSnapshot::OffStatus::timed_out,
          "deadline not bounded");
    const auto b = *state.request_off(ms(201), ms(230));
    state.owner_off(g, b.sequence, ms(202));
    check(!state.presented_off(g, a.sequence, 1, 1, 2, ms(14), ms(15), ms(16)) &&
              state.off_result(g, b.sequence, ms(203))->status ==
                  v::CameraStateSnapshot::OffStatus::pending,
          "late old identity retired replacement request");
    state.cancel_off(g);
    check(!state.presented_off(g, b.sequence, 1, 1, 2, ms(203), ms(204), ms(205)),
          "cancelled OFF completed");
    const auto d = *state.request_off(ms(206), ms(240));
    state.owner_off(g, d.sequence, ms(207));
    state.retire(g, ms(208));
    check(state.off_result(g, d.sequence, ms(209))->status ==
              v::CameraStateSnapshot::OffStatus::retired,
          "retirement lost request");
    const auto next = state.begin(ms(210));
    check(!state.presented_off(g, d.sequence, 1, 1, 3, ms(211), ms(212), ms(213)),
          "retired receipt reached replacement");
    const auto e = *state.request_off(ms(214), ms(250));
    check(!state.off_result(next, d.sequence, ms(215)), "old request identity reused");
    state.close(ms(216));
    check(state.off_result(next, e.sequence, ms(217))->status ==
              v::CameraStateSnapshot::OffStatus::retired,
          "close failed to retire OFF");
    check(!state.request_off(ms(218), ms(250)), "closed owner accepted OFF");
  }
  {
    v::CameraStateSnapshot state;
    const auto g = state.begin(ms(10));
    const auto request = *state.request_off(ms(11), ms(20));
    state.owner_off(g, request.sequence, ms(12));
    check(!state.presented_off(g, request.sequence, 1, 1, 1, ms(13), ms(14), ms(20)) &&
              state.off_result(g, request.sequence, ms(20))->status ==
                  v::CameraStateSnapshot::OffStatus::timed_out,
          "late receipt bypassed request deadline");
  }
  {
    auto base = std::make_shared<Base>();
    auto state = std::make_shared<v::CameraStateSnapshot>();
    auto provider = v::camera_state_provider(base, c, state);
    v::ConfiguredModel model(config, provider);
    model.refresh(0);
    paired(model, c, 0, false);
    const auto g = state->begin(ms(10));
    model.refresh(10);
    paired(model, c, 10, false);
    check(state->observe(g, true, ms(20) + 500000, ms(20) + 900000), "fresh capture rejected");
    model.refresh(20);
    paired(model, c, 20, true);
    check(state->observe(g, false, 0, ms(20) + 950000), "same-ms OFF rejected");
    model.refresh(20);
    paired(model, c, 20, false);
    check(!state->observe(g, true, ms(20) + 500000, ms(21)), "old capture revived after OFF");
    check(state->observe(g, true, ms(22), ms(23)), "fresh capture did not recover");
    model.refresh(23);
    paired(model, c, 23, true);
    check(state->observe(g, true, ms(22), ms(100)), "source heartbeat rejected");
    const auto held = provider->read(c.binding, 100);
    check(held && held->sampled_ms == 22, "heartbeat renamed capture time");
    model.refresh(100);
    paired(model, c, 100, true);
    model.refresh(173);
    paired(model, c, 173, false);
    check(!state->observe(g, true, ms(23), ms(174)), "late old frame revived expired state");
    check(state->observe(g, true, ms(175), ms(176)),
          "fresh capture could not recover after expiry");
    model.refresh(176);
    paired(model, c, 176, true);
    state->retire(g, ms(177));
    model.refresh(177);
    paired(model, c, 177, false);
    check(!state->observe(g, true, ms(178), ms(179)), "retired owner revived state");
    const auto next = state->begin(ms(180));
    check(!state->observe(g, false, 0, ms(181)), "old owner cleared replacement");
    check(state->observe(next, true, ms(182), ms(183)), "replacement did not publish");
    model.refresh(183);
    paired(model, c, 183, true);
    state->retire(g, ms(184));
    model.refresh(184);
    paired(model, c, 184, true);
    state->close(ms(185));
    model.refresh(185);
    paired(model, c, 185, false);
    rejects([&] { state->begin(ms(186)); });
    rejects([&] { provider->apply(c.binding, o::Value{true}, 186); });
    check(base->reads == 0 && base->writes == 0, "camera state reached simulation");
    check(provider->read("other", 186)->value == o::Value{std::int64_t(9)}, "ordinary read lost");
    check(provider->apply("other", o::Value{true}, 186) == o::Value{true} && base->writes == 1,
          "ordinary write lost");
  }
  {
    v::CameraStateSnapshot state;
    const auto g = state.begin(ms(10));
    check(!state.observe(g, true, ms(12), ms(11)), "future capture accepted");
    check(state.read(11, 151).value == o::Value{false}, "future capture enabled camera");
    check(state.observe(g, true, ms(13), ms(14)), "future failure prevented valid recovery");
    check(!state.observe(g, true, ms(15), ms(13)), "reversed owner clock accepted");
    check(state.read(14, 151).value == o::Value{false}, "clock failure stayed visible");
    check(!state.observe(g, true, ms(16), ms(17)), "failed owner resumed without new generation");
    rejects([&] { state.begin(ms(13)); });
    rejects([&] { state.read(14, 0); });
  }
  {
    v::CameraStateSnapshot state;
    const auto g = state.begin(ms(10));
    state.observe(g, true, ms(12), ms(13));
    check(state.read(11, 151).value == o::Value{false}, "raced future owner enabled earlier read");
    check(state.read(13, 151).value == o::Value{true},
          "raced newer observation was permanently discarded");
  }
  {
    // A worker may post between sampling loop time and reading the mailbox.
    // That earlier read stays false, but is not an owner OFF transition. The
    // genuinely newer owner observation becomes eligible on the next turn.
    auto state = std::make_shared<v::CameraStateSnapshot>();
    auto provider = v::camera_state_provider(std::make_shared<Base>(), c, state);
    v::ConfiguredModel model(config, provider);
    model.refresh(0);
    const auto g = state->begin(ms(10));
    model.refresh(10);
    state->observe(g, true, ms(12), ms(14));
    model.refresh(13);
    paired(model, c, 13, false);
    model.refresh(14);
    paired(model, c, 14, true);
    state->observe(g, false, 0, ms(14) + 500000);
    check(!state->observe(g, true, ms(12), ms(15)), "old raced capture bypassed actual Off");
    model.refresh(15);
    paired(model, c, 15, false);
    state->observe(g, true, ms(15) + 500000, ms(16));
    model.refresh(16);
    paired(model, c, 16, true);
  }
  {
    // Real concurrent producer/consumer with one bounded record, not a queue.
    v::CameraStateSnapshot state;
    const auto g = state.begin(0);
    std::thread producer([&] {
      for (std::uint64_t n = 1; n < 20000; ++n)
        (void)state.observe(g, true, ms(n), ms(n));
    });
    for (unsigned n = 0; n < 20000; ++n)
      (void)state.read(0, 151);
    producer.join();
    check(state.read(19999, 151).value == o::Value{true}, "concurrent state was corrupted");
    state.retire(g, ms(20000));
    check(state.read(20000, 151).value == o::Value{false}, "concurrent owner did not retire");
  }
  {
    // Copied OFF dates preserve source ordering and allow a same-ms transition.
    auto state = std::make_shared<v::CameraStateSnapshot>();
    auto provider = v::camera_state_provider(std::make_shared<Base>(), c, state);
    v::ConfiguredModel model(config, provider);
    model.refresh(0);
    auto& service = model.service();
    service.publish(c.input_stream_on_iid, o::Value{true}, 20, 20);
    service.deactivate_boolean(c.input_stream_on_iid, 20, 21);
    rejects([&] { service.deactivate_boolean(c.input_stream_on_iid, 19, 22); });
    rejects([&] { service.deactivate_boolean(c.input_stream_on_iid, 23, 22); });
    const std::array<std::int64_t, 1> ids{c.input_stream_on_iid};
    check(service.snapshot(ids, 21).at(ids[0]).value == o::Value{false},
          "rejected OFF changed service time/state");
  }
  {
    // Evidence follows authenticated requests and actual transport acceptance,
    // never local provider reads, queued notifications or a blocked retry.
    auto state = std::make_shared<v::CameraStateSnapshot>();
    auto provider = v::camera_state_provider(std::make_shared<Base>(), c, state);
    v::ConfiguredModel model(config, provider);
    model.refresh(0);
    auto trace = std::make_shared<v::ResourceTrace>(
        std::array{c.input_stream_on_iid, c.camera_general_on_iid});
    v::Transport normal(model.service(), {}, trace), high(model.service(), {}, trace);
    const auto message = [&](std::string name, o::Value values, std::int64_t plugin = 0) {
      headunit::DataMessage m;
      m.kind = headunit::DataMessageKind::synchronous;
      m.type = 0x636f6d6d;
      m.payload = v::encode_outgoing({std::move(name), 1, std::move(values), {}, {}}, plugin);
      return m;
    };
    const auto drain = [](v::Transport& port) {
      while (port.pending())
        port.sent();
    };
    check(normal.receive(message("registerRequest", o::Value{std::string("*")}), 0),
          "wildcard registration blocked");
    check(trace->counts()[0].registrations == 1 && trace->counts()[1].registrations == 1,
          "camera aliases missing from accepted wildcard registration");
    check(trace->counts()[0].responses_off == 0, "queueing a response counted as transfer");
    normal.sent();
    check(trace->counts()[0].responses_off == 0, "transport ACK counted as Boolean response");
    normal.sent();
    check(trace->counts()[0].responses_off == 1 && trace->counts()[1].responses_off == 1,
          "initial paired Off response transfer not traced");
    const auto g = state->begin(ms(10));
    state->observe(g, true, ms(12), ms(13));
    model.refresh(13);
    const auto on = model.service().updates(13);
    check(on && normal.notify(*on), "paired On notification missing");
    check(trace->counts()[0].updates_on == 0, "queueing On claimed transport acceptance");
    normal.sent();
    check(trace->counts()[0].updates_on == 1 && trace->counts()[1].updates_on == 1,
          "paired On notification transfer lost");
    const o::Value input{
        o::Array{o::Value{c.input_stream_on_iid}, o::Value{c.input_stream_on_iid}}};
    check(high.receive(message("readRequest", input), 13), "camera read blocked");
    drain(high);
    check(trace->counts()[0].reads == 1 && trace->counts()[1].reads == 0 &&
              trace->counts()[0].responses_on == 1,
          "camera read trace duplicated IIDs or included another resource");
    const auto counts = trace->counts();
    check(high.receive(message("registerRequest", o::Value{std::int64_t(99)}), 13),
          "malformed fixture blocked");
    drain(high);
    check(high.receive(message("readRequest", input, 1), 13), "foreign plugin fixture blocked");
    drain(high);
    check(trace->counts() == counts, "rejected request fabricated resource evidence");
    state->observe(g, false, 0, ms(14));
    model.refresh(14);
    const auto off = model.service().updates(14);
    check(bool(off), "paired Off notification missing");
    for (unsigned n = 0; n < 32; ++n)
      check(normal.notify(*off), "bounded notification queue filled early");
    check(!normal.notify(*off) && !normal.receive(message("readRequest", input), 14),
          "backpressure fixture did not block");
    check(trace->counts() == counts, "blocked retry or queued Off fabricated evidence");
    drain(normal);
    check(trace->counts()[0].updates_off == 32 && trace->counts()[1].updates_off == 32,
          "transferred Off messages not counted exactly");
    check(high.receive(message("unregisterRequest", o::Value{std::string("*")}), 14),
          "wildcard unregister blocked");
    drain(high);
    check(trace->counts()[0].unregistrations == 1 && trace->counts()[1].unregistrations == 1,
          "accepted alias unregister missing");
    rejects([&] {
      v::ResourceTrace invalid(std::array{c.input_stream_on_iid, c.input_stream_on_iid});
    });
  }
  std::cout << "Camera phone state: shared original capture dates, prompt paired OFF, expiry, "
               "generation retirement and concurrent ownership passed\n";
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
